#include "object_events.hpp"
#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {
PathWorld::PathWorld(std::size_t queue_limit) : queue_limit_(queue_limit) {
    if (!queue_limit) throw std::runtime_error("Object queue limit must be positive");
}
void PathWorld::ready() const {
    if (failed_) throw std::runtime_error("Object world failed; construct a new world before continuing");
}
PathWorld::Entry& PathWorld::entry(std::uint32_t id) {
    ready();
    const auto found = objects_.find(id);
    if (found == objects_.end()) throw std::runtime_error("Unknown object receiver");
    return found->second;
}
const PathMotion& PathWorld::motion(std::uint32_t id) const {
    const auto found = objects_.find(id);
    if (found == objects_.end()) throw std::runtime_error("Unknown object receiver");
    return found->second.motion;
}
void PathWorld::add(const PopulationObject& object) {
    ready();
    if (objects_.contains(object.id)) throw std::runtime_error("Duplicate object receiver");
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Object receiver generation exhausted");
    objects_.emplace(object.id, Entry{next_generation_, PathMotion(object)});
    ++next_generation_;
}
void PathWorld::remove(std::uint32_t id) {
    entry(id);
    objects_.erase(id);
}
void PathWorld::set_filter(Filter filter) {
    ready();
    if (pumping_) throw std::runtime_error("Cannot replace object filter during delivery");
    filter_ = std::move(filter);
}
void PathWorld::send(std::uint32_t id, ObjectMessage message) {
    const auto generation = entry(id).generation;
    if (message.kind != ObjectMessage::Kind::trigger && message.kind != ObjectMessage::Kind::pathed_notice)
        throw std::runtime_error("Unsupported object message kind");
    if (message.kind == ObjectMessage::Kind::pathed_notice && (message.action < 0 || message.action > 2))
        throw std::runtime_error("Unsupported pathed notice action");
    if (pending_.size() >= queue_limit_) throw std::runtime_error("Object pending queue limit exceeded");
    pending_.push_back({id, generation, message});
}
void PathWorld::collect(std::uint32_t id) {
    // Pose records are local publications, not PathedNotice messages.
    for (const auto& event : entry(id).motion.events()) {
        effects_.push_back({id, event});
        // A pose is a local publication; a delay end (00216cd8) is the host's
        // (the +e4 sound and the class-0xb reference triggers).
        if (event.kind == "pose" || event.kind == "delay_end") continue;
        int action;
        if (event.kind == "stopped") action = 0;
        else if (event.kind == "moving") action = 1;
        else if (event.kind == "waypoint") action = 2;
        else throw std::runtime_error("Unsupported motion notice");
        send(id, {ObjectMessage::Kind::pathed_notice, action, event.waypoint});
    }
}
void PathWorld::script_trigger(std::uint32_t id) {
    auto& target = entry(id).motion;
    send(id, {});
    target.prepare_trigger();
}
void PathWorld::receive_trigger(std::uint32_t id) {
    auto& target = entry(id).motion;
    try { target.trigger(); collect(id); }
    catch (...) { failed_ = true; throw; }
}
void PathWorld::script_reset_and_trigger(std::uint32_t id) {
    auto& target = entry(id).motion;
    // Reserve/enqueue first so queue capacity rejection cannot leave a half-reset.
    // No filter can run until a later explicit pump invocation.
    send(id, {});
    try { target.reset_path(); collect(id); }
    catch (...) { failed_ = true; throw; }
}
void PathWorld::script_seek(std::uint32_t id, int waypoint) {
    auto& target = entry(id).motion;
    try { target.seek(waypoint); collect(id); }
    catch (...) { failed_ = true; throw; }
}
void PathWorld::script_stop(std::uint32_t id) { entry(id).motion.stop(); }
void PathWorld::script_set_speed(std::uint32_t id, float speed) { entry(id).motion.set_speed(speed); }
void PathWorld::set_parent(std::uint32_t id, std::function<std::array<float,16>()> parent) {
    entry(id).motion.set_parent(std::move(parent));
}
void PathWorld::carry(std::uint32_t id, const std::array<float,16>& matrix) {
    auto& target = entry(id).motion;
    try { target.carry(matrix); collect(id); }
    catch (...) { failed_ = true; throw; }
}
void PathWorld::tick(std::uint32_t id, float timestep) {
    auto& target = entry(id).motion;
    try { target.tick(timestep); collect(id); }
    catch (...) { failed_ = true; throw; }
}
std::vector<ObjectDelivery> PathWorld::pump() {
    ready();
    if (pumping_) throw std::runtime_error("Reentrant object queue pump is unsupported");
    pumping_ = true;
    auto batch = std::move(pending_);
    pending_.clear();
    std::vector<ObjectDelivery> deliveries;
    try {
        for (const auto& envelope : batch) {
            auto found = objects_.find(envelope.receiver);
            auto outcome = ObjectDelivery::Outcome::stale;
            if (found != objects_.end() && found->second.generation == envelope.generation) {
                const bool intercepted = filter_ && filter_(envelope.receiver, envelope.message);
                // A filter may catch a failed mutating operation. Failure still closes this world.
                ready();
                // Native safety: a callback may remove or replace the target. Never deliver to its replacement.
                found = objects_.find(envelope.receiver);
                if (found != objects_.end() && found->second.generation == envelope.generation) {
                    if (intercepted) outcome = ObjectDelivery::Outcome::intercepted;
                    else if (envelope.message.kind == ObjectMessage::Kind::trigger) {
                        found->second.motion.trigger();
                        collect(envelope.receiver);
                        outcome = ObjectDelivery::Outcome::native_consumed;
                    } else outcome = ObjectDelivery::Outcome::unhandled;
                }
            }
            deliveries.push_back({envelope.receiver, envelope.message, outcome});
        }
    } catch (...) {
        pumping_ = false;
        failed_ = true;
        throw;
    }
    pumping_ = false;
    return deliveries;
}
std::vector<ObjectEffect> PathWorld::take_effects() {
    auto result = std::move(effects_);
    effects_.clear();
    return result;
}
}
