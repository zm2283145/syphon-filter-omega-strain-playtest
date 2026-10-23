#pragma once
#include "path_motion.hpp"
#include <functional>
#include <map>

namespace omega {

// Logical payloads, not the original memory layout or a network serialization.
struct ObjectMessage {
    enum class Kind { trigger, pathed_notice };
    Kind kind{Kind::trigger};
    int action{};
    int waypoint{-1};
    // A trigger's +28 word and +30 actor (0: none). Trigger(PathedGOBJ) 00211350 zeroes both; an InteractGOBJ's
    // 0xe forward (002166a8) carries its cloned Triangle trigger's.
    std::uint32_t word_28{};
    std::uint64_t actor_30{};
};
struct ObjectDelivery {
    std::uint32_t receiver{};
    ObjectMessage message;
    enum class Outcome { intercepted, native_consumed, unhandled, stale } outcome{};
};
struct ObjectEffect {
    std::uint32_t receiver{};
    MotionEvent event;
};

// Solo deferred-delivery adapter. Caller supplies script interception explicitly;
// this does not discover live bindings, simulate multiplayer, or move linked actors.
class PathWorld {
public:
    using Filter = std::function<bool(std::uint32_t, const ObjectMessage&)>;
    explicit PathWorld(std::size_t queue_limit = 16384);
    void add(const PopulationObject& object);
    void remove(std::uint32_t id);
    const PathMotion& motion(std::uint32_t id) const;
    void set_filter(Filter filter);
    void send(std::uint32_t id, ObjectMessage message);
    void script_trigger(std::uint32_t id);
    // 00219070 taking a trigger its script filter declined, delivered by the caller (a type-8 box's Triangle
    // trigger in the actor pump): the motion restarts at once; no filter is offered again.
    void receive_trigger(std::uint32_t id);
    void script_reset_and_trigger(std::uint32_t id);
    void script_seek(std::uint32_t id, int waypoint);
    void script_stop(std::uint32_t id);
    void script_set_speed(std::uint32_t id, float speed);
    // A lift link's parent (PathMotion::set_parent) and the lift's 00218200 carry.
    void set_parent(std::uint32_t id, std::function<std::array<float,16>()> parent);
    void carry(std::uint32_t id, const std::array<float,16>& matrix);
    void tick(std::uint32_t id, float timestep);
    std::vector<ObjectDelivery> pump();
    std::vector<ObjectEffect> take_effects();
    std::size_t pending_count() const { return pending_.size(); }
    bool failed() const { return failed_; }
private:
    struct Entry { std::uint64_t generation; PathMotion motion; };
    struct Envelope { std::uint32_t receiver; std::uint64_t generation; ObjectMessage message; };
    std::map<std::uint32_t, Entry> objects_;
    std::vector<Envelope> pending_;
    std::vector<ObjectEffect> effects_;
    Filter filter_;
    std::size_t queue_limit_;
    std::uint64_t next_generation_{1};
    bool pumping_{}, failed_{};
    void ready() const;
    Entry& entry(std::uint32_t id);
    void collect(std::uint32_t id);
};
}
