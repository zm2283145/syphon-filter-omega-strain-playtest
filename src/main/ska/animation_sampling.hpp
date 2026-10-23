#pragma once
#include "animation_clip.hpp"
#include "skeleton.hpp"
#include "clip_playback.hpp"

namespace omega {
struct SampledClipBone {
    std::uint32_t bone_index{};
    std::optional<std::array<float, 4>> translation, rotation;
};
struct NonRootClipSample {
    std::uint32_t frame_index{}, next_frame_index{};
    float fraction{};
    std::vector<SampledClipBone> bones;
};

// Original default clip controls only: no mirror, reversal, subrange or phase
// offset. Every non-root bone is listed; missing channels remain null for the
// downstream accumulator/retained-state finalizer. No normalization or root
// motion is performed at this stage.
NonRootClipSample sample_default_nonroot_bones(const AnimationClip& clip,
                                              const SkeletonDefinition& skeleton,
                                              float phase);
NonRootClipSample sample_nonroot_bones(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, const ClipPlaybackControl& control);
void write_nonroot_clip_sample(std::ostream& output, const NonRootClipSample& sample);
}
