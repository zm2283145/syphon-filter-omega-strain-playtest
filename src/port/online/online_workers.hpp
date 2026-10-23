#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2h, ONLINE_DEPLOY.md §12): a small worker pool for omega_server's
// password work (PBKDF2 at ~120000 iterations costs tens of milliseconds; on the main loop it stalled every other
// session: the load test's 64 sign-ups took the loop for seconds). submit() queues work for the workers; its done
// callback is queued back and run by drain() on the caller's (the server's) thread. Destroying the pool finishes the
// running jobs and drops every queued job and completion without running them.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace omega::online {

class WorkerPool {
public:
    explicit WorkerPool(unsigned threads) {
        if (threads == 0) threads = 1;
        for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stopping_ = true;
            jobs_.clear();
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    std::size_t threads() const { return threads_.size(); }

    // work() runs on a worker; its returned completion runs on the thread that calls drain().
    void submit(std::function<std::function<void()>()> work) {
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs_.push_back(std::move(work));
            ++outstanding_;
        }
        cv_.notify_one();
    }
    // Runs the completions that are ready; returns how many.
    std::size_t drain() {
        std::deque<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(m_);
            ready.swap(done_);
        }
        for (auto& f : ready) f();
        return ready.size();
    }
    // Jobs submitted whose completion has not been drained yet.
    std::size_t outstanding() const {
        std::lock_guard<std::mutex> lock(m_);
        return outstanding_;
    }

private:
    void run() {
        for (;;) {
            std::function<std::function<void()>()> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            auto completion = job();
            std::lock_guard<std::mutex> lock(m_);
            done_.push_back([this, completion = std::move(completion)] {
                {
                    std::lock_guard<std::mutex> inner(m_);
                    --outstanding_;
                }
                if (completion) completion();
            });
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<std::function<void()>()>> jobs_;
    std::deque<std::function<void()>> done_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace omega::online
