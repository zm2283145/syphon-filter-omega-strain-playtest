#pragma once
// The IK chain of the upper-body pose pass in 001cb160 (001ce03c..001ce930,
// Checkpoint 120q, research/COMBAT.md). It works on the chain table 004f32b0
// (upper_body_pose.hpp): entry +d0 the IK local rotation (the composer's
// selector 4), +e0 / +f0 its world rotation and position. Interpreter-verified
// with tests/upper_body_ik_goldens.hpp (tools/ee/gen_upper_body_ik.py).
//
// Checkpoint 120r added the right-hand target 001d4eb0 and rotation 001d40e0
// (tests/upper_body_hand_goldens.hpp from tools/ee/gen_upper_body_hand.py);
// Checkpoint 120s the left-hand rotation 001d2f90 and the callers in
// upper_body_combine.hpp (with 001dd6a0 / 001dd1c0). Checkpoint 132rs2b: the
// leg pass 001d0c80 (001d1140 / 001d0cf0, tests/leg_pass_goldens.hpp from
// tools/ee/gen_leg_pass.py) and its input block 001ccb60..001cdbc4
// (tests/leg_inputs_goldens.hpp from tools/ee/gen_leg_inputs.py).
#include "upper_body_pose.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace omega {

// 001d2880(component, i): entry i +d0 / +e0 / +f0 = +80 / +90 / +a0.
void upper_body_ik_reset(std::span<UpperBodyEntry> entries,std::size_t i);

// 001d5fa0(entry i): +e0 = +d0 and +f0 = node +1b0; with a parent, +e0 =
// parent +e0 (x) +e0 and +f0 placed by the parent's +e0 / +f0.
void upper_body_ik_world(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::size_t i);

// 001d5e90(entry i): +f0 = node +1b0, placed by the parent's +e0 / +f0; +e0
// is left alone.
void upper_body_ik_position(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::size_t i);

// 001d28d0(component, q, i): t = +30 (x) q, hemisphere-tested against
// +30 (x) node +1c0, clamped by +10 (001d29f0) into +d0; then +d0 = +20 (x)
// +d0 and 001d5fa0.
void upper_body_ik_set_rotation(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& q,std::size_t i);

// 001d3e30(component, out, state): the left-hand target of an upper-body
// state (state & 0xff). States 3 and 10..16: component +490 placed by entry
// 20's +e0 / +f0; state 5: entry 13's +a0 plus the unit direction to entry
// 19's sampled +60 moved by entry 14's +a0 - +60, scaled by |19 +60 - 13 +60|;
// state 24: component +5c0. Returns false (out untouched) for every other
// state.
bool upper_body_left_hand_target(std::span<const UpperBodyEntry> entries,const RootVector& component_490,
    const RootVector& component_5c0,std::uint32_t state,RootVector& out);

// 001d4eb0(component, out, state): the right-hand target of an upper-body
// state (state & 0xff), used by 001cb160 when the state row's +4f is set.
// States 3, 4, 5, 23: entry 15's +a0 plus |20 +60 - 15 +60| along the RSQRT
// direction to entry 20's sampled +60 moved into entry 14's frame (state 3:
// by 14's +50 conjugate, +90 and +a0; else by +a0 - +60); states 6, 7: 15
// +a0 + 20 +60 - 15 +60; states 10, 11 (entry 14) and 12, 16 (entry 0): 20
// +60 moved into that entry's frame; state 24: component +780. Returns false
// (out untouched) for every other state.
bool upper_body_right_hand_target(std::span<const UpperBodyEntry> entries,const RootVector& component_780,
    std::uint32_t state,RootVector& out);

// The stack arguments 001cb160 passes to 001d40e0.
struct UpperBodyHandInput {
    RootVector component_250{0,0,0,1}; // the +250 quaternion (+25c its w)
    RootVector component_6b0{0,0,0,1}; // state 24
    RootVector target{};               // a3: sp+d70, the aim point in model space
    RootVector up{};                   // t0: sp+df0, (0, 0, 1, 0) in model space
    RootVector aim{0,0,0,1};           // t1: sp+d90 (states 12, 16)
    RootVector body{0,0,0,1};          // t2: sp+cd0
};

