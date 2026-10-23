#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <vector>

namespace omega {

// Bounded scheduler for one loaded VM. Caller supplies script-clock ticks;
// argument capture/seconds conversion belong to the VM, not this queue.
class ScriptScheduler {
public:
    using Handle = std::uint32_t;
    struct Snapshot {
        Handle handle{};
        std::int32_t remaining_ticks{};
        bool operator==(const Snapshot&) const = default;
    };
    explicit ScriptScheduler(std::size_t max_pending = 16384,
                             std::size_t callback_budget = 100000);
    ScriptScheduler(const ScriptScheduler&) = delete;
    ScriptScheduler& operator=(const ScriptScheduler&) = delete;
    ScriptScheduler(ScriptScheduler&&) = delete;
    ScriptScheduler& operator=(ScriptScheduler&&) = delete;

    Handle enqueue(std::int32_t delay_ticks, std::function<void()> callback);
    bool cancel(Handle handle);
    bool reschedule(Handle handle, std::int32_t delay_ticks);
    void clear();
    std::size_t advance(std::int32_t script_delta);
    // Queue order, including an active callback until it returns. A canceled
    // active handle can therefore appear here without remaining addressable.
    std::vector<Snapshot> snapshot() const;
    bool failed() const { return failed_; }

private:
    struct Entry {
        Handle handle;
        std::int32_t remaining_ticks;
        std::function<void()> callback;
    };
    using Queue = std::list<Entry>;
    Queue queue_;
    std::map<Handle, Queue::iterator> handles_;
    Queue::iterator current_;
    std::uint64_t next_handle_{1}; // Opaque native identity; no original counter claim.
    std::size_t max_pending_, callback_budget_;
    bool advancing_{}, failed_{};
    void ready() const;
};

}
