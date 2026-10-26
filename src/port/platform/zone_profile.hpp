#pragma once
// PORT-ONLY (checkpoint 132vt1, research/VITA.md "Profiling"): coarse scoped timers for finding the handheld's hot
// spots. OMEGA_ZONE("name") times the rest of the enclosing scope (inclusive of nested zones) into a static slot;
// the Vita app prints the slots every 120 frames and resets them. Without OMEGA_ZONE_PROFILE (every Windows build)
// the macro is an empty statement, so the shared code is unchanged. Zones belong on the simulation thread only (the
// counters are not atomic). OMEGA_ZONE_LAPS(laps) / OMEGA_ZONE_LAP(laps, "name") time consecutive stretches of
// one long scope (the play step's sections) without new scopes: each lap adds the time since the previous one. Each
// zone also counts the heap allocations its thread made inside it (132vs1; the Vita app's operator new count).

#if defined(OMEGA_ZONE_PROFILE)
#include <atomic>
#include <chrono>
#include <cstdint>
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

namespace omega::zone {

// Port-only (132vs1): the calling thread's heap allocations so far (the Vita app's operator new count), which each
// zone charges with its own; null (no counts) until the app sets it.
inline std::uint32_t (*thread_allocations)() = nullptr;
inline std::uint32_t allocations_now() { return thread_allocations ? thread_allocations() : 0U; }

struct Slot {
    const char* name;
    std::uint64_t ns = 0;
    std::uint32_t calls = 0;
    std::uint32_t allocs = 0;
    Slot* next = nullptr;

    explicit Slot(const char* n) : name(n) {
        next = head().load(std::memory_order_relaxed);
        while (!head().compare_exchange_weak(next, this, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
    static std::atomic<Slot*>& head() {
        static std::atomic<Slot*> h{nullptr};
        return h;
    }
};

#if defined(__vita__)
// 132vt2f: the hardware log of vt2e showed every zone costing ~10 us more than its body (a lap with nothing in it:
// 9.7 us), the price of std::chrono::steady_clock there. calibrate_clock() (the app's start) times both clocks and
// keeps the process clock (microseconds) when it is cheaper.
inline bool g_process_clock = false;
#endif

inline std::uint64_t now_ns() {
#if defined(__vita__)
    if (g_process_clock) return sceKernelGetProcessTimeWide() * 1000u;
#endif
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Scope {
    Slot& slot;
    std::uint64_t t0;
    std::uint32_t a0;
    explicit Scope(Slot& s) : slot(s), t0(now_ns()), a0(allocations_now()) {}
    ~Scope() {
        slot.ns += now_ns() - t0;
        slot.allocs += allocations_now() - a0;
        ++slot.calls;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

struct Laps {
    std::uint64_t t = now_ns();
    std::uint32_t a = allocations_now();
};

inline void lap(Laps& laps, Slot& slot) {
    const std::uint64_t now = now_ns();
    const std::uint32_t a = allocations_now();
    slot.ns += now - laps.t;
    slot.allocs += a - laps.a;
    ++slot.calls;
    laps.t = now;
    laps.a = a;
}

#if defined(__vita__)
// The cost of one read of each clock (ns, over 2000 reads); the process clock is kept when it is cheaper.
struct ClockCost {
    double steady_ns, process_ns;
};
inline ClockCost calibrate_clock() {
    constexpr int reads = 2000;
    volatile std::uint64_t sink = 0;
    const std::uint64_t p0 = sceKernelGetProcessTimeWide();
    for (int k = 0; k < reads; ++k)
        sink = sink + static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t p1 = sceKernelGetProcessTimeWide();
    for (int k = 0; k < reads; ++k) sink = sink + sceKernelGetProcessTimeWide();
    const std::uint64_t p2 = sceKernelGetProcessTimeWide();
    const ClockCost cost{double(p1 - p0) * 1000.0 / reads, double(p2 - p1) * 1000.0 / reads};
    g_process_clock = cost.process_ns < cost.steady_ns;
    return cost;
}
#endif

}  // namespace omega::zone

#define OMEGA_ZONE_LAPS(var) ::omega::zone::Laps var
#define OMEGA_ZONE_LAP(var, label)                    \
    do {                                              \
        static ::omega::zone::Slot omega_lap_{label}; \
        ::omega::zone::lap(var, omega_lap_);          \
    } while (false)
#define OMEGA_ZONE_CAT2(a, b) a##b
#define OMEGA_ZONE_CAT(a, b) OMEGA_ZONE_CAT2(a, b)
#define OMEGA_ZONE(label)                                                                \
    static ::omega::zone::Slot OMEGA_ZONE_CAT(omega_zone_slot_, __LINE__){label};       \
    ::omega::zone::Scope OMEGA_ZONE_CAT(omega_zone_scope_, __LINE__) {                  \
        OMEGA_ZONE_CAT(omega_zone_slot_, __LINE__)                                       \
    }
#else
#define OMEGA_ZONE(label) \
    do {                  \
    } while (false)
#define OMEGA_ZONE_LAPS(var) \
    do {                     \
    } while (false)
#define OMEGA_ZONE_LAP(var, label) \
    do {                           \
    } while (false)
#endif

// 132vt2f: the Vita GL backend's per-draw zones (about 13 clock reads a draw, about 1,000 draws a frame in a level)
// only with OMEGA_ZONE_PROFILE_FINE (CMake OMEGA_VITA_FINE_ZONES=ON); the default VPK keeps the coarse zones.
#if defined(OMEGA_ZONE_PROFILE_FINE)
#define OMEGA_ZONE_FINE(label) OMEGA_ZONE(label)
#define OMEGA_ZONE_FINE_LAPS(var) OMEGA_ZONE_LAPS(var)
#define OMEGA_ZONE_FINE_LAP(var, label) OMEGA_ZONE_LAP(var, label)
#else
#define OMEGA_ZONE_FINE(label) \
    do {                       \
    } while (false)
#define OMEGA_ZONE_FINE_LAPS(var) \
    do {                          \
    } while (false)
#define OMEGA_ZONE_FINE_LAP(var, label) \
    do {                                \
    } while (false)
#endif
