// Port-only (checkpoint 132vt1): thread roles, core pinning and busy-time accounting; see thread_roles.hpp.
#include "thread_roles.hpp"

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <cstdint>

#include <mutex>
#endif

namespace omega {

const char* thread_role_name(ThreadRole role) {
    switch (role) {
        case ThreadRole::simulation: return "simulation";
        case ThreadRole::render: return "render";
        case ThreadRole::worker: return "worker";
        case ThreadRole::loader: return "loader";
        case ThreadRole::audio: return "audio";
    }
    return "?";
}

unsigned thread_role_core(ThreadRole role) {
    switch (role) {
        case ThreadRole::simulation: return 0;
        case ThreadRole::render: return 1;
        case ThreadRole::worker:
        case ThreadRole::loader:
        case ThreadRole::audio: return 2;
    }
    return 0;
}

#if defined(__vita__)

namespace {

// Priorities: the Vita's user range is 64 (highest) .. 191 (lowest); the main thread starts at 0x10000100 (the
// "default" relative value, 160 here). Audio must not starve behind the worker sharing its core, so it runs higher.
int role_priority(ThreadRole role) {
    switch (role) {
        case ThreadRole::audio: return 64;
        case ThreadRole::render: return 96;
        case ThreadRole::simulation: return 128;
        case ThreadRole::worker:
        case ThreadRole::loader: return 144;
    }
    return 160;
}

struct Entry {
    SceUID id;
    ThreadUsage usage;
    bool gone = false;  // the thread has exited: its UID is never queried again
};

std::mutex g_mutex;
std::vector<Entry> g_threads;
thread_local std::size_t t_index = static_cast<std::size_t>(-1);

// A thread's exit (std::thread is a pthread here) marks its entry gone, so thread_usage() never asks the kernel about
// a deleted thread (Vita3K aborts on such queries some seconds later; a real kernel may have reused the UID).
pthread_key_t g_exit_key;
std::once_flag g_exit_once;
void mark_gone(void* slot) {
    const auto index = reinterpret_cast<std::uintptr_t>(slot) - 1;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < g_threads.size()) g_threads[index].gone = true;
}

}  // namespace

void enter_thread_role(ThreadRole role, const std::string& name) {
    std::call_once(g_exit_once, [] { pthread_key_create(&g_exit_key, mark_gone); });
    const SceUID id = sceKernelGetThreadId();
    const int mask = role == ThreadRole::worker
        ? (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)   // 132vt2d: also the render core's idle time
        : SCE_KERNEL_CPU_MASK_USER_0 << thread_role_core(role);
    const int set = sceKernelChangeThreadCpuAffinityMask(id, mask);
    const int prio = sceKernelChangeThreadPriority(id, role_priority(role));
    Entry e{};
    e.id = id;
    e.usage.name = name;
    e.usage.role = role;
    e.usage.affinity = static_cast<unsigned>(sceKernelGetThreadCpuAffinityMask(id));
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(id, &info) >= 0) e.usage.priority = info.currentPriority;
    if (set < 0 || prio < 0) e.usage.name += " (pinning failed)";
    std::lock_guard<std::mutex> lock(g_mutex);
    for (std::size_t i = 0; i < g_threads.size(); ++i)
        if (g_threads[i].id == id) {
            g_threads[i].usage.name = e.usage.name;
            g_threads[i].usage.role = role;
            g_threads[i].usage.affinity = e.usage.affinity;
            g_threads[i].usage.priority = e.usage.priority;
            g_threads[i].gone = false;
            t_index = i;
            pthread_setspecific(g_exit_key, reinterpret_cast<void*>(std::uintptr_t(i) + 1));
            return;
        }
    t_index = g_threads.size();
    g_threads.push_back(e);
    pthread_setspecific(g_exit_key, reinterpret_cast<void*>(std::uintptr_t(t_index) + 1));
}

std::vector<ThreadUsage> thread_usage() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<ThreadUsage> out;
    out.reserve(g_threads.size());
    for (Entry& e : g_threads) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (e.gone) {
        } else if (sceKernelGetThreadInfo(e.id, &info) >= 0) {
            e.usage.run_us = static_cast<std::uint64_t>(info.runClocks);
            e.usage.priority = info.currentPriority;
        } else {
            e.gone = true;
        }
        out.push_back(e.usage);
    }
    return out;
}

bool core_idle_us(std::uint64_t (&idle)[3]) {
    SceKernelSystemInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetSystemInfo(&info) < 0) return false;
    bool any = false;
    for (unsigned i = 0; i < 3; ++i) {
        idle[i] = info.cpuInfo[i].idleClock;
        any = any || idle[i] != 0;
    }
    // Vita3K leaves the call unimplemented (success, nothing filled): an all-zero idle clock is "not available".
    return any;
}

void count_busy_time(std::uint64_t us) {
    if (t_index == static_cast<std::size_t>(-1)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (t_index < g_threads.size()) g_threads[t_index].usage.busy_us += us;
}

ThreadBusyScope::ThreadBusyScope() : start_(sceKernelGetProcessTimeWide()) {}

ThreadBusyScope::~ThreadBusyScope() { count_busy_time(sceKernelGetProcessTimeWide() - start_); }

#else

void enter_thread_role(ThreadRole, const std::string&) {}
void count_busy_time(std::uint64_t) {}
std::vector<ThreadUsage> thread_usage() { return {}; }
bool core_idle_us(std::uint64_t (&)[3]) { return false; }

#endif

}  // namespace omega
