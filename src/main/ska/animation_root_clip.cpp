#include "animation_root_clip.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Root clip requires finite input/arithmetic");
    return value;
}
void finite_vector(const RootVector& value) { for (float component : value) checked(component); }
float add(float a, float b) { return checked(a + b); }
float sub(float a, float b) { return checked(a - b); }
float mul(float a, float b) { return checked(a * b); }
std::uint32_t word(const AnimationClip& clip, std::size_t offset) {
    const auto& b = clip.header_bytes;
    return std::uint32_t(b[offset]) | (std::uint32_t(b[offset + 1]) << 8) |
        (std::uint32_t(b[offset + 2]) << 16) | (std::uint32_t(b[offset + 3]) << 24);
}
float dot(const RootVector& a, const RootVector& b) {
    return add(add(add(mul(a[0], b[0]), mul(a[1], b[1])), mul(a[2], b[2])), mul(a[3], b[3]));
}
float blend(float a, float b, float t, float complement) {
    return add(mul(b, t), mul(a, complement));
}
RootVector compose(const RootVector& a, const RootVector& b) {
    RootVector result;
    for (unsigned i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        result[i] = sub(add(add(mul(a[j], b[k]), mul(a[i], b[3])), mul(b[i], a[3])), mul(b[j], a[k]));
    }
    result[3] = sub(sub(sub(mul(a[3], b[3]), mul(a[2], b[2])), mul(a[1], b[1])), mul(a[0], b[0]));
    return result;
}
RootVector rotate(const RootVector& q, const RootVector& value) {
    RootVector first;
    const RootVector conjugate{sub(0, q[0]), sub(0, q[1]), sub(0, q[2]), q[3]};
    for (unsigned i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        first[i] = sub(add(mul(q[j], value[k]), mul(value[i], q[3])), mul(value[j], q[k]));
    }
    first[3] = sub(sub(sub(mul(q[3], 0), mul(q[2], value[2])), mul(q[1], value[1])), mul(q[0], value[0]));
    RootVector result{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        result[i] = sub(add(add(mul(first[j], conjugate[k]), mul(first[i], conjugate[3])),
            mul(conjugate[i], first[3])), mul(conjugate[j], first[k]));
    }
    result[3] = value[3];
    return result;
}
RootVector quaternion(const RootVector& a, const RootVector& b, float t, float complement) {
    finite_vector(a); finite_vector(b);
    const bool opposite = dot(a, b) < 0;
    RootVector result;
    for (unsigned i = 0; i < 4; ++i) result[i] = blend(a[i], opposite ? sub(0, b[i]) : b[i], t, complement);
    if (opposite && result[3] < 0) for (auto& component : result) component = sub(0, component);
    return result;
}
void accumulate(RootVector& target, const RootVector& source, float weight, bool subtract = false) {
    for (unsigned i = 0; i < 4; ++i) {
        const float weighted = mul(source[i], weight);
        target[i] = subtract ? sub(target[i], weighted) : add(target[i], weighted);
    }
}
void vector_json(std::ostream& out, const RootVector& value) {
    out << '[';
    for (unsigned i = 0; i < 4; ++i) { if (i) out << ','; out << value[i]; }
    out << ']';
}
void optional_json(std::ostream& out, const std::optional<RootVector>& value) {
    if (value) vector_json(out, *value); else out << "null";
}
void key_json(std::ostream& out, const RootAnchorKey& key) {
    out << '[';
    for (unsigned i = 0; i < 4; ++i) { if (i) out << ','; out << unsigned(key[i]); }
    out << ']';
}
}

RootClipSample sample_default_root_clip(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, float phase_advance) {
    if (clip.looping && phase == 1)
        throw std::runtime_error("Root clip phase outside supported default domain");
    return sample_root_clip(clip, skeleton, phase, phase_advance, {false, false, clip.looping});
}

