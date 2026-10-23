#pragma once
// The tail of the upper-body pose pass in 001cb160 after the forward pass
// 001d5730 (Checkpoint 120s, research/COMBAT.md): the smoothstep of +318
// into +31c (001ccb18), the look record weight ramp of 001dd6a0, the arm
// glue 001ce080..001ce6b8 (001d4eb0 / 001d2c10 / 001d40e0 / 001d28d0 for the
// right hand, 001d3e30 / 001d2c10 / 001d2f90 / 001d28d0 for the left), the
// head pass 001d0780 and the node rows 001ce7c0..001ce830 (node +1c0 to the
// matrix passed to the node's vt +38). Interpreter-verified with
// tests/upper_body_combine_goldens.hpp (tools/ee/gen_upper_body_combine.py).
//
// The look records +3a0 / +410 run through 001dd6a0 / 001dd1c0
// (tests/upper_body_look_goldens.hpp, tools/ee/gen_upper_body_look.py).
//
// Not ported: the leg pass 001d0c80 with its inputs 001ccb60..001cdb98.
#include "upper_body_ik.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace omega {

// 001ccb18..001ccb5c: x = +318; +31c = min(max(3 x^2 - 2 x^3, 0), 1).
float upper_body_smoothstep(float x);

// 001dd6a0(record, ..., f12 dt) for a record whose +4 is set: step =
// min(1, 3 dt); +60 = min(1, +60 + step) when +5 (active) is set (every
// active path ends at 001de250), else max(0, +60 - step) (001de280). The
// caller returns at once when +4 is clear (001dd6d4).
float upper_body_look_ramp(float weight,bool active,float dt);

// A look record (component +3a0 / +410, stride 0x70). 001d8610 (called at
// 001d83d4 by the component constructor 001d81a0) sets bytes +0..+3 to -1,
// +4 / +5 to 0, +10 to 00136080 (0, 0, 0, 0), +20..+50 to vf0 and +60 to 0;
// 001d84b8..001d84dc then sets +3a0's bytes to (0xe, 0, -1, -1) with +3a4 =
// 0 and +410's to (0xe, 9, 5, 0xe) with +414 = 1. +3a4 is never stored
// nonzero, so only +410 runs; its +5 / +10 are ActorAimState's
// head_active_415 / head_point_420.
struct UpperBodyLookRecord {
    std::array<std::int8_t,4> nodes{-1,-1,-1,-1}; // +0 origin node, +1 entry, +2 base entry, +3 second entry
    bool enabled=false;                            // +4
    bool active=false;                             // +5
    RootVector point{};                            // +10 (world)
    RootVector world_20{0,0,0,1},local_30{0,0,0,1},world_40{0,0,0,1},local_50{0,0,0,1};
    float weight=0.0F;                             // +60
};
UpperBodyLookRecord upper_body_head_look_record(); // +410 after 001d81a0

// 001dd1c0(out, record, component, context) with context +14 set: up =
// (0, 0, 1) through the transposed rotation of the context +28 object's +10
// matrix (its column 2, w 0); p = origin (the +0 node's +10 matrix row 3,
// 001831b0) through the component (xyzw); out = the matrix quaternion of
// 001d3ad0's look frame of point - p about up (limit 0x3f7fffe7, falling
// back to the identity rows 2 / 0 as 001cf660, secondary_look_at).
RootVector upper_body_look_target(const RootVector& point,const std::array<float,16>& up_matrix,
    const std::array<float,16>& component,const RootVector& origin);

// 001dd6a0(record, component, context, table 004f32b0, f12 dt). When +4 is
// set: with +5, b = c (x) (e[+2].+90 (x) e[+1].+20) (just e[+1].+20 when +2 is
// -1) with c the component's matrix quaternion; l = 001dd1c0; q = conj(b) (x)
// l; +30 = q, or when +60 != 0 the 00190750 lerp by min(1, 3 dt) of conj(b)
// (x) +20 (negated if its 4D dot with +30 is < 0) to q (negated if q.w < 0);
// +30 clamped by 001d29f0's test against e[+1].+10; +20 = b (x) +30. When +3
// != -1 the same for +40 / +50 with b' = +20 (x) e[+3].+20 and e[+3].+10 (q'
// is matched against +30, not +50). Then the weight ramp
// (upper_body_look_ramp). The component matrix (sp+e40) is the same object
// matrix 001dd1c0 reads its up axis from.
void upper_body_look(UpperBodyLookRecord& record,std::span<const UpperBodyEntry> entries,
    const std::array<float,16>& component,const RootVector& origin,float dt);