// 001d40e0(component, out, state, ...): the right-hand rotation (state &
// 0xff) in entry 20's parent frame. States 3..7, 10, 11, 23: the static rest
// quaternion 0049ff60 (Euler 0, pi/2, pi/2), turned by the normalized 0.35
// lerp to +250 when |1 - +25c| > 1e-7, by the look-at of the target from
// entry 20 +f0 about up (limit 0x3f7fffe7), made w >= 0, then body (x) and
// conj(parent +e0) (x). States 12, 16: aim (x) 0049ff70, w >= 0, body (x),
// conj(parent +e0) (x). States 13..15: entry 20 +40. State 24: conj(parent
// +e0) (x) component +6b0, hemisphere-matched to entry 20 +20. Returns false
// (out untouched) for every other state.
bool upper_body_right_hand_rotation(std::span<const UpperBodyEntry> entries,const UpperBodyHandInput& in,
    std::uint32_t state,RootVector& out);

// The arguments 001cb160 passes to 001d2f90 (a3 sp+d70, t0 sp+df0, t1
// sp+cd0) and the component fields it reads.
struct UpperBodyLeftHandInput {
    RootVector component_250{0,0,0,1};
    RootVector component_480{0,0,0,1}; // vf0 from the constructor 001d81a0
    RootVector component_4f0{0,0,0,1}; // state 24
    RootVector target{};
    RootVector up{};
    RootVector body{0,0,0,1};
};

// 001d2f90(component, out, state, target, up, body): the left-hand rotation
// (state & 0xff) in entry 19's parent frame. State 5: the aim path of
// 001d40e0 from the static rest 0049ff80 (Euler 0, pi/2, -pi/2) and entry 19
// +f0. States 3, 10..16: conj(parent +e0) (x) entry 20 +e0 (x) component
// +480. State 24: conj(parent +e0) (x) component +4f0, hemisphere-matched to
// entry 19 +20. Returns false (out untouched) for every other state.
bool upper_body_left_hand_rotation(std::span<const UpperBodyEntry> entries,const UpperBodyLeftHandInput& in,
    std::uint32_t state,RootVector& out);

// The inputs of the two-bone arm solver 001d1740 (a0 / a1 are the outputs).
struct UpperBodyArmInput {
    RootVector target;            // a2
    RootVector upper_position;    // a3: upper bone +a0
    RootVector upper_local;       // t0: upper bone +80
    RootVector upper_limit;       // t1: upper bone +10
    RootVector upper_reference;   // t2: upper bone +20
    RootVector forearm_position;  // t3: forearm +a0
    RootVector forearm_local;     // sp+0: forearm +80
    RootVector forearm_limit;     // sp+8: forearm +10
    RootVector forearm_reference; // sp+10: forearm +20
    RootVector parent_world;      // sp+18: the upper bone's parent +90
    RootVector hand_position;     // sp+20: hand +a0
};

// 001d1740(upper out, forearm out, ...): swings the arm so the hand points
// at the target (rotation between the shoulder->hand and shoulder->target
// directions), bends the shoulder by the law-of-cosines angle about
// reach x elbow, clamps the upper local rotation by its limit (the inline
// copy of 001d29f0 between the reference conjugates), then turns the
// forearm from the moved elbow onto the target and clamps it the same way.
// A zero reach x elbow axis keeps the first swing and the upper local
// rotation (negated to w >= 0).
void upper_body_arm_solve(const UpperBodyArmInput& in,RootVector& upper,RootVector& forearm);

// 001d2c10(component, target, i, weight): the arm reach of hand entry i over
// its parent (forearm) and grandparent (upper bone). weight <= 0 restores
// both +d0 from +40; otherwise 001d1740 runs and weight < 1 blends +40 to
// the hemisphere-matched solution (unnormalized lerp), weight >= 1 takes it.
// The two entries above the upper bone get +d0 / +e0 / +f0 = +80 / +90 /
// +a0, then 001d5fa0 (upper bone, forearm) and 001d5e90 (hand).
void upper_body_ik_reach(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& target,std::size_t i,float weight);

