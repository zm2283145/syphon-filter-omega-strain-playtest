#include "animation_clip.hpp"
#include "binary.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace omega {
AnimationClip decode_animation_clip(std::span<const std::uint8_t> bytes, std::uint32_t bone_count) {
    if (bytes.size() > 64 * 1024 * 1024 || bytes.size() < 112)
        throw std::runtime_error("Animation clip input outside bounded size");
    if (!bone_count || bone_count > 32) throw std::runtime_error("Clip bone count must be 1..32");
    AnimationClip clip;
    clip.version = read_u32(bytes, 0);
    if (clip.version != 3) throw std::runtime_error("Only SKA version 3 is supported");
    clip.frame_intervals = read_u32(bytes, 4);
    if (!clip.frame_intervals) throw std::runtime_error("Animation clip has no frame intervals");
    clip.stride_words = read_u32(bytes, 8);
    clip.frame_period = std::bit_cast<float>(read_u32(bytes, 12));
    if (!std::isfinite(clip.frame_period) || clip.frame_period <= 0)
        throw std::runtime_error("Clip frame period must be positive and finite");
    clip.looping = read_u32(bytes, 16) != 0;
    clip.bone_count = bone_count;
    clip.translation_mask = read_u32(bytes, 28);
    clip.rotation_mask = read_u32(bytes, 32);
    if (bone_count < 32 && ((clip.translation_mask | clip.rotation_mask) >> bone_count))
        throw std::runtime_error("Clip channel mask exceeds supplied skeleton");
    const bool root_yaw = (clip.rotation_mask & 1) && read_u32(bytes, 24) != 0;
    const auto stride = 4U * (std::popcount(clip.translation_mask) + std::popcount(clip.rotation_mask) + unsigned(root_yaw));
    if (!stride || clip.stride_words != stride) throw std::runtime_error("Clip stride does not match channel layout");
    const std::uint64_t frames = std::uint64_t(clip.frame_intervals) + (clip.looping ? 0 : 1);
    if (frames > 10000 || frames * bone_count > 1000000)
        throw std::runtime_error("Clip decoded frame count exceeds safety bound");
    const auto payload_extent = static_cast<std::size_t>(112 + frames * stride * 4);
    const auto sector_extent = (payload_extent + 2047) & ~std::size_t(2047);
    if (bytes.size() != payload_extent &&
        (bytes.size() != sector_extent ||
         !std::all_of(bytes.begin() + payload_extent, bytes.end(), [](auto b) { return b == 0; })))
        throw std::runtime_error("Clip payload extent does not match frame count");
    std::copy_n(bytes.begin(), 112, clip.header_bytes.begin());
    std::size_t cursor = 112;
    auto vector = [&]() {
        std::array<float, 4> value{};
        for (auto& component : value) {
            component = std::bit_cast<float>(read_u32(bytes, cursor)); cursor += 4;
            if (!std::isfinite(component)) throw std::runtime_error("Nonfinite clip channel value");
        }
        return value;
    };
    clip.frames.reserve(static_cast<std::size_t>(frames));
    for (std::uint64_t index = 0; index < frames; ++index) {
        ClipFrame frame;
        frame.bones.resize(bone_count);
        for (unsigned bone = 0; bone < bone_count; ++bone) {
            const auto bit = std::uint32_t(1) << bone;
            auto& channel = frame.bones[bone];
            if (clip.translation_mask & bit) channel.translation = vector();
            if (clip.rotation_mask & bit) {
                if (bone == 0 && root_yaw) {
                    std::array<std::uint32_t, 4> words{};
                    for (auto& word : words) { word = read_u32(bytes, cursor); cursor += 4; }
                    if (!std::isfinite(std::bit_cast<float>(words[0])))
                        throw std::runtime_error("Nonfinite root yaw value");
                    frame.root_yaw_words = words;
                }
                channel.rotation = vector();
            }
        }
        clip.frames.push_back(std::move(frame));
    }
    if (cursor != payload_extent) throw std::runtime_error("Unconsumed animation clip payload");
    return clip;
}

void write_animation_clip(std::ostream& out, const AnimationClip& clip) {
    out << std::setprecision(9) << "{\"version\":" << clip.version
        << ",\"frame_intervals\":" << clip.frame_intervals << ",\"stride_words\":" << clip.stride_words
        << ",\"frame_period\":" << clip.frame_period << ",\"looping\":" << (clip.looping ? "true" : "false")
        << ",\"bone_count\":" << clip.bone_count << ",\"translation_mask\":" << clip.translation_mask
        << ",\"rotation_mask\":" << clip.rotation_mask << ",\"header_bytes\":[";
    for (std::size_t i = 0; i < clip.header_bytes.size(); ++i) {
        if (i) out << ',';
        out << unsigned(clip.header_bytes[i]);
    }
    out << "],\"frames\":[";
    auto vector = [&](const std::optional<std::array<float, 4>>& value) {
        if (!value) { out << "null"; return; }
        out << '[';
        for (std::size_t i = 0; i < 4; ++i) { if (i) out << ','; out << (*value)[i]; }
        out << ']';
    };
    for (std::size_t f = 0; f < clip.frames.size(); ++f) {
        if (f) out << ',';
        const auto& frame = clip.frames[f];
        out << "{\"root_yaw_words\":";
        if (!frame.root_yaw_words) out << "null";
        else {
            out << '[';
            for (std::size_t i = 0; i < 4; ++i) { if (i) out << ','; out << (*frame.root_yaw_words)[i]; }
            out << ']';
        }
        out << ",\"bones\":[";
        for (std::size_t b = 0; b < frame.bones.size(); ++b) {
            if (b) out << ',';
            out << "{\"translation\":"; vector(frame.bones[b].translation);
            out << ",\"rotation\":"; vector(frame.bones[b].rotation);
            out << '}';
        }
        out << "]}";
    }
    out << "]}";
}
}
