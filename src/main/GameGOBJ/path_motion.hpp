#pragma once
#include "population.hpp"
#include <array>
#include <functional>

namespace omega {
struct MotionEvent { std::string kind; int waypoint{-1}; };
struct MotionState {
    int index{}, direction{1}, stop_target{-1}, replicated_stop_target{-1};
    float progress{1}, delay{}, speed{};
    bool stopped{true};
};

// A bounded projection of delivered operations for kind-2 objects in path mode 1
// or 3, unparented or parented (+6c) to a lift.
// Events record issue order; there is no script interception, queue, sound or replication here.
class PathMotion {
public:
    explicit PathMotion(const PopulationObject& object);
    void trigger();
    // Script-side preparation occurs before queued trigger delivery.
    void prepare_trigger();
    void reset_path();
    void stop();
    void seek(int index);
    void set_speed(float speed);
    // The +6c parent the GameGOBJ base's link setup (0021b440 -> vt+110 =
    // 00216c50) gives a lift's pathed link: the function answers the parent's
    // current root (rows; its +50 component +10..+4f). Path points then go
    // through 00218ea0's parented branch.
    void set_parent(std::function<std::array<float,16>()> parent) { parent_ = std::move(parent); }
    bool parented() const { return static_cast<bool>(parent_); }
    // A lift's 00218200 link pass: the root becomes `matrix` (rows; basis x,
    // y, z then the translation), one "pose" event.
    void carry(const std::array<float,16>& matrix);
    // 00216c60 (mode 1), bit-exact against 960 interpreter goldens (state,
    // notices, published pose); mode 3 and one-point paths (checkpoint 132el7)
    // against tests/pathed_mode3_goldens.hpp. The object loop 00133420 runs it
    // every frame whether the object is present or not (GOB +4c 0021a8c0).
    // Event kinds: "pose", the notices "moving" / "waypoint" / "stopped", and
    // "delay_end" (00216cd8: a positive delay ran out; the +e4 sound and the
    // class-0xb reference triggers are the host's).
    void tick(float timestep);
    // Script GetPos (00211310): the waypoint index *(+d0), not the progress.
    int position() const { return state_.index; }
    // Script State (002111c0): 1 (Moving) while the stopped byte +dc is clear.
    bool moving() const { return !state_.stopped; }
    // Replaces the runtime state (tests and hosts restoring a recorded state);
    // the speed and progress must be finite.
    void restore_state(const MotionState& state);
    const MotionState& state() const { return state_; }
    std::uint32_t mode() const { return mode_; }
    float delay_word() const { return pause_; }
    const PopulationObject& pose() const { return pose_; }
    const std::vector<MotionEvent>& events() const { return events_; }
private:
    PopulationObject pose_;
    MotionState state_;
    std::vector<float> lengths_;   // +b4: n entries, the last 0.0
    std::uint32_t mode_{1};        // +d8
    float pause_{};                // +c4
    std::vector<MotionEvent> events_;
    std::function<std::array<float,16>()> parent_;
    Vec3 waypoint(int index) const;
    void publish(int from, int to, float fraction, bool endpoint);
    void publish_endpoint(int point, int rotation);
    void notice(const char* kind, int waypoint = -1);
};
void write_motion_state(std::ostream& out, const PathMotion& motion);
// The interpolated rotation of 00218c80 / 00190750: the far quaternion is
// negated when the 4-term dot is negative, then (a + b*t) - a*t is scaled by
// 1/sqrt of ((x^2 + y^2) + z^2) + w^2. Throws on a zero or nonfinite norm.
std::array<float,4> path_nlerp(std::array<float,4> from, std::array<float,4> to, float t);
// The VU quaternion-to-basis rows (row-major 3x3) the pose writers use.
std::array<float,9> quaternion_basis(const std::array<float,4>& q);
}
