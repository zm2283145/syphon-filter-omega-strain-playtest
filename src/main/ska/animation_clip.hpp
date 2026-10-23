#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace omega {
struct ClipBone {
    std::optional<std::array<float, 4>> translation, rotation;
};
struct ClipFrame {
    std::optional<std::array<std::uint32_t, 4>> root_yaw_words;
    std::vector<ClipBone> bones;
};
struct AnimationClip {
    std::uint32_t version{}, frame_intervals{}, stride_words{}, bone_count{};
    float frame_period{};
    bool looping{};
    std::uint32_t translation_mask{}, rotation_mask{};
    std::array<std::uint8_t, 112> header_bytes{};
    std::vector<ClipFrame> frames;
};
// Raw version-3 channels only. No blending, pose offsets or root-motion execution.
AnimationClip decode_animation_clip(std::span<const std::uint8_t> bytes, std::uint32_t bone_count);
void write_animation_clip(std::ostream& output, const AnimationClip& clip);
}
