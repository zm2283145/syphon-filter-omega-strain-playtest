// Port-only (132vc2): see small_alloc.hpp.
#include "small_alloc.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#if defined(_MSC_VER) || defined(__vita__)
#include <malloc.h>
#endif

namespace omega::small_alloc {

namespace {

constexpr std::array<std::uint16_t, 20> kSizes{16,  32,  48,  64,  80,  96,  112, 128, 160, 192,
                                              224, 256, 320, 384, 448, 512, 640, 768, 896, 1024};
constexpr std::size_t kClasses = kSizes.size();

constexpr std::array<std::uint8_t, max_size / 16 + 1> make_index() {
    std::array<std::uint8_t, max_size / 16 + 1> r{};
    for (std::size_t q = 0; q < r.size(); ++q) {
        std::size_t c = 0;
        while (kSizes[c] < q * 16) ++c;
        r[q] = static_cast<std::uint8_t>(c);
    }
    return r;
}
constexpr auto kIndex = make_index();  // (size + 15) / 16 -> class

constexpr std::uint32_t batch_of(std::size_t c) {
    const std::size_t n = 4096 / kSizes[c];
    return static_cast<std::uint32_t>(n < 8 ? 8 : (n > 64 ? 64 : n));
}

constexpr std::size_t kSuperBytes = 1024 * 1024;  // chunks are carved from 1 MB blocks (16 chunks)
constexpr std::size_t kTable = 8192;              // chunk table slots (open addressing): 512 MB of chunks at most

struct Block {
    Block* next;
};

struct ClassList {
    std::mutex m;
    Block* free = nullptr;
    std::size_t count = 0;
    char* carve = nullptr;
    char* carve_end = nullptr;
};

struct Global {
    std::array<ClassList, kClasses> classes;
    std::mutex chunks_m;
    char* super = nullptr;
    char* super_end = nullptr;
    std::atomic<std::size_t> limit{0};
    std::atomic<std::size_t> held{0};
    std::atomic<std::uint64_t> refills{0}, flushes{0}, refused{0};
    std::array<std::atomic<std::uintptr_t>, kTable> keys{};  // chunk base (0: empty)
    std::array<std::uint8_t, kTable> cls{};
};

Global& g() {
    static Global* global = [] {
        void* raw = std::malloc(sizeof(Global));
        return new (raw) Global();  // placement: never through operator new, never destroyed
    }();
    return *global;
}

std::size_t slot_of(std::uintptr_t base) {
    std::uint64_t h = static_cast<std::uint64_t>(base >> 16) * 0x9E3779B97F4A7C15ull;
    return static_cast<std::size_t>(h >> 40) % kTable;
}

int class_of_chunk(std::uintptr_t base) {
    Global& G = g();
    std::size_t s = slot_of(base);
    for (std::size_t n = 0; n < kTable; ++n, s = (s + 1) % kTable) {
        const std::uintptr_t k = G.keys[s].load(std::memory_order_acquire);
        if (k == base) return G.cls[s];
        if (k == 0) return -1;
    }
    return -1;
}

void* aligned_block(std::size_t bytes) {
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, chunk_bytes);
#elif defined(__vita__)
    return memalign(chunk_bytes, bytes);
#else
    return std::aligned_alloc(chunk_bytes, bytes);
#endif
}

// A new chunk for class c, registered in the table; nullptr at the limit. Called with the class lock held.
char* new_chunk(std::size_t c) {
    Global& G = g();
    std::lock_guard lock(G.chunks_m);
    const std::size_t held = G.held.load(std::memory_order_relaxed);
    const std::size_t limit = G.limit.load(std::memory_order_acquire);
    if (held + chunk_bytes > limit) return nullptr;
    if (G.super == G.super_end) {
        const std::size_t want = limit - held < kSuperBytes ? chunk_bytes : kSuperBytes;
        auto* p = static_cast<char*>(aligned_block(want));
        if (!p) return nullptr;
        G.super = p;
        G.super_end = p + want;
    }
    char* chunk = G.super;
    G.super += chunk_bytes;
    const auto base = reinterpret_cast<std::uintptr_t>(chunk);
    std::size_t s = slot_of(base);
    for (std::size_t n = 0;; ++n, s = (s + 1) % kTable) {
        if (n == kTable) return nullptr;  // the table is full: no more chunks (the chunk is simply not used)
        if (G.keys[s].load(std::memory_order_relaxed) == 0) break;
    }
    G.cls[s] = static_cast<std::uint8_t>(c);
    G.keys[s].store(base, std::memory_order_release);
    G.held.fetch_add(chunk_bytes, std::memory_order_relaxed);
    return chunk;
}

struct Cache {
    Block* head[kClasses]{};
    std::uint32_t count[kClasses]{};
    ~Cache();
};
thread_local Cache t_cache;

// Fills the thread's empty class-c list with up to one batch; false when nothing could be had.
bool refill(std::size_t c, Cache& cache) {
    Global& G = g();
    ClassList& L = G.classes[c];
    const std::uint32_t want = batch_of(c);
    const std::size_t size = kSizes[c];
    Block* head = nullptr;
    std::uint32_t got = 0;
    {
        std::lock_guard lock(L.m);
        while (got < want && L.free) {
            Block* b = L.free;
            L.free = b->next;
            b->next = head;
            head = b;
            ++got;
        }
        L.count -= got;
        while (got < want) {
            if (L.carve + size > L.carve_end) {
                char* chunk = new_chunk(c);
                if (!chunk) break;
                L.carve = chunk;
                L.carve_end = chunk + (chunk_bytes / size) * size;
            }
            auto* b = reinterpret_cast<Block*>(L.carve);
            L.carve += size;
            b->next = head;
            head = b;
            ++got;
        }
    }
    if (!got) return false;
    G.refills.fetch_add(1, std::memory_order_relaxed);
    cache.head[c] = head;
    cache.count[c] = got;
    return true;
}

void flush_n(std::size_t c, Cache& cache, std::uint32_t n) {
    Global& G = g();
    Block* first = cache.head[c];
    Block* last = first;
    for (std::uint32_t k = 1; k < n; ++k) last = last->next;
    cache.head[c] = last->next;
    cache.count[c] -= n;
    ClassList& L = G.classes[c];
    {
        std::lock_guard lock(L.m);
        last->next = L.free;
        L.free = first;
        L.count += n;
    }
    G.flushes.fetch_add(1, std::memory_order_relaxed);
}

void flush(std::size_t c, Cache& cache) { flush_n(c, cache, batch_of(c)); }

Cache::~Cache() {
    for (std::size_t c = 0; c < kClasses; ++c)
        if (count[c]) flush_n(c, *this, count[c]);
}

}  // namespace

