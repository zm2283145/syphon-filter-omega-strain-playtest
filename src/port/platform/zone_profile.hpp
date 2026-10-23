#pragma once
// PORT-ONLY (checkpoint 132vt1, research/VITA.md "Profiling"): coarse scoped timers for finding the handheld's hot
// spots. OMEGA_ZONE("name") times the rest of the enclosing scope (inclusive of nested zones) into a static slot;
// the Vita app prints the slots every 120 frames and resets them. Without OMEGA_ZONE_PROFILE (every Windows build)
// the macro is an empty statement, so the shared code is unchanged. Zones belong on the simulation thread only (the
// counters are not atomic).

#if defined(OMEGA_ZONE_PROFILE)
#include <atomic>
#include <chrono>
#include <cstdint>

namespace omega::zone {

struct Slot {
    const char* name;
    std::uint64_t ns = 0;
    std::uint32_t calls = 0;
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

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Scope {
    Slot& slot;
    std::uint64_t t0;
    explicit Scope(Slot& s) : slot(s), t0(now_ns()) {}
    ~Scope() {
        slot.ns += now_ns() - t0;
        ++slot.calls;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace omega::zone

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
#endif
