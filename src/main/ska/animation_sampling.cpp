#include "animation_sampling.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Animation sample requires finite arithmetic");
    return value;
}
void finite_vector(const std::array<float, 4>& vector) {
    for (const auto component : vector) checked(component);
}
float interpolate(float first, float second, float fraction, float complement) {
    const float second_product = checked(second * fraction);
    const float first_product = checked(first * complement);
    return checked(second_product + first_product);
}
std::array<float, 4> quaternion(const std::array<float, 4>& first,
                                const std::array<float, 4>& second,
                                float fraction, float complement) {
    std::array<float, 4> products{};
    for (std::size_t i = 0; i < products.size(); ++i) products[i] = checked(first[i] * second[i]);
    const float xy = checked(products[0] + products[1]);
    const float xyz = checked(xy + products[2]);
    const float dot = checked(xyz + products[3]);
    const bool opposite = dot < 0.0F;
    std::array<float, 4> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const float endpoint = opposite ? 0.0F - second[i] : second[i];
        result[i] = interpolate(first[i], endpoint, fraction, complement);
    }
    // The original canonicalizes W only along its negative-dot branch.
    if (opposite && result[3] < 0.0F) {
        for (auto& component : result) component = 0.0F - component;
    }
    return result;
}
}

NonRootClipSample sample_default_nonroot_bones(const AnimationClip& clip,
                                              const SkeletonDefinition& skeleton,
                                              float phase) {
    if (clip.looping && phase == 1.0F)
        throw std::runtime_error("Animation phase outside supported default interval");
    return sample_nonroot_bones(clip, skeleton, phase, {false, false, clip.looping});
}

NonRootClipSample sample_nonroot_bones(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, const ClipPlaybackControl& control) {
    checked(phase);
    if (clip.version != 3 || clip.bone_count == 0 || clip.bone_count > 32 ||
        skeleton.bones.size() != clip.bone_count)
        throw std::runtime_error("Animation sample clip/skeleton mismatch");
    const std::uint64_t frame_count = std::uint64_t(clip.frame_intervals) + (clip.looping ? 0 : 1);
    if (!clip.frame_intervals || frame_count > 10000 || clip.frames.size() != frame_count)
        throw std::runtime_error("Animation sample frame extent mismatch");
    if (clip.bone_count < 32 && ((clip.translation_mask | clip.rotation_mask) >> clip.bone_count))
        throw std::runtime_error("Animation sample mask exceeds skeleton");

    const auto cursor = select_clip_sample(clip, phase, 0, control);
    const auto index = cursor.frame_index, next = cursor.next_frame_index;
    NonRootClipSample result;
    result.frame_index = index;
    result.next_frame_index = next;
    result.fraction = cursor.fraction;
    const float complement = checked(1.0F - result.fraction);
    const auto& first = clip.frames[index];
    const auto& second = clip.frames[next];
    if (first.bones.size() != clip.bone_count || second.bones.size() != clip.bone_count)
        throw std::runtime_error("Animation sample selected a malformed frame");
    result.bones.reserve(clip.bone_count - 1);
    std::vector<bool> destinations(clip.bone_count, false);
    destinations[0] = true;
    for (std::uint32_t bone = 1; bone < clip.bone_count; ++bone) {
        const auto bit = std::uint32_t(1) << bone;
        const bool translation = (clip.translation_mask & bit) != 0;
        const bool rotation = (clip.rotation_mask & bit) != 0;
        const auto& a = first.bones[bone];
        const auto& b = second.bones[bone];
        if (a.translation.has_value() != translation || b.translation.has_value() != translation ||
            a.rotation.has_value() != rotation || b.rotation.has_value() != rotation)
            throw std::runtime_error("Animation sample channel/mask mismatch");
        SampledClipBone sample;
        sample.bone_index = control.mirror ? skeleton.bones[bone].record_bytes[1] : bone;
        if (sample.bone_index >= clip.bone_count || destinations[sample.bone_index])
            throw std::runtime_error("Animation mirror map is not a non-root permutation");
        destinations[sample.bone_index] = true;
        if (translation) {
            finite_vector(*a.translation);
            finite_vector(*b.translation);
            const auto& offset = skeleton.bones[sample.bone_index].local_offset;
            const std::array<float, 4> local{checked(offset.x), checked(offset.y), checked(offset.z), 0.0F};
            std::array<float, 4> value{};
            for (std::size_t component = 0; component < 3; ++component) {
                float blended = interpolate((*a.translation)[component], (*b.translation)[component],
                                                  result.fraction, complement);
                if (control.mirror && component == 0) blended = -blended;
                value[component] = checked(blended + local[component]);
            }
            value[3] = checked((*a.translation)[3] + local[3]);
            sample.translation = value;
        }
        if (rotation) {
            finite_vector(*a.rotation);
            finite_vector(*b.rotation);
            sample.rotation = quaternion(*a.rotation, *b.rotation, result.fraction, complement);
            if (control.mirror) {
                (*sample.rotation)[1] = -(*sample.rotation)[1];
                (*sample.rotation)[2] = -(*sample.rotation)[2];
            }
        }
        result.bones.push_back(std::move(sample));
    }
    std::sort(result.bones.begin(), result.bones.end(), [](const auto& a, const auto& b) {
        return a.bone_index < b.bone_index;
    });
    return result;
}

void write_nonroot_clip_sample(std::ostream& out, const NonRootClipSample& sample) {
    out << std::setprecision(9) << "{\"frame_index\":" << sample.frame_index
        << ",\"next_frame_index\":" << sample.next_frame_index
        << ",\"fraction\":" << sample.fraction << ",\"bones\":[";
    auto vector = [&](const std::optional<std::array<float, 4>>& value) {
        if (!value) { out << "null"; return; }
        out << '[';
        for (std::size_t i = 0; i < value->size(); ++i) {
            if (i) out << ',';
            out << (*value)[i];
        }
        out << ']';
    };
    for (std::size_t i = 0; i < sample.bones.size(); ++i) {
        if (i) out << ',';
        const auto& bone = sample.bones[i];
        out << "{\"bone_index\":" << bone.bone_index << ",\"translation\":";
        vector(bone.translation);
        out << ",\"rotation\":";
        vector(bone.rotation);
        out << '}';
    }
    out << "]}\n";
}
}