void configure(std::size_t limit_bytes) { g().limit.store(limit_bytes, std::memory_order_release); }

void* allocate(std::size_t size) noexcept {
    if (size > max_size) return nullptr;
    const std::size_t c = kIndex[(size + 15) >> 4];
    Cache& cache = t_cache;
    if (!cache.head[c] && !refill(c, cache)) {
        g().refused.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    Block* b = cache.head[c];
    cache.head[c] = b->next;
    --cache.count[c];
    return b;
}

bool owns(const void* p) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(chunk_bytes - 1);
    return class_of_chunk(base) >= 0;
}

std::size_t block_size(const void* p) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(chunk_bytes - 1);
    const int c = class_of_chunk(base);
    return c < 0 ? 0 : kSizes[static_cast<std::size_t>(c)];
}

void release(void* p) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t(chunk_bytes - 1);
    const int ci = class_of_chunk(base);
    if (ci < 0) return;  // not ours (owns() is the caller's test)
    const auto c = static_cast<std::size_t>(ci);
    Cache& cache = t_cache;
    auto* b = static_cast<Block*>(p);
    b->next = cache.head[c];
    cache.head[c] = b;
    if (++cache.count[c] > 2 * batch_of(c)) flush(c, cache);
}

Stats stats() noexcept {
    Global& G = g();
    Stats s;
    s.chunk_bytes_held = G.held.load(std::memory_order_relaxed);
    s.refills = G.refills.load(std::memory_order_relaxed);
    s.flushes = G.flushes.load(std::memory_order_relaxed);
    s.refused = G.refused.load(std::memory_order_relaxed);
    return s;
}

}  // namespace omega::small_alloc
