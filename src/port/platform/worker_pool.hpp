#pragma once
// Port-only (checkpoint 132mt3, research/THREADING.md step 5): a small fixed-size job system for the CPU draw
// preparation. The PS2 game has no equivalent (the EE prepares a frame alone while the VU1 / GS run beside it); the
// pool only spreads pure computation (character skinning, the particle programs' emulation) over cores, and every
// result is merged in the original order, so the output is the same bytes as the single-threaded path.
//
// WorkerPool(n) starts n worker threads (0: none, every call runs inline on the caller: the old path). for_each(count,
// fn) runs fn(0) .. fn(count - 1) on the workers and the calling thread and returns when all are done; a call that
// throws does not stop the others, and the exception of the lowest index is rethrown on the caller (the one a serial
// loop would have met first). Calls must not touch shared state except through their own index's slot. The workers
// run a job under the caller's floating-point mode (132vs3: float_environment.hpp; the simulation thread flushes
// denormals as the PS2 does), so an index gives the same bits on any thread. Portable C++ threads only (std::thread,
// mutex, condition_variable), so the PS Vita build (3 user cores) uses the same code.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omega {

class WorkerPool {
public:
    explicit WorkerPool(unsigned workers);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }
    void for_each(std::size_t count, const std::function<void(std::size_t)>& fn);
    // Port-only (132vl1, research/VITA.md "Load time (132vl1)"): for_each, with `first` run on the calling thread
    // while the workers start on the indices (the PS Vita's level load reads the next members from the disc while
    // this batch decodes); the caller then joins the indices. With no workers: the indices, then first(). An
    // exception of an index is rethrown before one of first().
    void for_each_beside(std::size_t count, const std::function<void(std::size_t)>& fn, const std::function<void()>& first);

    // The worker count `--worker-threads -1` / the setting's "auto" picks: one less than the hardware threads (the
    // caller works too), at most 7, 0 on a single core. PS Vita: the 3 user cores (hardware_concurrency is not
    // reliable there), so 2 workers.
    static unsigned default_workers(unsigned hardware_threads);
#if defined(__vita__)
    static unsigned default_workers() { return default_workers(3); }
#else
    static unsigned default_workers() { return default_workers(std::thread::hardware_concurrency()); }
#endif

    // Calls made through for_each with more than one index (the parallel ones) and the indices they ran.
    std::uint64_t batches() const { return batches_; }
    std::uint64_t items() const { return items_; }

    // Port-only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"): the parallel calls' timing,
    // totals since the start over every pool, kept only in the profile build (OMEGA_ZONE_PROFILE: the Vita's; zeros
    // elsewhere). Microseconds. Does not change what a call computes.
    struct Profile {
        std::uint64_t calls = 0;         // for_each calls run on the workers (more than one index)
        std::uint64_t items = 0;         // their indices
        std::uint64_t caller_items = 0;  // ... of them run by the calling thread
        std::uint64_t call_us = 0;       // the caller's time inside the calls
        std::uint64_t tail_us = 0;       // ... of it waiting for the workers after its last index
        std::uint64_t joins = 0;         // workers that woke up for a call while it was still running
        std::uint64_t wake_us = 0;       // from a call's start to each joining worker's start
        std::uint64_t worker_us = 0;     // the joining workers' time inside the calls
    };
    static Profile profile();

    // Port-only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"), PS Vita only: an idle worker
    // spins up to about 300 us for the next call before it sleeps on the condition variable, and the caller spins up
    // to about 2 ms for the workers' last indices before it does (the hardware's wake-up through the condition
    // variable took 113-134 us a worker, 12-14 calls a frame). On by default; the switch is the Vita app's A/B.
    // Other platforms: no spin. Changes only when the threads run, never what a call computes.
    static void set_spin(bool on) { spin_.store(on, std::memory_order_relaxed); }
    static bool spin() { return spin_.load(std::memory_order_relaxed); }

private:
    struct Job {
        const std::function<void(std::size_t)>* fn = nullptr;
        std::size_t count = 0;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::mutex error_mutex;
        std::size_t error_index = 0;
        std::exception_ptr error;
        std::uint64_t float_controls = 0;  // the caller's floating-point mode, which the workers run the job under
        std::uint64_t start_us = 0;        // 132vc1: the call's start (the profile build only)
    };
    std::size_t work(Job& job);  // the indices this thread ran
    void loop();

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    Job* job_ = nullptr;
    std::uint64_t generation_ = 0;
    unsigned active_ = 0;  // workers inside the current job
    std::atomic<std::uint64_t> published_{0};  // 132vc1: generation_, for the spins (written under mutex_)
    std::atomic<unsigned> active_now_{0};      // 132vc1: active_, for the caller's spin (written under mutex_)
    static inline std::atomic<bool> spin_{true};
    bool quit_ = false;
    std::uint64_t batches_ = 0, items_ = 0;
};

// The value of `--worker-threads` / the port setting worker_threads=: "auto" (or -1) is default_workers(), else a
// count 0..64. Throws for anything else.
int parse_worker_threads(const std::string& text);

// Port-only (checkpoint 132mt3, research/THREADING.md "The loading-screen hang"): a thread made once and kept for the
// session, running the jobs handed to it. On Windows a thread's start and its end go through the loader's work queue
// (DLL_THREAD_ATTACH / DETACH), which the NVIDIA driver's idle-priority threads can hold for minutes on a saturated
// machine; a host thread made early (play_main starts the session's ones before the front end) lets the level
// hand-offs and level ends start and end no thread. run() hands the job to the thread when it is idle, else (a second
// job at the same time: tests) to a thread of its own; the returned Completion's wait() returns once the job ran.
class HostThread {
public:
    struct Completion {
        void wait();
        bool done();
        void finish();
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
    };
    static HostThread& loading_screen();  // the loading screens' frames (loading_screen_host.hpp)
    static HostThread& render();          // the play's GPU submission (gfx_render_thread.hpp)
    explicit HostThread(const char* name = "host") : name_(name) {}
    void start();                         // makes the thread (idempotent)
    std::shared_ptr<Completion> run(std::function<void()> job);
    std::uint64_t jobs_run() const { return jobs_; }
    std::uint64_t jobs_spawned() const { return spawned_; }

private:
    void loop();
    const char* name_;  // the thread's name in the Vita busy-time report (thread_roles.hpp)
    std::mutex mutex_;
    std::condition_variable wake_;
    std::function<void()> job_;
    bool started_ = false, busy_ = false;
    std::atomic<std::uint64_t> jobs_{0}, spawned_{0};
};

}  // namespace omega
