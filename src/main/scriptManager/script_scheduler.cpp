#include "script_scheduler.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {

ScriptScheduler::ScriptScheduler(std::size_t max_pending, std::size_t callback_budget)
    : current_(queue_.end()), max_pending_(max_pending), callback_budget_(callback_budget) {
    if (!max_pending_ || !callback_budget_)
        throw std::runtime_error("Script scheduler limits must be positive");
}

void ScriptScheduler::ready() const {
    if (failed_) throw std::runtime_error("Script scheduler previously failed");
}

ScriptScheduler::Handle ScriptScheduler::enqueue(std::int32_t delay, std::function<void()> callback) {
    ready();
    if (!callback) throw std::runtime_error("Scheduled callback is empty");
    if (queue_.size() >= max_pending_)
        throw std::runtime_error("Script scheduler pending limit reached");
    if (next_handle_ > std::uint64_t(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Script scheduler handle space exhausted");
    const auto handle = static_cast<Handle>(next_handle_);
    queue_.push_back({handle, delay, std::move(callback)});
    const auto inserted = std::prev(queue_.end());
    try {
        handles_.emplace(handle, inserted);
    } catch (...) {
        queue_.erase(inserted);
        throw;
    }
    ++next_handle_;
    return handle;
}

bool ScriptScheduler::cancel(Handle handle) {
    ready();
    const auto found = handles_.find(handle);
    if (found == handles_.end()) return false;
    const auto entry = found->second;
    handles_.erase(found);
    // Original removal protects the current event, but the script handle map
    // is still erased. Its storage lives until callback return and pump cleanup.
    if (entry != current_) queue_.erase(entry);
    return true;
}

bool ScriptScheduler::reschedule(Handle handle, std::int32_t delay) {
    ready();
    const auto found = handles_.find(handle);
    if (found == handles_.end()) return false;
    found->second->remaining_ticks = delay;
    return true;
}

void ScriptScheduler::clear() {
    ready();
    handles_.clear();
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it == current_) ++it;
        else it = queue_.erase(it);
    }
}

std::size_t ScriptScheduler::advance(std::int32_t delta) {
    ready();
    if (advancing_) {
        failed_ = true;
        throw std::runtime_error("Recursive script scheduler advance is unsupported");
    }
    if (delta < 0) throw std::runtime_error("Negative script-clock delta is unsupported");
    // Validate the entire subtraction pass before mutating any queued delay.
    for (const auto& entry : queue_) {
        if (std::int64_t(entry.remaining_ticks) - delta < std::numeric_limits<std::int32_t>::min())
            throw std::runtime_error("Script scheduler tick subtraction overflows");
    }
    for (auto& entry : queue_) entry.remaining_ticks -= delta;
    advancing_ = true;
    std::size_t executed = 0;
    try {
        while (true) {
            const auto due = std::find_if(queue_.begin(), queue_.end(),
                [](const Entry& entry) { return entry.remaining_ticks <= 0; });
            if (due == queue_.end()) break;
            if (executed >= callback_budget_)
                throw std::runtime_error("Script scheduler callback budget reached");
            current_ = due;
            due->callback();
            if (failed_) throw std::runtime_error("Script scheduler failed during callback");
            ++executed;
            handles_.erase(due->handle);
            current_ = queue_.end();
            queue_.erase(due);
            // Restart from live begin without subtracting this pump's delta
            // again. Callback-enqueued zero/negative delays can run now.
        }
        advancing_ = false;
        return executed;
    } catch (...) {
        // Callback execution cannot be rolled back. Poison this scheduler and
        // release queued captures; never continue partially executed VM work.
        failed_ = true;
        advancing_ = false;
        current_ = queue_.end();
        handles_.clear();
        queue_.clear();
        throw;
    }
}

std::vector<ScriptScheduler::Snapshot> ScriptScheduler::snapshot() const {
    std::vector<Snapshot> result;
    result.reserve(queue_.size());
    for (const auto& entry : queue_) result.push_back({entry.handle, entry.remaining_ticks});
    return result;
}

}
