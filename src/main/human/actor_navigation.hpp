#pragma once
#include "animation_root.hpp"
#include "animation_controller.hpp"
#include <functional>
#include <cstdint>
#include <optional>
#include <iosfwd>

namespace omega {
struct ActorNavigationAnchor {RootVector position{0,0,0,1};float weight{};};
struct ActorNavigationInput {
    bool physical_present{},target_enabled{};
    std::uint32_t mode{},flags3204{},flags3210{};
    RootParentTransform world;
    RootVector target_position{0,0,0,1};
    float target_yaw{},speed_limit{};
    bool stationary_latch{},retime_target{};
    bool component_predicate{},actor_flag14{};
    bool equipment_present{},primary_classified{},secondary_classified{};
    std::uint8_t equipment_state{},body_class{};
    bool body_enabled{};
    RootVector equipment_rotation{0,0,0,1};
    ActorNavigationAnchor first_ladder_anchor,second_ladder_anchor;
    RootVector fallback_ladder_position{0,0,0,1};
};
struct ActorNavigationTuning {
    float ladder_offset{}; // Initialized004a0410.
    float ladder_stop_distance{}; // Initialized004a0420.
    float distance_scale{}; // Initialized0049cff0, positive.
};
ActorNavigationTuning read_actor_navigation_tuning(std::istream& input);
struct ActorNavigationPlan {
    bool updated{},arrived{},turn_pose{};
    RootVector direction{};
    float speed{},turn{};
    // Original00476490 sends a synchronous solo self-directed action with
    // flags1/transport1. Present values are31 or32; delivery is host-owned.
    std::optional<std::uint8_t> immediate_ladder_action;
    // Retained height-error direction before the later component predicate.
    std::optional<std::uint8_t> ladder_action_candidate;
    // Original vector-curve helpers retime to .2 and copy the target vector.
    // Owning curve state and its subsequent update remain host responsibilities.
    std::optional<RootVector> retime_target;
    float target_duration{0.2F};
};
// Original001a80e0 navigation body. Inputs are resolved gameplay state, not
// invented destinations. sqrt/rsqrt follow the interpreter's VU model
// (vu_float.hpp); portable trig is not a PS2 bit-parity claim.
// All calculated values use separated binary32 operations and finite guards.
ActorNavigationPlan prepare_actor_navigation(const ActorNavigationInput& input,
    const ActorNavigationTuning& tuning);
// Original tail001a9058..908c; metric is the recovered ordered group sum.
float actor_navigation_playback_rate(float metric,float channel2_current);
float actor_navigation_group_metric(const AnimationController& controller,
    const AnimationPlayerState& player,const AnimationGraph& graph,
    const MotionGroupSelector& selector={std::nullopt,0});
struct ActorNavigationHost {
    std::function<ActorNavigationInput()> snapshot;
    // Apply only updated direction/speed/turn/turn_pose/arrived fields. The
    // supplied patch has request fields cleared; this is not an event callback.
    std::function<void(const ActorNavigationPlan&)> apply;
    std::function<void(std::uint8_t)> deliver_ladder_immediate;
    std::function<void(bool)> set_arrived;
    std::function<void(float,const RootVector&)> retime_target;
    std::function<float()> group_metric,channel2_current;
    std::function<void(float)> set_playback_rate;
};
// Original body, immediate ladder request, optional vector-curve request, then
// playback-rate tail. Later predicates/model reads use fresh host state.
void update_actor_navigation(const ActorNavigationHost& host,const ActorNavigationTuning& tuning);
}
