#pragma once
// Port-only (checkpoint 132vt1, research/VITA.md "Threads and cores", research/THREADING.md): which core each of the
// port's threads runs on. The shared code's threads (the simulation thread, the render host threads, the worker pool,
// the audio output thread and, once THREADING.md step 4 lands, the background loader) name their role once at the top
// of their body; nothing else about them changes, so the simulation stays single-threaded and deterministic.
//
// PS Vita: the role pins the thread to one of the three user cores (sceKernelChangeThreadCpuAffinityMask), sets its
// priority and registers it for the per-thread busy-time report (thread_usage()):
//   simulation (+ mission scripts, the level load)  core 0
//   render (GXM submission, the loading screens)     core 1
//   worker (WorkerPool), loader                      core 2 (132vt2d: the workers may also run on core 1, below the
//                                                    render thread's priority, in its waits)
//   audio (SceAudioOut output)                       core 2, high priority
// Every other platform: enter_thread_role and ThreadBusyScope do nothing (the Windows build is unchanged).
#include <cstdint>
#include <string>
#include <vector>

namespace omega {

enum class ThreadRole : std::uint8_t { simulation, render, worker, loader, audio };

const char* thread_role_name(ThreadRole role);
// The user core (0..2) the role is pinned to on the Vita (a worker's home core; it may also run on core 1).
unsigned thread_role_core(ThreadRole role);

// Called once by a thread at the top of its body (name: for the report, e.g. "worker 1").
void enter_thread_role(ThreadRole role, const std::string& name);

// One registered thread's state; the times are totals since it registered (microseconds).
struct ThreadUsage {
    std::string name;
    ThreadRole role = ThreadRole::simulation;
    unsigned affinity = 0;       // the kernel's affinity mask read back after pinning (Vita: 0x10000 << core)
    int priority = 0;            // the kernel's current priority
    std::uint64_t busy_us = 0;   // time inside ThreadBusyScope (the thread's work, not its waits)
    std::uint64_t run_us = 0;    // the kernel's run clock (0 where the kernel or emulator does not keep it)
};
std::vector<ThreadUsage> thread_usage();

// Per user core idle time since boot (sceKernelGetSystemInfo cpuInfo[].idleClock, microseconds). False where the
// kernel does not provide it (other platforms; Vita3K leaves sceKernelGetSystemInfo unimplemented).
bool core_idle_us(std::uint64_t (&idle)[3]);

// Counts time measured elsewhere (e.g. a frame's CPU part, which ends inside a wait) as the calling thread's busy
// time (Vita only).
void count_busy_time(std::uint64_t us);

// Counts the time between construction and destruction as the calling thread's busy time (Vita only).
class ThreadBusyScope {
public:
#if defined(__vita__)
    ThreadBusyScope();
    ~ThreadBusyScope();

private:
    std::uint64_t start_;
#else
    ThreadBusyScope() {}
#endif
    ThreadBusyScope(const ThreadBusyScope&) = delete;
    ThreadBusyScope& operator=(const ThreadBusyScope&) = delete;
};

}  // namespace omega
