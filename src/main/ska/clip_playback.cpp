#include "clip_playback.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Clip playback requires finite arithmetic");
    return value;
}
}
ClipSampleCursor select_clip_sample(const AnimationClip& clip, float phase,
    float phase_advance, const ClipPlaybackControl& control) {
    checked(phase); checked(phase_advance);
    checked(control.range_start); checked(control.range_end); checked(control.phase_offset);
    if (phase < 0 || phase > 1 || phase_advance < 0 ||
        control.range_start < 0 || control.range_end > 1 ||
        control.range_start > control.range_end || control.phase_offset < 0 || control.phase_offset >= 1)
        throw std::runtime_error("Clip playback controls outside supported domain");
    const std::uint64_t count = std::uint64_t(clip.frame_intervals) + (clip.looping ? 0 : 1);
    if (clip.version != 3 || !clip.frame_intervals || count > 10000 || clip.frames.size() != count)
        throw std::runtime_error("Clip playback frame extent mismatch");
    float advance = checked(float(clip.frame_intervals) * phase_advance);
    if (control.reverse) {
        if (phase != 0 || !control.looping) phase = checked(1.0F - phase);
        advance = -advance;
    }
    if (control.phase_offset == 0) {
        if (control.range_start != 0 || control.range_end != 1) {
            const float width = checked(control.range_end - control.range_start);
            const float scaled = checked(phase * width);
            phase = checked(control.range_start + scaled);
            if (!control.looping) phase = std::min(1.0F, phase);
            else if (phase >= control.range_end) phase = checked(phase - width);
            advance = checked(advance * width);
        }
    } else {
        phase = checked(phase + control.phase_offset);
        if (phase >= 1) phase = checked(phase - 1.0F);
    }
    const float coordinate = checked(phase * float(clip.frame_intervals));
    if (coordinate < 0 || coordinate > float(clip.frame_intervals))
        throw std::runtime_error("Clip playback frame coordinate outside extent");
    ClipSampleCursor result;
    result.frame_index = static_cast<std::uint32_t>(coordinate);
    result.next_frame_index = control.looping ?
        (result.frame_index + 1 >= clip.frame_intervals ? 0U : result.frame_index + 1) :
        std::min(result.frame_index + 1, clip.frame_intervals);
    if (result.frame_index >= count || result.next_frame_index >= count)
        throw std::runtime_error("Clip playback selected an unavailable frame");
    result.fraction = checked(coordinate - float(result.frame_index));
    result.frame_advance = advance;
    return result;
}
}