RootClipSample sample_root_clip(const AnimationClip& clip,
    const SkeletonDefinition& skeleton, float phase, float phase_advance,
    const ClipPlaybackControl& control, const RootVector& incoming_rotation) {
    checked(phase); checked(phase_advance);
    finite_vector(incoming_rotation);
    if (clip.version != 3 || !clip.bone_count || clip.bone_count > 32 || skeleton.bones.size() != clip.bone_count)
        throw std::runtime_error("Root clip/skeleton mismatch");
    const std::uint64_t count = std::uint64_t(clip.frame_intervals) + (clip.looping ? 0 : 1);
    if (!clip.frame_intervals || count > 10000 || clip.frames.size() != count)
        throw std::runtime_error("Root clip frame extent mismatch");
    if (clip.bone_count < 32 && ((clip.translation_mask | clip.rotation_mask) >> clip.bone_count))
        throw std::runtime_error("Root clip mask exceeds skeleton");
    const auto cursor = select_clip_sample(clip, phase, phase_advance, control);
    RootClipSample result;
    result.frame_index = cursor.frame_index;
    result.next_frame_index = cursor.next_frame_index;
    result.fraction = cursor.fraction;
    result.frame_advance = cursor.frame_advance;
    std::copy_n(clip.header_bytes.begin() + 0x24, 4, result.anchor_key.begin());
    const float complement = sub(1, result.fraction);
    const auto& first = clip.frames[result.frame_index];
    const auto& second = clip.frames[result.next_frame_index];
    if (first.bones.size() != clip.bone_count || second.bones.size() != clip.bone_count)
        throw std::runtime_error("Root clip selected malformed frame");
    const bool has_t = (clip.translation_mask & 1) != 0, has_q = (clip.rotation_mask & 1) != 0;
    const bool has_yaw = has_q && word(clip, 0x18) != 0;
    const auto& a = first.bones[0];
    const auto& b = second.bones[0];
    if (a.translation.has_value() != has_t || b.translation.has_value() != has_t ||
        a.rotation.has_value() != has_q || b.rotation.has_value() != has_q ||
        first.root_yaw_words.has_value() != has_yaw || second.root_yaw_words.has_value() != has_yaw)
        throw std::runtime_error("Root clip channel/mask/header mismatch");
    result.bone0_translation = has_t;
    result.bone0_rotation = has_q;
    if (has_t) {
        finite_vector(*a.translation); finite_vector(*b.translation);
        RootVector pose;
        for (unsigned i = 0; i < 3; ++i) pose[i] = blend((*a.translation)[i], (*b.translation)[i], result.fraction, complement);
        pose[3] = add((*a.translation)[3], 0);
        if (control.mirror) pose[0] = -pose[0];
        if (word(clip, 0x14) != 0) {
            RootVector motion;
            for (unsigned i = 0; i < 4; ++i) motion[i] = mul(pose[i], result.frame_advance);
            for (unsigned i = 0; i < 3; ++i) {
                if (clip.header_bytes[0x14 + i]) pose[i] = 0;
                else motion[i] = 0;
            }
            result.motion = motion;
        }
        RootVector baseline{};
        if (word(clip, 0x28) != 0) {
            for (unsigned i = 0; i < 3; ++i)
                if (clip.header_bytes[0x14 + i]) baseline[i] = checked(std::bit_cast<float>(word(clip, 0x40 + 4 * i)));
        }
        if (control.mirror) baseline[0] = -baseline[0];
        const auto& offset = skeleton.bones[0].local_offset;
        const RootVector local{checked(offset.x), checked(offset.y), checked(offset.z), 0};
        for (unsigned i = 0; i < 4; ++i) pose[i] = add(add(pose[i], baseline[i]), local[i]);
        result.pose_translation = pose;
    }
    if (has_q) {
        if (has_yaw) {
            const float first_angle = checked(std::bit_cast<float>(first.root_yaw_words->at(0)));
            const float next_angle = checked(std::bit_cast<float>(second.root_yaw_words->at(0)));
            float difference = sub(next_angle, first_angle);
            const float radians = std::bit_cast<float>(std::uint32_t{0x3c8efa35});
            if (difference < mul(radians, -180)) difference = add(difference, mul(radians, 360));
            else if (difference > mul(radians, 180)) difference = add(difference, mul(radians, -360));
            float angle = add(first_angle, mul(difference, result.fraction));
            if (control.mirror) angle = -angle;
            result.angular_increment = mul(angle, result.frame_advance);
        }
        result.pose_quaternion = quaternion(*a.rotation, *b.rotation, result.fraction, complement);
        if (control.mirror) {
            (*result.pose_quaternion)[1] = -(*result.pose_quaternion)[1];
            (*result.pose_quaternion)[2] = -(*result.pose_quaternion)[2];
        }
        if (incoming_rotation[3] != 1) {
            result.pose_quaternion = compose(incoming_rotation, *result.pose_quaternion);
            if (result.pose_translation) result.pose_translation = rotate(incoming_rotation, *result.pose_translation);
            if (result.motion) result.motion = rotate(incoming_rotation, *result.motion);
        }
    }
    return result;
}

