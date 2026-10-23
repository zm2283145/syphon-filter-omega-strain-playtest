#pragma once
// The upper-body pose pass of the secondary component's update 001cb160
// (Checkpoint 120o, research/COMBAT.md). After the per-bone sampler 001d6120
// fills the chain table 004f32b0 (21 entries, stride 0x100; entry i is model
// node i), 001cb160 runs:
//   001cdd18..001cde8c  the mode loop: per entry the state modes +b0/+b1 of
//                       the current (+310) and previous (+311) upper-body
//                       states and the aim weight +b4, blended by +31c;
//   001d5730            the forward pass: +80 (local), +90 (world rotation),
//                       +a0 (world position) from the parent's +90/+a0;
//   001d02b0            the composer: node +1c0 from the state selectors.
// Interpreter-verified with tests/upper_body_pose_goldens.hpp
// (tools/ee/gen_upper_body_pose.py), including the static tables after
// 001d7de0 / 001d7b20. Checkpoint 120p adds the per-bone sampler loop of
// 001d6120 (upper_body_sample; tests/upper_body_sampler_goldens.hpp from
// tools/ee/gen_upper_body_sampler.py): +40 (local), +50 (world) and +60
// (position) for entries whose +70 byte is set, from the playback records'
// clip keys.
//
// The IK chain after 001d5730 is in upper_body_ik.hpp, the look records
// 001dd6a0, the 0xc / 0x10 branch 001cdec0..001ce02c, the arm glue, the head
// pass 001d0780 and the final matrix loop 001ce7c0 in upper_body_combine.hpp,
// and the whole pass as hosted in play in upper_body_pass.hpp. Not ported:
// the leg pass 001d0c80.
#include "animation_root.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace omega {

inline constexpr std::size_t upper_body_entry_count=21;
inline constexpr std::size_t upper_body_state_count=25;

// Row 0049f790 + 0x50 s after the remap 001d7de0: +40 modes, +44 selectors,
// +48 weights (pointers into shared arrays), +4c..+4f flag bytes.
struct UpperBodyStateRow {
    std::array<std::uint8_t,upper_body_entry_count> modes{};
    std::array<std::uint8_t,upper_body_entry_count> selectors{};
    std::array<float,upper_body_entry_count> weights{};
    std::array<std::uint8_t,4> flags{};
    std::uint32_t modes_address=0;
    std::uint32_t selectors_address=0;
    std::uint32_t weights_address=0;
};
const std::array<UpperBodyStateRow,upper_body_state_count>& upper_body_state_rows();

// Chain table entry constants set by 001d7b20: +10 the limit (xyz from
// 004f3030, w untouched), +20 the reference rotation (004f2d90), +30 its
// conjugate, +70 (0048a6a0: sampled by 001d6120), +b8 (0048a6c0: aim from
// node +1c0).
struct UpperBodyEntryConstants {
    RootVector limit{};
    RootVector reference{};
    RootVector inverse{};
    std::uint8_t sampled=0;
    std::uint8_t composed_aim=0;
};
const std::array<UpperBodyEntryConstants,upper_body_entry_count>& upper_body_entry_constants();
// 001d7b20's +c0: the parent entry of each model node from its parent byte
// (node +c0 points at it; 0xff: none, -1).
std::array<int,upper_body_entry_count> upper_body_parents(
    std::span<const std::uint8_t,upper_body_entry_count> parent_bytes);

