#pragma once
// The aim half of the secondary (upper-body) component's update 001cb160
// (actor +3314; research/COMBAT.md, "Checkpoint 54"): from the entry to
// 001cc8d4, where the state step (secondary_component.hpp,
// step_secondary_states) takes the aim angle. Interpreter-verified:
//   001cb160..001cb3e8  the weights +308/+30c moved toward +304 (their eased
//                       values are computed and discarded), the two upper-body
//                       layers +4a0/+660 (001cab40, 001cfae0) and the settled
//                       bytes +820/+821 (001cfa90);
//   001cb3ec..001cc17c  the aim solve: node 0's rotation, the model component
//                       and the base aggregate give the body-relative
//                       orientation +150 aligns; aim slot 0 (replaced by a point
//                       2000 ahead when the states do not aim); the look-at
//                       001cf660 from node 0xe (or node 0 with the 1.45 offset)
//                       to the aim point; the per-state clamps; the +300 timer;
//   001cc268..001cc534  the +160 quaternion curve (damping 10, retimed 0.15 s
//                       while aiming) and +140;
//   001cc538..001cc788  the +230 recoil spring in 1/60 s steps with the kick
//                       001cff50 (three draws of the game RNG after each
//                       001ca7d0 request);
//   001cc7f0..001cc8d0  +140, +150 and the angle atan2f(-v.x, v.y) of the Y axis
//                       rotated by the solved quaternion.
// The upper-body pose pass after the state step is in upper_body_pose.hpp
// (Checkpoint 120o: the chain table constants, the mode loop, 001d5730 and
// 001d02b0), upper_body_ik.hpp / upper_body_combine.hpp (120q..120s) and
// upper_body_pass.hpp (120s, hosted in play); still not ported: the leg pass
// 001d0c80; the one-shot layer at +830 (001cea90,
// 001ce980): in this build no instruction stores a nonzero state there and its
// +838 receiver is never set, so it stays idle.
#include "actor_aim_actions.hpp"
#include "animation_vector_curve.hpp"
#include "camera_curves.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace omega {

// A 0x1c0-byte layer of the pair at +4a0 (constructor 001d8500): the source
// frame +0 (orientation) / +10 (position) relative to node +20 (-1: none), the
// quaternion curve +30 (class 004da440, no hemisphere, damping 0) and the vector
// curve +100 (class 004da410, damping 0). Its source is written only by
// 001ca990, reached from the idle one-shot layer, so it keeps the constructor's
// identity frame.
struct SecondaryAimLayer {
    RootVector orientation{0,0,0,1}, position{0,0,0,1};
    std::int8_t node{-1};
    CameraQuaternionCurve rotation;
    AnimationVectorCurveState translation;
    bool operator==(const SecondaryAimLayer&) const = default;
};
SecondaryAimLayer make_secondary_aim_layer();
// 001cfa90: both curves have no time remaining.
bool secondary_aim_layer_settled(const SecondaryAimLayer& layer);

struct SecondaryAimState {
    RootVector aim_140{0,0,0,1};            // the clamped aim relative to the body, last frame
    RootVector body_150{0,0,0,1};           // the body orientation, last frame (hemisphere reference)
    CameraQuaternionCurve curve_160;        // damping set to 10 each update
    CameraQuaternionCurve curve_230;        // the recoil spring; damping set to 5 each update
    float timer_300{};
    float weight_304{1}, weight_308{1}, weight_30c{1};
    float recoil_324{}, recoil_328{};
    std::array<SecondaryAimLayer,2> layers;
    std::uint8_t settled_820{}, settled_821{};
    bool operator==(const SecondaryAimState&) const = default;
};
// The 001d81a0 constructor values of these fields.
SecondaryAimState make_secondary_aim_state();

// 001ca7d0(component, amount, record +a4), called by 001b3400 (the shot's
// screen shake): +324 = amount; +328 = 0 when the second value is at most 4,
// else clamp((value + 30) * 0.018, 0, 1).
void request_secondary_aim_recoil(SecondaryAimState& state,float amount,float record_a4);

