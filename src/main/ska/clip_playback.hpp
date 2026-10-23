#pragma once
#include "animation_clip.hpp"

namespace omega {
struct ClipPlaybackControl {
    bool reverse{}, mirror{}, looping{};
    float range_start{}, range_end{1}, phase_offset{};
};
struct ClipSampleCursor {
    std::uint32_t frame_index{}, next_frame_index{};
    float fraction{}, frame_advance{};
};
// Resolve graph phase using the clip object's playback controls. Looping here
// is the runtime override; stored frame extent still comes from the raw clip.
ClipSampleCursor select_clip_sample(const AnimationClip& clip, float phase,
    float phase_advance, const ClipPlaybackControl& control);
}
