#pragma once
#include "animation_controller.hpp"
#include "animation_player.hpp"
#include "material_properties.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// Walking footsteps (research/GENERATOR_RUNTIME.md "Checkpoint 119m"): the gait
// part of 00197e00, the foot probe 001e9710 with the published floor plane
// (001e5430 / 001e53a0) and the dry (surface material) branch of 00188fb0.
namespace omega {

// 00198f30 / 00198ca0 on the motion group selector {0, group} (actor +2ed0):
// the newest active record's first group matching the group number (the match
// rule of actor_navigation_group_metric); nullopt when none. 001987d0: that
// selector's total weight summed over every active record. 00197e00 samples
// group 0 (the gait, sp+990) and group 5 (the shimmy, sp+988).
struct ActorGaitSample {
    std::optional<float> phase;   // group +24 (phase.current)
    float weight{};
};
ActorGaitSample actor_gait_sample(const AnimationController& controller,
    const AnimationPlayerState& player, const AnimationGraph& graph, std::int32_t group = 0);
inline constexpr std::int32_t actor_shimmy_motion_group = 5;   // 00197e30: sw 5, 0x98c(sp)

// 00197e00..00198018. physical: +3310 present; contact_dd: physical byte +dd.
// foot: the new +31dd (0 = not written); effect: 00188fb0(actor, foot, 0) runs.
struct ActorGaitStep {
    float phase_351c{};
    std::uint8_t foot{};
    bool effect{};
};
ActorGaitStep actor_gait_step(float phase_351c, const ActorGaitSample& sample, bool physical, bool contact_dd);

// 0019801c..00198274 on the group-5 sample: +3520 = phase (0 without a record);
// sound when the phase crosses 0.11 / 0.3 / 0.46 / 0.66 / 0.8 upwards from the
// old +3520 or falls below it (the wrap), and the weight is above 0.25. The
// sound is M_SHIMMY_MOVE (0049e048) bank 1 owned by the actor (001ca210,
// 0020f990, 003c9420 transport 1).
struct ActorShimmyStep {
    float phase_3520{};
    bool crossed{};   // 00198138: 001987d0 queried
    bool sound{};
};
ActorShimmyStep actor_shimmy_step(float phase_3520, const ActorGaitSample& sample);
inline constexpr const char* actor_shimmy_move_sound = "M_SHIMMY_MOVE";   // 0049e048

// 001e5430(out, point, normal): (n, -(n.x*p.x + n.y*p.y + n.z*p.z)), VU0 order.
RootVector actor_floor_plane(const RootVector& normal, const RootVector& point);
// 001e53a0(a, b): ((a.x*b.x + a.y*b.y) + a.z*b.z) + a.w*b.w, VU0 order.
float actor_plane_dot(const RootVector& a, const RootVector& b);

// 001e9710(out, physical, hit*, foot) with the threshold f12: (0,0,0,1) and no
// hit when +e8 is clear or foot is 0; otherwise node 7 (foot 1) or 11's world
// position, its z projected on the plane +f0, and hit = node z - plane z < threshold.
struct ActorFootProbe {
    RootVector point{0, 0, 0, 1};
    bool hit{};
};
ActorFootProbe actor_foot_probe(bool contact_e8, std::uint8_t foot, const RootVector& plane_f0, float threshold,
    const std::function<RootVector(int)>& node_world_position);
inline constexpr float actor_footstep_probe_threshold = 2.25F;   // 00189044: 0x40100000

// 00188fb0's dry branch after a probe hit: the material row (00189560) by the
// floor surface byte, property 4 when +3204 bit 4 is set else 1, then the sound:
// owned (property bank -1: 0020f990, transport 1) or positional at the probe
// point (0020f860, transport 0), none for -2. effect_id is property +c (not -1 /
// -2 -> 00232460); effect_named marks a -1 id with an effect name (the level's
// 004089f0, MaterialPropertyTable::resolve_effects, clears resolved names).
struct ActorFootstepSound {
    enum class Kind { none, owned, positional } kind{};
    std::int32_t bank{};
    std::string name;
    std::int32_t effect_id{-1};
    bool effect_named{};
};
ActorFootstepSound actor_footstep_sound(const MaterialRow& row, std::uint32_t flags_3204, std::uint32_t mode_320c,
    bool argument_a2);

// 0x001892e4..0x00189438: the frame 00188fb0 gives 00232460 for a resolved
// effect id. u = the physical velocity +3310 -> +310 (the static zero vector
// 004e1d90 without a physical component). With u x (0, 0, 1, 0) not zero
// (001320b0): rows normalize(u x Z), normalize(u), their cross, (0, 0, 0, 1)
// (00131f00); otherwise 00131c70(u) (the reference (0, 0, 1, 0) when |u.z| <
// 0.707, else (1, 0, 0, 0)) or, u zero, the identity 00131c40. Row 3 is then
// the probe point (sp+b20, all four words). VU arithmetic.
std::array<RootVector, 4> actor_footstep_effect_frame(const RootVector& velocity_310, const RootVector& probe_point);

}