RootClipAccumulation accumulate_root_clip_contributions(std::span<const RootAnchorDefinition> anchors,
    std::span<const WeightedRootClipSample> contributions) {
    if (anchors.empty() || anchors.size() > 256 || contributions.size() > 10000)
        throw std::runtime_error("Root clip accumulator registry/contribution limit");
    RootClipAccumulation result;
    result.anchors.reserve(anchors.size());
    for (const auto& anchor : anchors) {
        if (std::any_of(result.anchors.begin(), result.anchors.end(), [&](const auto& row) { return row.key == anchor.key; }))
            throw std::runtime_error("Root clip accumulator duplicate anchor");
        result.anchors.push_back({anchor.key, {}});
    }
    for (const auto& contribution : contributions) {
        const float weight = checked(contribution.weight);
        if (weight < 0) throw std::runtime_error("Root clip accumulator negative weight unsupported");
        if (weight == 0) continue;
        const auto& sample = contribution.sample;
        const auto found = std::find_if(result.anchors.begin(), result.anchors.end(), [&](const auto& row) { return row.key == sample.anchor_key; });
        if (found == result.anchors.end()) throw std::runtime_error("Root clip accumulator unresolved anchor");
        if (sample.bone0_translation != sample.pose_translation.has_value() ||
            sample.bone0_rotation != sample.pose_quaternion.has_value() ||
            (sample.motion && !sample.pose_translation) || (sample.angular_increment && !sample.pose_quaternion))
            throw std::runtime_error("Root clip accumulator inconsistent sample channels");
        if (contribution.motion_bucket > 3) throw std::runtime_error("Root clip accumulator unsupported motion bucket");
        auto& frame = found->frame;
        frame.weight = add(frame.weight, weight);
        if (sample.angular_increment) frame.angle = add(frame.angle, mul(checked(*sample.angular_increment), weight));
        if (sample.pose_quaternion) {
            finite_vector(*sample.pose_quaternion);
            bool subtract = false;
            if (contribution.align_sign) {
                const auto& reference = anchors[static_cast<std::size_t>(found - result.anchors.begin())].sign_reference;
                if (!reference) throw std::runtime_error("Root clip accumulator unset sign reference");
                finite_vector(*reference);
                subtract = dot(*sample.pose_quaternion, *reference) < 0;
            }
            accumulate(frame.pose_quaternion, *sample.pose_quaternion, weight, subtract);
            accumulate(result.bone0_rotation_accumulator, {0, 0, 0, 1}, weight);
            result.bone0_rotation_applied = true;
        }
        if (sample.pose_translation) {
            finite_vector(*sample.pose_translation);
            accumulate(frame.pose_translation, *sample.pose_translation, weight);
            if (sample.motion) {
                finite_vector(*sample.motion);
                accumulate(frame.motion_buckets[contribution.motion_bucket], *sample.motion, weight);
            }
            // Original bone0 helper receives a zero vector, then forces W=1.
            result.bone0_translation_accumulator[3] = 1;
            result.bone0_translation_applied = true;
        }
    }
    return result;
}

void write_root_clip_sample(std::ostream& out, const RootClipSample& sample) {
    out << std::setprecision(9) << "{\"anchor_key\":"; key_json(out, sample.anchor_key);
    out << ",\"frame_index\":" << sample.frame_index << ",\"next_frame_index\":" << sample.next_frame_index
        << ",\"fraction\":" << sample.fraction << ",\"frame_advance\":" << sample.frame_advance;
    out << ",\"pose_translation\":"; optional_json(out, sample.pose_translation);
    out << ",\"pose_quaternion\":"; optional_json(out, sample.pose_quaternion);
    out << ",\"motion\":"; optional_json(out, sample.motion);
    out << ",\"angular_increment\":";
    if (sample.angular_increment) out << *sample.angular_increment; else out << "null";
    out << ",\"bone0_translation\":" << (sample.bone0_translation ? "true" : "false")
        << ",\"bone0_rotation\":" << (sample.bone0_rotation ? "true" : "false") << '}';
}
void write_root_clip_accumulation(std::ostream& out, const RootClipAccumulation& value) {
    out << std::setprecision(9) << "{\"anchors\":[";
    for (std::size_t i = 0; i < value.anchors.size(); ++i) {
        if (i) out << ',';
        const auto& row = value.anchors[i]; const auto& frame = row.frame;
        out << "{\"key\":"; key_json(out, row.key);
        out << ",\"frame\":{\"weight\":" << frame.weight << ",\"pose_quaternion\":"; vector_json(out, frame.pose_quaternion);
        out << ",\"pose_translation\":"; vector_json(out, frame.pose_translation);
        out << ",\"angle\":" << frame.angle << ",\"motion_buckets\":[";
        for (unsigned bucket = 0; bucket < 4; ++bucket) { if (bucket) out << ','; vector_json(out, frame.motion_buckets[bucket]); }
        out << "]}}";
    }
    out << "],\"bone0_translation_accumulator\":"; vector_json(out, value.bone0_translation_accumulator);
    out << ",\"bone0_rotation_accumulator\":"; vector_json(out, value.bone0_rotation_accumulator);
    out << ",\"bone0_translation_applied\":" << (value.bone0_translation_applied ? "true" : "false")
        << ",\"bone0_rotation_applied\":" << (value.bone0_rotation_applied ? "true" : "false") << '}';
}
}
