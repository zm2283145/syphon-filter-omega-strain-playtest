#include "worker_pool.hpp"

#include "thread_roles.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {

WorkerPool::WorkerPool(unsigned workers) {
    threads_.reserve(workers);
    for (unsigned k = 0; k < workers; ++k)
        threads_.emplace_back([this, k] {
            enter_thread_role(ThreadRole::worker, "worker " + std::to_string(k + 1));  // Vita: core 2 (no-op elsewhere)
            loop();
        });
}

WorkerPool::~WorkerPool() {
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

void WorkerPool::work(Job& job) {
    for (;;) {
        const std::size_t i = job.next.fetch_add(1);
        if (i >= job.count) return;
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
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            job = job_;
            if (!job) continue;
            ++active_;
        }
        {
            ThreadBusyScope busy_time;
            work(*job);
        }
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        finished_.notify_all();
    }
}

void WorkerPool::for_each(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    if (threads_.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i) fn(i);  // the single-threaded path (the first throw ends it)
        return;
    }
    Job job;
    job.fn = &fn;
    job.count = count;
    {
        std::lock_guard lock(mutex_);
        job_ = &job;
        ++generation_;
        ++batches_;
        items_ += count;
    }
    wake_.notify_all();
    work(job);
    {
        std::unique_lock lock(mutex_);
        job_ = nullptr;  // a worker waking late sees no job
        finished_.wait(lock, [&] { return active_ == 0; });
    }
    if (job.done.load() != count) throw std::logic_error("WorkerPool: a job index did not run");
    if (job.error) std::rethrow_exception(job.error);
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
