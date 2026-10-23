#pragma once
// The upper-body pose pass of 001cb160 after its state step and 001d7480
// (Checkpoint 120s, research/COMBAT.md), as far as it is ported, for one
// actor:
//   001ccb18  +31c = smoothstep(+318);
//   001d6120  the per-bone sampler (upper_body_sample) on the records +e0 /
//             +110, aim sp+cf0;
//   001cdd18  the mode loop of states +310 / +311;
//   001cdea0  the 0xc / 0x10 root offset (upper_body_root_offset) into node 0
//             +1b0 and its vt +3c;
//   001d5730  the forward pass, aim sp+cf0;
//   001ce04c  001dd6a0 for the look records +3a0 (+4 is 0: returns) and
//             +410 (upper_body_look, its 001dd1c0 target from node 14);
//   001ce080  the arm glue (upper_body_arms) with the aim's stack vectors;
//   001ccb60  the leg input block (upper_body_leg_targets): the physical
//             component's two contacts, nodes 11 / 7 / 16 / 12 +10 and
//             nodes 16 / 12 +90, through the component matrix;
//   001ce6bc  001d2880(component, 0);
//   001ce734  001d0c80(component, sp+b90, sp+a30, 0x10, sp+c0 || s0) and
//   001ce75c  001d0c80(component, sp+b80, sp+a20, 0xc, s1 || s0) with s0 =
//             max(entry 0 +b0, +b1) != 2: the leg pass (upper_body_leg_pass)
//             on entries 16, 11, 6, 3 and 12, 7, 4, 1;
//   001ce764  001d0780(component, +440, +460, 0xe, +470): the head pass
//             (upper_body_head) on entries 14 and 9 with +410's +30 / +50 /
//             +60;
//   001ce740  001d02b0, the composer into node +1c0;
//   001ce7c0  per node the rows of +1c0 handed to the node's vt +38
//             (001cee20: the node's local rotation rows +60..+88).
// Not ported, so their effect is unknown: the state 24 sources +4f0 / +5c0 /
// +6b0 / +780.
//
// The chain table 004f32b0 is one static array shared by every actor, so
// every field this actor's pass does not write holds another actor's (or an
// earlier frame's) value; the stack slots sp+990 / sp+930 likewise. The pass
// therefore runs several times with different finite fills of every such
// unknown word (the table fields 001d7b20 does not set, the two stack slots,
// the state 24 fields, node +1c0 where it is unknown, the look record's
// +20..+60 when the caller does not know them) and reports a
// node's rows only when they are bitwise identical in every run: those do
// not depend on any unknown word. A run that throws (nonfinite arithmetic,
// a key outside the clip) leaves the whole frame unreported.
#include "secondary_aim.hpp"
#include "upper_body_combine.hpp"
#include "upper_body_ik.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace omega {

struct UpperBodyPassInput {
    std::uint8_t state_a=0,state_b=0;              // +310 / +311 after the state step
    float weight_318=0.0F;                         // +318 after the state step
    std::array<std::uint8_t,upper_body_entry_count> parent_bytes{}; // the nodes' parent bytes (0xff: none)
    // Node +1a0, +1b0 and the bone offset; +1c0 where composed_known.
    std::array<UpperBodyNode,upper_body_entry_count> nodes{};
    std::array<bool,upper_body_entry_count> composed_known{};
    SecondaryAimResult aim;                        // sp+d70, df0, d90, cf0, cd0, 9f0
    RootVector component_250{0,0,0,1};             // the recoil curve +230's current value
    // The left-hand grip +480 / +490 (constructor 001d81a0: vf0 / 0; actor
    // logic's upper_body_grip_step, 001b1c28).
    RootVector component_480{0,0,0,1},component_490{};
    float weight_308=0.0F;
    UpperBodySampleSlot a,b;                       // the records +e0 / +110
    // The look record +410 (+3a0 never runs: its +4 is 0), its +20..+60
    // unknown unless look_known; the component matrix (context +28, the
    // matrix the aim reads); the +10 matrix row 3 of node look_410.nodes[0]
    // (001831b0); the frame's dt (f12, kept in f23 from 001cb1a8).
    UpperBodyLookRecord look_410=upper_body_head_look_record();
    bool look_known=true;
    std::array<float,16> component{};
    RootVector look_origin{0,0,0,1};
    float dt=0.0F;
    // 001ccb60's inputs (the physical component's contacts +40 / +70, the
    // node matrices and +90 translations) and the component's byte +8:
    // 1 from the constructor 001d81a0, 2 when activation 001b5560
    // (001b574c..001b5794) finds actor +14 set and the +58 component's
    // vt +54 true (the local player).
    UpperBodyLegInput legs;
    std::uint8_t component_8=1;
};
struct UpperBodyPassResult {
    bool reported=false;                           // false: a run threw (failure)
    std::string failure;
    float blend_31c=0.0F;
    std::optional<RootVector> root_offset;         // node 0 +1b0 / its local translation row
    std::array<std::optional<std::array<float,12>>,upper_body_entry_count> rows; // vt +38 rows
    std::array<std::optional<RootVector>,upper_body_entry_count> composed;      // node +1c0
    float weight_308=0.0F;
    UpperBodySampleSlot a,b;                       // the records after 001d6120's sampling
    UpperBodyArmsResult arms;
    std::optional<UpperBodyLookRecord> look_410;   // after 001dd6a0 when no unknown word reached it
};
inline constexpr std::size_t upper_body_pass_runs=4;
UpperBodyPassResult run_upper_body_pass(const UpperBodyPassInput& input);

// The model's side of the input: the first 21 nodes' +1a0 / +1b0 and bone
// record offsets, their parent bytes, the binding's bone count (the
// skeleton's first word, UpperBodySampleSlot::bone_count) and their +90
// (vt +30, 001cf3f0: the local matrix's translation row, which 001ccb60
// reads for nodes 16 and 12).
struct UpperBodyModelNodes {
    std::array<std::uint8_t,upper_body_entry_count> parent_bytes{};
    std::array<UpperBodyNode,upper_body_entry_count> nodes{};
    std::uint32_t bone_count=0;
    std::array<RootVector,upper_body_entry_count> translation_90{};
};

}