// 001d1140(component, target, node, flag): the leg reach of entry node (16 or
// 12) over its parent (end), grandparent (lower), great-grandparent (upper)
// and the entry above (Checkpoint 132rs2b). With flag 0 and component +8 !=
// 2 the three +d0 = +80 (no world update). Otherwise the lower local
// rotation is turned (in the upper +90 frame) by the arc from the RSQRT
// direction lower +a0 -> end +a0 to the one towards the midpoint of end and
// upper +a0, the node's +a0 swung with it about lower +a0 (both directions
// must be nonzero, else neither moves); 001d1740 solves upper / lower +d0
// for the target with the above entry's +e0 as the parent frame; end +d0 =
// +80; then 001d5fa0 on upper, lower and end.
void upper_body_leg_solve(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::uint8_t component_8,const RootVector& target,std::size_t node,bool flag);

// 001d0cf0(component, rotation, node, flag): flag 0: node +d0 = model node
// +1a0. Otherwise 001d0df0: t = +30 (x) conj(parent +e0) (x) rotation,
// negated when t.w < 0, clamped by +10 (the 001d29f0 logic), +d0 = +20 (x) t.
// +e0 / +f0 are not updated.
void upper_body_foot_rotation(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& rotation,std::size_t node,bool flag);

// 001d0c80(component, target, rotation, node, flag): 001d1140 then 001d0cf0.
void upper_body_leg_pass(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::uint8_t component_8,const RootVector& target,const RootVector& rotation,std::size_t node,bool flag);

// One ground contact record of the physical component (component +10 =
// actor +3310): +40 point, +50 normal, +60 weight (and +70 / +80 / +90).
struct UpperBodyLegContact {
    RootVector point{},normal{};
    float weight=0.0F;
};
// What 001ccb60..001cdbc4 of 001cb160 reads: the two contact records, the
// +10 world matrices (64 bytes in memory order) of nodes 11, 7, 16 and 12
// and the +90 (vt +30, 001cf3f0) of nodes 16 and 12.
struct UpperBodyLegInput {
    UpperBodyLegContact contact_40,contact_70;
    std::array<float,16> node_11{},node_7{},node_16{},node_12{};
    RootVector translation_16{},translation_12{};
};
// Its stack outputs: the normals sp+cb0 / ca0 in the component frame, the
// leg targets sp+b90 (node 16) / b80 (node 12), the foot rotations sp+a70..
// a40 (sp+a30 = a50 and sp+a20 = a40: sp+d0 and s5 are always 1) and the
// ground flags sp+c0 / s1.
struct UpperBodyLegTargets {
    RootVector normal_cb0{},normal_ca0{};
    RootVector target_b90{},target_b80{};
    RootVector rotation_a70{},rotation_a60{},rotation_a50{},rotation_a40{};
    bool flag_c0=false,flag_s1=false;
};
// 001ccb60..001cdbc4 (component the matrix sp+e40, inverse its rigid inverse
// sp+e00): the normal of a contact with weight > 0 turned into the component
// frame (0013ed60), else (0, 0, 1, 0); the world position of node 16 (node
// 11's matrix on node 16's +90) and node 12 (node 7 on node 12) through the
// component (00171b30); flag = weight > 0 and ground z (contact +48 / +78) not
// below the foot; a clear flag takes the foot's own z; the target is (foot x,
// foot y, z, 1) through the inverse. Each foot rotation is the node frame
// (001bc2b0) turned by the rotation taking its row 2 onto the normal
// (001cf2d0 with the inline quaternion matrix), as a quaternion (the inline
// block of secondary_matrix_quaternion). Nodes 11 / 16 use the +70 contact,
// 7 / 12 the +40 contact.
UpperBodyLegTargets upper_body_leg_targets(const UpperBodyLegInput& in,const std::array<float,16>& component,
    const std::array<float,16>& inverse);

}
