// Port-only (132ps5d, research/PS5.md "Heap"): the title's malloc heap. The link wraps malloc, calloc, realloc, free,
// posix_memalign and malloc_usable_size (cmake/ps5/OmegaPs5Targets.cmake), so the game, libc++ and the OpenGL runtime
// all allocate here: one sceLibc mspace over 1 GiB of CPU-cached direct memory, mapped once at the first allocation
// and never unmapped (late static destructors still free into it). The system C library's own heap is small (256 MiB in
// the process parameters, tools/ps5/fetch_toolchain.sh) and flexible memory holds only a few hundred MiB, while a level
// wants about 0.5 GB (the PC's heap); 132ps5c ran with the same 1 GiB through the former ps5-opengl SDK's allocator
// (hw2: 2 GiB faulted in the first allocations). When the mspace is full or cannot be made, allocations continue in the
// system heap; free and realloc route by address, so blocks of both heaps can be live.
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
void* __real_malloc(std::size_t size);
void* __real_calloc(std::size_t count, std::size_t size);
void* __real_realloc(void* address, std::size_t size);
void __real_free(void* address);
int __real_posix_memalign(void** address, std::size_t alignment, std::size_t size);
std::size_t __real_malloc_usable_size(const void* address);

void* sceLibcMspaceCreate(const char* name, void* base, std::size_t size, unsigned flags);
void* sceLibcMspaceMalloc(void* mspace, std::size_t size);
void* sceLibcMspaceCalloc(void* mspace, std::size_t count, std::size_t size);
void* sceLibcMspaceRealloc(void* mspace, void* address, std::size_t size);
void sceLibcMspaceFree(void* mspace, void* address);
int sceLibcMspacePosixMemalign(void* mspace, void** address, std::size_t alignment, std::size_t size);
std::size_t sceLibcMspaceMallocUsableSize(const void* address);
std::int64_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end, std::size_t length,
                                  std::size_t alignment, int memory_type, std::int64_t* physical_start);
int sceKernelMapDirectMemory(void** address, std::size_t length, int protection, int flags, std::int64_t physical_start,
                             std::size_t alignment);
}

namespace {

constexpr std::size_t heap_request = std::size_t(1024) << 20;
constexpr std::size_t heap_alignment = std::size_t(2) << 20;
constexpr int memory_type_cpu_cached = 12;  // the GL driver's own CPU-side memory type (as in 132ps5c's heap)

std::atomic<int> g_state{0};  // 0 not made, 1 being made, 2 ready, -1 failed (the system heap only)
void* g_base = nullptr;
std::size_t g_size = 0;
void* g_mspace = nullptr;
std::atomic<std::size_t> g_live{0};

bool heap_ready() {
    int state = g_state.load(std::memory_order_acquire);
    if (state == 2) return true;
    if (state != 0) return false;
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return expected == 2;
    void* base = nullptr;
    std::size_t size = 0;
    std::int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), heap_request, heap_alignment,
                                      memory_type_cpu_cached, &physical) == 0 &&
        sceKernelMapDirectMemory(&base, heap_request, PROT_READ | PROT_WRITE, 0, physical, heap_alignment) == 0 && base) {
        size = heap_request;
    } else {
        base = nullptr;
        // Flexible memory, halving down to 128 MiB.
        for (std::size_t s = heap_request; !base && s >= (std::size_t(128) << 20); s /= 2) {
            void* m = mmap(nullptr, s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (m != MAP_FAILED) base = m, size = s;
        }
    }
    void* mspace = base ? sceLibcMspaceCreate("omega", base, size, 0) : nullptr;
    if (!mspace) {
        g_state.store(-1, std::memory_order_release);
        return false;
    }
    g_base = base;
    g_size = size;
    g_mspace = mspace;
    g_state.store(2, std::memory_order_release);
    return true;
}

bool owns(const void* address) {
    if (g_state.load(std::memory_order_acquire) != 2) return false;
    const auto a = reinterpret_cast<std::uintptr_t>(address), b = reinterpret_cast<std::uintptr_t>(g_base);
    return a >= b && a - b < g_size;
}

void* counted(void* address) {
    if (address) g_live.fetch_add(sceLibcMspaceMallocUsableSize(address), std::memory_order_relaxed);
    return address;
}

}  // namespace

extern "C" {

void* __wrap_malloc(std::size_t size) {
    void* p = heap_ready() ? counted(sceLibcMspaceMalloc(g_mspace, size)) : nullptr;
    return p ? p : __real_malloc(size);
}

void* __wrap_calloc(std::size_t count, std::size_t size) {
    void* p = heap_ready() ? counted(sceLibcMspaceCalloc(g_mspace, count, size)) : nullptr;
    return p ? p : __real_calloc(count, size);
}

void __wrap_free(void* address) {
    if (!owns(address)) {
        __real_free(address);
        return;
    }
    g_live.fetch_sub(sceLibcMspaceMallocUsableSize(address), std::memory_order_relaxed);
    sceLibcMspaceFree(g_mspace, address);
}

void* __wrap_realloc(void* address, std::size_t size) {
    if (!address) return __wrap_malloc(size);
    if (!owns(address)) return __real_realloc(address, size);
    const std::size_t before = sceLibcMspaceMallocUsableSize(address);
    if (size == 0) {
        __wrap_free(address);
        return nullptr;
    }
    if (void* p = sceLibcMspaceRealloc(g_mspace, address, size)) {
        g_live.fetch_add(sceLibcMspaceMallocUsableSize(p), std::memory_order_relaxed);
        g_live.fetch_sub(before, std::memory_order_relaxed);
        return p;
    }
    // The mspace is full: move the block to the system heap (the original stays valid if that fails too).
    void* p = __real_malloc(size);
    if (p) {
        std::memcpy(p, address, before < size ? before : size);
        __wrap_free(address);
    }
    return p;
}

int __wrap_posix_memalign(void** address, std::size_t alignment, std::size_t size) {
    if (heap_ready()) {
        const int result = sceLibcMspacePosixMemalign(g_mspace, address, alignment, size);
        if (result == 0) {
            counted(*address);
            return 0;
        }
        if (result != ENOMEM) return result;
    }
    return __real_posix_memalign(address, alignment, size);
}

std::size_t __wrap_malloc_usable_size(const void* address) {
    return owns(address) ? sceLibcMspaceMallocUsableSize(address) : __real_malloc_usable_size(address);
}

// The heap's live bytes and size, for the renderer's statistics line (gfx_gl46_ps5.cpp).
std::size_t omega_ps5_heap_live_bytes() { return g_live.load(std::memory_order_relaxed); }
std::size_t omega_ps5_heap_size() { return g_state.load(std::memory_order_acquire) == 2 ? g_size : 0; }

}  // extern "C"