// What the update reads from the model context (component +4 = actor +3020 =
// actor +2e50) and the model (component +c = actor +2ec0). Matrices are the
// 64 bytes of the original in memory order (row k = bytes 16k..16k+15).
struct SecondaryAimModel {
    std::array<float,16> component{};                 // context +28: the published placement matrix
    std::span<const std::array<float,16>> nodes;      // node k +10..+4f (skeleton space)
    RootVector node0_rotation{0,0,0,1};               // node 0 +1a0 (the rotation finalizer's cache)
    RootVector base_orientation{0,0,0,1};             // model +e0 (the root collection's base aggregate)
    RootVector base_position{0,0,0,1};                // model +f0
};
struct SecondaryAimInput {
    std::uint8_t current_310{}, next_311{};           // the state pair (rows of 0049f790, 0..24)
    float dt{};
    SecondaryAimModel model;
    std::function<std::uint32_t()> random;            // 00127dd0 (only drawn by a pending recoil)
};
struct SecondaryAimResult {
    float angle{};                                    // f0 at 001cc8d4
    bool clamped{};                                   // the 001cc028 clamp applied (d30 = q0 * d30)
    // The stack vectors the pose pass after the state step reads (Checkpoint
    // 120s): sp+d70 the aim point in skeleton space (moved with the look when
    // clamped), sp+df0 world Z in skeleton space, sp+d90 the look (the clamped
    // solve when clamped), sp+cf0 the sampler / forward aim and sp+cd0 the
    // hand rotations' body (secondary_aim_body).
    RootVector target_d70{}, up_df0{}, look_d90{0,0,0,1}, aim_cf0{0,0,0,1}, body_cd0{0,0,0,1};
    // sp+9f0 of the 0xc / 0x10 branch (secondary_aim_root_offset).
    RootVector root_offset_9f0{};
};
// Runs 001cb160 up to the state step. ActorAimState carries aim slot 0
// (+3a5/+3b0), which is replaced by the point 2000 ahead of the base aggregate
// when neither state aims. Throws for states outside the table, fewer than 15
// nodes, a nonpositive dt, a zero look direction or a look-at whose fallback
// frame 00131c70 / 00131c40 would be needed, and nonfinite arithmetic.
SecondaryAimResult step_secondary_aim(SecondaryAimState& state,ActorAimState& aim,const SecondaryAimInput& input);

// 001cc154..001cc264, when the clamp applied: R = new_look (x) conj(old_look)
// (001cfa20 / 001cf5b0), and the aim point d70 = origin d80 + the 4-lane
// offset d70 - d80 (001ae380) turned by R (its xyz sandwich, w kept; 173d00
// adds all four lanes).
RootVector secondary_aim_moved_target(const RootVector& old_look,const RootVector& new_look,
    const RootVector& target,const RootVector& origin);
// 001cc79c..001cc858 after the recoil loop: aim = aim (x) +250 unless
// |1 - +25c| <= 1e-7 (fb 0x33d6bf95); body = aim (x) conj(look).
struct SecondaryAimBody { RootVector aim_cf0, body_cd0; };
SecondaryAimBody secondary_aim_body(const RootVector& aim_cf0,const RootVector& curve_250,const RootVector& look_d90);
// 001cdec0..001cdf1c: sp+9f0 = 0013ed60(001a7370(base orientation sp+db0,
// (0.3, 0, 0, 0)), the rigid inverse sp+e00): xyz = row0 x + row1 y + row2 z,
// w = 0.
RootVector secondary_aim_root_offset(const RootVector& base_orientation,const std::array<float,16>& inverse);
// 1cb1f8..1cb294: the rigid inverse of the component matrix (sp+e00).
std::array<float,16> secondary_aim_inverse(const std::array<float,16>& component);

// Shared VU-sequence helpers (exposed for the tests).
// 001cfa20 / 00132810: the quaternion product a*b.
RootVector secondary_quaternion_multiply(const RootVector& a,const RootVector& b);
// The inline matrix-to-quaternion block (001cb470..001cb620, 001cfb6c..001cfd10)
// on the first three rows of a matrix in memory order.
RootVector secondary_matrix_quaternion(const std::array<float,16>& m);
// 001cf660(out, direction, up, identity 0055c9f0, limit): the look-at frame
// (rows side, forward, third) of the direction about up, falling back to the
// identity's row 2 and then row 0 (rows reordered) when |d . axis| >= limit.
// 001d40e0 inlines the same tests around 001d3ad0. The zero-cross fallback
// frame (00131c70 / 00131c40) throws.
std::array<float,16> secondary_look_at(const RootVector& direction,const RootVector& up,float limit);

// The cache +1a0 the rotation finalizer 003ba2b0 leaves on a node: a nonzero
// accumulator +190 normalized by VRSQRT of its 4D square sum; otherwise an
// all-zero cache becomes the XYZ-conjugated quaternion of the saved
// inverse-bind rows +d0..+f0 (the same matrix block); otherwise it is kept.
// For node 0 the accumulator is (0, 0, 0, sum of the root-rotation weights)
// (animation_root_clip.hpp, bone0_rotation_accumulator); the aim reads it.
RootVector bone_rotation_cache(const RootVector& accumulator,const RootVector& cache,const std::array<float,16>& inverse_bind);

}
