#pragma once
// Port-only (132vc2, research/VITA.md "Main-thread CPU (132vc2)"): a small-object allocator for the PS Vita's C++
// heap (vita_main.cpp's operator new). The game's frame makes about 3,000 heap allocations (std::vector growth,
// std::function, strings), each one newlib's malloc behind its single lock plus the heap profile's atomics, shared by
// the 3 cores. Blocks of up to max_size bytes come instead from per-thread free lists refilled in batches from
// size-class lists (one std::mutex each, taken once per batch), carved from 64 KB chunks that hold one class each.
// Larger blocks (and everything once `limit` bytes of chunks are held) take the caller's fallback.
//
// It only changes where a block lives, never its contents: the program computes the same values (no code may depend
// on heap addresses; nothing in the port does). Freed small blocks return to their class, not to malloc, so the
// chunks stay held (bounded by the peak of live small blocks). Portable C++ (std::mutex, thread_local) so the PC
// tests run the same code (tests/small_alloc_tests.cpp).
#include <cstddef>
#include <cstdint>

namespace omega::small_alloc {

constexpr std::size_t max_size = 1024;        // larger requests: nullptr from allocate (the caller's fallback)
constexpr std::size_t chunk_bytes = 64 * 1024;

// Sets the most chunk memory held (default 0: allocate always gives nullptr). Calls racing with this one may observe
// either limit and safely fall back; the Vita configures it once during startup.
void configure(std::size_t limit_bytes);
// A block of at least `size` bytes (1..max_size, 16-byte aligned), or nullptr (too large, not configured, or the limit
// reached): the caller then uses its own allocator.
void* allocate(std::size_t size) noexcept;
// True when `p` (non-null) is a block of allocate's; then release(p) frees it.
bool owns(const void* p) noexcept;
void release(void* p) noexcept;
// The usable size of an owned block (its class size).
std::size_t block_size(const void* p) noexcept;

struct Stats {
    std::size_t chunk_bytes_held = 0;   // chunks carved so far (never returned)
    std::uint64_t refills = 0;          // batches taken from the class lists
    std::uint64_t flushes = 0;          // batches given back to them
    std::uint64_t refused = 0;          // small requests refused at the limit
};
Stats stats() noexcept;

}  // namespace omega::small_alloc