// One chain table entry.
struct UpperBodyEntry {
    RootVector limit{};          // +10
    RootVector reference{};      // +20
    RootVector inverse{};        // +30
    RootVector sampled_local{};  // +40 (001d6120)
    RootVector sampled_world{};  // +50 (001d6120)
    RootVector sampled_position{}; // +60 (001d6120)
    std::uint8_t sampled=0;      // +70 (001d7b20)
    RootVector local{};          // +80
    RootVector world{};          // +90
    RootVector position{};       // +a0
    RootVector target{};         // +d0 (IK local; read by the composer, selector 4)
    RootVector ik_world{};       // +e0 (IK world rotation, upper_body_ik.hpp)
    RootVector ik_position{};    // +f0 (IK world position)
    std::uint8_t mode_a=0;       // +b0
    std::uint8_t mode_b=0;       // +b1
    float weight=0.0F;           // +b4
    std::uint8_t composed_aim=0; // +b8
    int parent=-1;               // +c0
};
// The model node fields the pass reads and writes (node stride 0x1d0).
struct UpperBodyNode {
    RootVector pose{};     // +1a0
    RootVector offset{};   // +1b0
    RootVector composed{}; // +1c0
    // +c0 points at the skeleton's bone record; 001d6120 reads its floats
    // +8 / +c / +10 (the local offset, skeleton.hpp).
    std::array<float,3> bone_offset{};
};

// 001d6120's view of one playback record (+e0 / +110, secondary_playback.hpp)
// and its clip: +14 (root translation: bone 0 keeps only z), +18 (root yaw:
// bone 0 has an extra key quadword before its rotation), +1c / +20 the
// translation / rotation bone masks, the clip's key words from the key base,
// and the binding's bone count (the skeleton's first word).
struct UpperBodySampleSlot {
    bool present=false;          // record +0 != 0
    std::uint32_t root_translation_14=0,root_yaw_18=0,translation_mask_1c=0,rotation_mask_20=0;
    std::span<const std::uint32_t> keys;
    std::uint32_t bone_count=0;
    std::int32_t cursor_10=0;    // record +10
    float fraction_14=0.0F;      // record +14
    std::int32_t key_18=0,next_key_1c=0; // record +18 / +1c as word offsets
};

// 001cdd18..001cde8c: weights A and B are clamped to max(0, w) in place, A
// first (they may be the same array); then +b4 per entry.
void upper_body_mode_loop(std::span<const std::uint8_t,upper_body_entry_count> modes_a,
    std::span<float,upper_body_entry_count> weights_a,
    std::span<const std::uint8_t,upper_body_entry_count> modes_b,
    std::span<float,upper_body_entry_count> weights_b,
    float blend,std::span<UpperBodyEntry,upper_body_entry_count> entries);

// 001d29f0(out, q, limit): the rotation clamp; returns v0 (1: clamped).
bool upper_body_limit_rotation(RootVector& out,const RootVector& q,const RootVector& limit);
// 00190750(out, a, b, t): normalized a + (b - a) t through the accumulator.
RootVector upper_body_lerp(const RootVector& a,const RootVector& b,float t);
// 001a7df0(out, a1, a2): a2 (x) a1.
RootVector upper_body_quaternion_multiply(const RootVector& a1,const RootVector& a2);

// 001d5730(component, aim): entries.size() == nodes.size() == the model's
// node count (<= 21); blend is the component's +31c.
void upper_body_forward(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    float blend,const RootVector& aim);

// 001d6120(component, aim) up to its timing tail (001d6120..001d70a0): a is
// +e0, b is +110; both are sampled per node when +318 > 0 and both are
// present (rotations hemisphere-tested against +20 and lerped by +31c,
// translations lerped), else the present one. Entries with +70 set get +40,
// +50 = parent +50 (x) +40 (node 0: aim (x) +40) and +60 = the sampled
// translation plus the bone offset, placed by the parent (node 0: rotated by
// aim, no translation). Slot cursors and key offsets advance per sampled
// bone. Returns at once when neither record is present.
void upper_body_sample(UpperBodySampleSlot& a,UpperBodySampleSlot& b,float weight_318,float blend_31c,
    const RootVector& aim,std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes);

// 001d02b0(component): node +1c0 from the selectors of states A (+310) and B
// (+311); distinct is A != B.
void upper_body_compose(std::span<const UpperBodyEntry> entries,std::span<UpperBodyNode> nodes,
    std::span<const std::uint8_t,upper_body_entry_count> selectors_a,
    std::span<const std::uint8_t,upper_body_entry_count> selectors_b,bool distinct,float blend);

}
