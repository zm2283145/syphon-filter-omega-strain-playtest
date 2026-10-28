#include "worker_pool.hpp"

#include "float_environment.hpp"
#include "thread_roles.hpp"

#include <algorithm>
#include <stdexcept>

#if defined(OMEGA_ZONE_PROFILE)
#include <chrono>
#endif
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

namespace omega {

#if defined(OMEGA_ZONE_PROFILE)
namespace {
// 132vc1 (port-only): WorkerPool::Profile's totals.
struct ProfileCounters {
    std::atomic<std::uint64_t> calls{0}, items{0}, caller_items{0}, call_us{0}, tail_us{0}, joins{0}, wake_us{0},
        worker_us{0};
};
ProfileCounters g_profile;
std::uint64_t profile_now_us() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}
}  // namespace
#endif

WorkerPool::Profile WorkerPool::profile() {
    Profile p;
#if defined(OMEGA_ZONE_PROFILE)
    p.calls = g_profile.calls.load();
    p.items = g_profile.items.load();
    p.caller_items = g_profile.caller_items.load();
    p.call_us = g_profile.call_us.load();
    p.tail_us = g_profile.tail_us.load();
    p.joins = g_profile.joins.load();
    p.wake_us = g_profile.wake_us.load();
    p.worker_us = g_profile.worker_us.load();
#endif
    return p;
}

WorkerPool::WorkerPool(unsigned workers) {
    threads_.reserve(workers);
    for (unsigned k = 0; k < workers; ++k)
        threads_.emplace_back([this, k] {
            enter_thread_role(ThreadRole::worker, "worker " + std::to_string(k + 1));  // Vita: core 2 (no-op elsewhere)
            loop();
        });
}

WorkerPool::~WorkerPool() {
    try {
        wait_tasks();  // 132vc2: no task outlives the pool
    } catch (...) {
    }
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
}

unsigned WorkerPool::default_workers(unsigned hardware_threads) {
    if (hardware_threads <= 1) return 0;
    return std::min(hardware_threads - 1, 7U);
}

std::size_t WorkerPool::work(Job& job) {
    std::size_t ran = 0;
    for (;;) {
        const std::size_t i = job.next.fetch_add(1);
        if (i >= job.count) return ran;
        ++ran;
        try {
            (*job.fn)(i);
        } catch (...) {
            std::lock_guard lock(job.error_mutex);
            if (!job.error || i < job.error_index) {
                job.error = std::current_exception();
                job.error_index = i;
            }
        }
        job.done.fetch_add(1);
    }
}

void WorkerPool::loop() {
    std::uint64_t seen = 0;
    for (;;) {
        Job* job = nullptr;
#if defined(__vita__)
        if (spin()) {  // 132vc1: the next call is often a few hundred microseconds away (the particle hooks)
            const std::uint64_t until = sceKernelGetProcessTimeWide() + 300;
            for (unsigned k = 0; published_.load(std::memory_order_acquire) == seen && !tasks_pending(); ++k)
                if ((k & 63u) == 63u && sceKernelGetProcessTimeWide() >= until) break;
        }
#endif
        Task task;
        bool have_task = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || generation_ != seen || tasks_head_ < tasks_.size(); });
            if (quit_) return;
            if (generation_ != seen) {
                seen = generation_;
                job = job_;
            }
            if (job) {
                ++active_;
                active_now_.fetch_add(1, std::memory_order_relaxed);
            } else {
                have_task = take_task(task);  // 132vc2: no job to join: a queued task
            }
        }
        if (!job) {
            if (have_task) run_task(task);
            continue;
        }
        {
            ThreadBusyScope busy_time;
            const FloatControlsScope caller_mode(job->float_controls);  // 132vs3: the caller's rounding / denormal flush
#if defined(OMEGA_ZONE_PROFILE)
            const std::uint64_t joined = profile_now_us();
            if (work(*job)) {
                g_profile.joins.fetch_add(1, std::memory_order_relaxed);
                g_profile.wake_us.fetch_add(joined - job->start_us, std::memory_order_relaxed);
                g_profile.worker_us.fetch_add(profile_now_us() - joined, std::memory_order_relaxed);
            }
#else
            work(*job);
#endif
        }
        {
            std::lock_guard lock(mutex_);
            --active_;
            active_now_.fetch_sub(1, std::memory_order_release);
        }
        finished_.notify_all();
    }
}

bool WorkerPool::take_task(Task& out) {
    if (tasks_head_ >= tasks_.size()) return false;
    out = std::move(tasks_[tasks_head_++]);
    if (tasks_head_ == tasks_.size()) {
        tasks_.clear();
        tasks_head_ = 0;
    }
    return true;
}

void WorkerPool::run_task(Task& task) {
    {
        ThreadBusyScope busy_time;
        const FloatControlsScope caller_mode(task.float_controls);
        try {
            task.fn();
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!task_error_) task_error_ = std::current_exception();
        }
        task.fn = nullptr;
    }
    {
        std::lock_guard lock(mutex_);
        tasks_pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
    tasks_done_.notify_all();
}

void WorkerPool::submit(std::function<void()> task) {
    if (threads_.empty()) {
        try {
            task();
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!task_error_) task_error_ = std::current_exception();
        }
        return;
    }
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back({std::move(task), float_controls()});
        tasks_pending_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_.notify_one();
}

