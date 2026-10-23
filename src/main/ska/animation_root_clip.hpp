#pragma once
#include "animation_clip.hpp"
#include "animation_root.hpp"
#include "skeleton.hpp"
#include "clip_playback.hpp"

namespace omega {
using RootAnchorKey = std::array<std::uint8_t, 4>;
struct RootClipSample {
    RootAnchorKey anchor_key{};
    std::uint32_t frame_index{}, next_frame_index{};
    float fraction{}, frame_advance{};
    std::optional<RootVector> pose_translation, pose_quaternion, motion;
    std::optional<float> angular_increment;
    bool bone0_translation{}, bone0_rotation{};
};
struct RootAnchorDefinition {
    RootAnchorKey key{};
    std::optional<RootVector> sign_reference;
};
struct WeightedRootClipSample {
    float weight{};
    RootClipSample sample;
    std::uint8_t motion_bucket{};
    bool align_sign{};
};
struct RootAnchorFrame {
    RootAnchorKey key{};
    RootFrame frame;
};
struct RootClipAccumulation {
    std::vector<RootAnchorFrame> anchors;
    RootVector bone0_translation_accumulator{}, bone0_rotation_accumulator{};
    bool bone0_translation_applied{}, bone0_rotation_applied{};
};

// Version3, no mirror/reversal/subrange/phase offset, identity incoming Q.
// phase_advance is the explicit normalized graph-group advance, not seconds.
RootClipSample sample_default_root_clip(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, float phase_advance);
RootClipSample sample_root_clip(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, float phase_advance,
    const ClipPlaybackControl& control, const RootVector& incoming_rotation = {0, 0, 0, 1});
// New per-update totals, ordered nonnegative weights. Zero weights skip their
// sample entirely. Unique explicit anchors only; unresolved keys are rejected.
// Optional sign alignment uses the prior reference, never the growing sum.
RootClipAccumulation accumulate_root_clip_contributions(
    std::span<const RootAnchorDefinition> anchors,
    std::span<const WeightedRootClipSample> contributions);
void write_root_clip_sample(std::ostream& output, const RootClipSample& sample);
void write_root_clip_accumulation(std::ostream& output, const RootClipAccumulation& value);
}