// 001d0780(component, c440, c460, i, w): entry i and its parent. s =
// |1 - +25c| > 1e-7 and l = 00190750((0, 0, 0, 1), conj(+250), 0.5). w != 0:
// parent +d0 = 00190750(+80, +20 (x) c440, 0.7 w), entry +d0 the same with
// c460, each then (x) l when s. w == 0: +d0 = +80 (x) l when s, else +d0 /
// +e0 / +f0 = +80 / +90 / +a0 (parent first).
void upper_body_head(std::span<UpperBodyEntry> entries,const RootVector& component_250,
    const RootVector& c440,const RootVector& c460,std::size_t i,float weight);

// The component and stack inputs of the arm glue.
struct UpperBodyArmsInput {
    std::uint8_t state_a=0,state_b=0; // +310 / +311
    float blend_31c=0.0F;
    RootVector component_250{0,0,0,1};
    RootVector component_480{0,0,0,1},component_490{}; // constructor 001d81a0: vf0, 004e1d90; then upper_body_grip_step
    RootVector component_4f0{0,0,0,1},component_5c0{},component_6b0{0,0,0,1},component_780{}; // state 24
    RootVector target_d70{},up_df0{},look_d90{0,0,0,1},body_cd0{0,0,0,1};
};
// The stack slots and component weight the glue updates. 990 / 930 are the
// rotations handed to 001d28d0; when neither state yields one the call reads
// the slot unchanged (whatever the stack held).
struct UpperBodyArmsState {
    float weight_308=0.0F;
    RootVector rotation_990{},rotation_930{};
};
struct UpperBodyArmsResult {
    bool right=false,left=false;                  // row +4f / +4e gate
    bool right_rotation=false,left_rotation=false; // 990 / 930 written this call
};
// 001ce080..001ce6b8. Right (row flag +4f of state A or B): targets 9b0 /
// 9a0 from 001d4eb0 (B only when A != B); both: 9c0 = their xyz lerp by
// +31c (w of 9b0), +308 = 1; A: 9c0 = 9b0, +308 = 1 - +31c; B: 9c0 = 9a0,
// +308 = +31c; neither: 9c0 = 0, +308 = 0; 001d2c10(9c0, 20, +308).
// Rotations 980 / 970 from 001d40e0; both: 980 hemisphere-matched to 970,
// 990 = their unnormalized lerp by +31c; else the one returned; then
// 001d28d0(990, 20). Left (+4e): the same with 001d3e30 (950 / 940 -> 960),
// 001d2c10(960, 19), 001d2f90 (920 / 910 -> 930) and 001d28d0(930, 19).
UpperBodyArmsResult upper_body_arms(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const UpperBodyArmsInput& in,UpperBodyArmsState& state);

// The gun model at attach point 0 (actor +60 +2410): bone 1's position +170
// and rotation +180 and bone 2's position +190 (the .GUN bones at +150).
struct UpperBodyGunGrip {
    RootVector position_170{},rotation_180{0,0,0,1},position_190{};
};
// 001b1c28..001b1cec in actor logic 001b1710 (after 0019f6c0), with +3314
// present: nothing unless 00385230 finds a model (nullopt). Position
// 003851c0 = +170 + +190 (vadd; its w lane, the bones' parent words, is
// replaced by 1.0 at 001b1c7c); rotation 00385130 = +180, or vf0 when
// vaddx +180, vf0 sets exactly the four zero flags (MAC 0xf). With
// t = 00192740(10.5 dt, 0, 1) (max.s then min.s): +480 = 00190750(+480,
// rotation, t) (upper_body_lerp) and +490 = 001a9160(+490, position, t)
// (+490 + position t - +490 t through the accumulator, all lanes).
void upper_body_grip_step(RootVector& rotation_480,RootVector& offset_490,
    const std::optional<UpperBodyGunGrip>& gun,float dt);

// 001ce7c0..001ce830: the rows sp+3c0..3f0 built from node +1c0 for the
// node's vt +38: row r = (m[4r], m[4r+1], m[4r+2], 0), row 3 = vf0.
std::array<float,12> upper_body_node_rows(const RootVector& q);

// 001cdea0..001ce02c, run when +310 or +311 is 0xc or 0x10: a = sp+9f0
// (secondary_aim_root_offset) when A is 0xc / 0x10, else node 0 +1b0; b the
// same for B; node 0 +1b0 = (a + b t - a t through the accumulator (xyz),
// 1.0) with t = +31c, handed to node 0's vt +3c (the local translation row).
// nullopt when the branch is skipped.
std::optional<RootVector> upper_body_root_offset(std::uint8_t state_a,std::uint8_t state_b,float blend_31c,
    const RootVector& node0_offset,const RootVector& offset_9f0);

}