void WorkerPool::wait_tasks() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            if (!take_task(task)) {
                tasks_done_.wait(lock, [&] { return tasks_pending_.load(std::memory_order_acquire) == 0; });
                if (task_error_) {
                    auto e = task_error_;
                    task_error_ = nullptr;
                    std::rethrow_exception(e);
                }
                return;
            }
        }
        run_task(task);
    }
}

void WorkerPool::for_each(std::size_t count, const std::function<void(std::size_t)>& fn) {
    for_each_beside(count, fn, {});
}

void WorkerPool::for_each_beside(std::size_t count, const std::function<void(std::size_t)>& fn,
                                 const std::function<void()>& first) {
    if (count == 0 || threads_.empty()) {
        for (std::size_t i = 0; i < count; ++i) fn(i);  // the single-threaded path (the first throw ends it)
        if (first) first();
        return;
    }
    if (count == 1 && !first) {
        fn(0);
        return;
    }
    Job job;
    job.fn = &fn;
    job.count = count;
    job.float_controls = float_controls();
#if defined(OMEGA_ZONE_PROFILE)
    job.start_us = profile_now_us();
#endif
    {
        std::lock_guard lock(mutex_);
        job_ = &job;
        ++generation_;
        published_.store(generation_, std::memory_order_release);
        ++batches_;
        items_ += count;
    }
    wake_.notify_all();
    std::exception_ptr first_error;
    if (first) try {
            first();
        } catch (...) {
            first_error = std::current_exception();
        }
#if defined(OMEGA_ZONE_PROFILE)
    const std::size_t caller_ran = work(job);
    const std::uint64_t tail_start = profile_now_us();
#else
    work(job);
#endif
    {
        std::unique_lock lock(mutex_);
        job_ = nullptr;  // a worker waking late sees no job
#if defined(__vita__)
        if (active_ != 0 && spin()) {  // 132vc1: no worker can join now; spin for the active ones before sleeping
            lock.unlock();
            const std::uint64_t until = sceKernelGetProcessTimeWide() + 2000;
            for (unsigned k = 0; active_now_.load(std::memory_order_acquire) != 0; ++k)
                if ((k & 63u) == 63u && sceKernelGetProcessTimeWide() >= until) break;
            lock.lock();
        }
#endif
        finished_.wait(lock, [&] { return active_ == 0; });
    }
#if defined(OMEGA_ZONE_PROFILE)
    const std::uint64_t end = profile_now_us();
    g_profile.calls.fetch_add(1, std::memory_order_relaxed);
    g_profile.items.fetch_add(count, std::memory_order_relaxed);
    g_profile.caller_items.fetch_add(caller_ran, std::memory_order_relaxed);
    g_profile.call_us.fetch_add(end - job.start_us, std::memory_order_relaxed);
    g_profile.tail_us.fetch_add(end - tail_start, std::memory_order_relaxed);
#endif
    if (job.done.load() != count) throw std::logic_error("WorkerPool: a job index did not run");
    if (job.error) std::rethrow_exception(job.error);
    if (first_error) std::rethrow_exception(first_error);
}

void HostThread::Completion::wait() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return finished; });
}

bool HostThread::Completion::done() {
    std::lock_guard lock(mutex);
    return finished;
}

void HostThread::Completion::finish() {
    {
        std::lock_guard lock(mutex);
        finished = true;
    }
    cv.notify_all();
}

HostThread& HostThread::loading_screen() {
    static HostThread* t = new HostThread("loading screen");  // never destroyed: its thread waits to the process end
    return *t;
}

HostThread& HostThread::render() {
    static HostThread* t = new HostThread("render");
    return *t;
}

void HostThread::start() {
    std::lock_guard lock(mutex_);
    if (started_) return;
    started_ = true;
    std::thread([this] { loop(); }).detach();
}

void HostThread::loop() {
    enter_thread_role(ThreadRole::render, name_);  // Vita: both host threads draw: core 1 (no-op elsewhere)
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return static_cast<bool>(job_); });
            job = std::move(job_);
            job_ = nullptr;
        }
        job();
    }
}

std::shared_ptr<HostThread::Completion> HostThread::run(std::function<void()> job) {
    auto completion = std::make_shared<Completion>();
    start();
    {
        std::lock_guard lock(mutex_);
        if (!busy_) {
            busy_ = true;
            job_ = [this, completion, job = std::move(job)] {
                // The host thread is marked free before the waiter wakes, so a job given right after wait() reuses it.
                struct Release {
                    HostThread* host;
                    std::shared_ptr<Completion> completion;
                    ~Release() {
                        {
                            std::lock_guard lock(host->mutex_);
                            host->busy_ = false;
                        }
                        completion->finish();
                    }
                } release{this, completion};
                job();
            };
            ++jobs_;
            wake_.notify_one();
            return completion;
        }
    }
    ++spawned_;
    std::thread([completion, job = std::move(job)] {  // the host thread is busy: a thread of its own
        try {
            job();
        } catch (...) {
            completion->finish();
            throw;
        }
        completion->finish();
    }).detach();
    return completion;
}

int parse_worker_threads(const std::string& text) {
    if (text == "auto" || text == "-1") return -1;
    std::size_t used = 0;
    int value = -2;
    try {
        value = std::stoi(text, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != text.size() || value < 0 || value > 64)
        throw std::invalid_argument("worker threads: expected auto, -1 or 0..64, got \"" + text + "\"");
    return value;
}

}  // namespace omega
