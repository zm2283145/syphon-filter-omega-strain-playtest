#include "animation_pose.hpp"
#include "vu_float.hpp"
#include "skeleton_pose.hpp"
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Animation pose requires finite arithmetic");
    return value;
}
template<std::size_t N> void finite(const std::array<float, N>& value) {
    for (float component : value) checked(component);
}
bool zero(const std::array<float, 4>& value) {
    return value[0] == 0.0F && value[1] == 0.0F && value[2] == 0.0F && value[3] == 0.0F;
}
std::array<float, 4> normalize(const std::array<float, 4>& value) {
    std::array<float, 4> square{};
    for (std::size_t i = 0; i < 4; ++i) square[i] = checked(value[i] * value[i]);
    const float xy = checked(square[0] + square[1]);
    const float xyz = checked(xy + square[2]);
    const float norm2 = checked(xyz + square[3]);
    if (norm2 <= 0.0F) throw std::runtime_error("Animation pose quaternion norm underflow");
    // Rotation finalizer 003ba2b0 at 003ba320: VU RSQRT
    // Q = vf0.w/sqrt(norm2), then vmulq.xyzw.
    const float reciprocal = checked(vu::rsqrt(norm2));
    std::array<float, 4> result{};
    for (std::size_t i = 0; i < 4; ++i) result[i] = checked(value[i] * reciprocal);
    return result;
}
void apply_basis(NonRootBonePose& bone, const std::array<float, 4>& q) {
    std::array<float, 4> doubled{}, x{}, y{}, z{};
    for (std::size_t i = 0; i < 4; ++i) doubled[i] = checked(q[i] + q[i]);
    for (std::size_t i = 0; i < 4; ++i) {
        x[i] = checked(q[i] * doubled[0]);
        y[i] = checked(q[i] * doubled[1]);
        z[i] = checked(q[i] * doubled[2]);
    }
    auto diagonal = [](float first, float second) {
        return checked(checked(1.0F - first) - second);
    };
    const std::array<float, 12> basis{
        diagonal(y[1], z[2]), checked(x[1] + z[3]), checked(x[2] - y[3]), 0,
        checked(y[0] - z[3]), diagonal(x[0], z[2]), checked(y[2] + x[3]), 0,
        checked(z[0] + y[3]), checked(z[1] - x[3]), diagonal(x[0], y[1]), 0};
    for (std::size_t i = 0; i < basis.size(); ++i) bone.local[i] = basis[i];
}
void validate_retained(const NonRootBonePose& bone, std::size_t index) {
    if (bone.bone_index != index + 1) throw std::runtime_error("Animation pose bone indices must be consecutive");
    checked(bone.rest_offset.x); checked(bone.rest_offset.y); checked(bone.rest_offset.z);
    finite(bone.local); finite(bone.translation_cache); finite(bone.rotation_cache);
    if (bone.local[3] != 0 || bone.local[7] != 0 || bone.local[11] != 0 || bone.local[15] != 1)
        throw std::runtime_error("Animation pose requires affine local transforms");
}
}

NonRootPose make_initial_nonroot_pose(const SkeletonDefinition& skeleton) {
    if (skeleton.bones.empty() || skeleton.bones.size() > 32)
        throw std::runtime_error("Animation pose skeleton must have 1..32 bones");
    // Reuse the verified hierarchy/extent checks and prove that this source
    // remains within the identity saved-basis initialization domain.
    const auto bind = skeleton_bind_pose(skeleton);
    const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (const auto& bone : bind)
        for (std::size_t i = 0; i < identity.size(); ++i)
            if (bone.inverse_bind[i] != identity[i])
                throw std::runtime_error("Animation pose saved bind basis is outside supported initial domain");
    NonRootPose state;
    state.bones.reserve(skeleton.bones.size() - 1);
    for (std::size_t index = 1; index < skeleton.bones.size(); ++index) {
        NonRootBonePose bone;
        bone.bone_index = static_cast<std::uint32_t>(index);
        bone.rest_offset = skeleton.bones[index].local_offset;
        for (std::size_t i = 0; i < identity.size(); ++i) bone.local[i] = identity[i];
        bone.local[12] = bone.rest_offset.x;
        bone.local[13] = bone.rest_offset.y;
        bone.local[14] = bone.rest_offset.z;
        bone.local[15] = 1;
        state.bones.push_back(bone);
    }
    return state;
}

void update_nonroot_pose(NonRootPose& state, std::span<const WeightedNonRootSample> contributions) {
    if (state.bones.size() > 31 || contributions.size() > 10000)
        throw std::runtime_error("Animation pose update exceeds bounded domain");
    NonRootPose next = state;
    for (std::size_t i = 0; i < next.bones.size(); ++i) {
        auto& bone = next.bones[i];
        validate_retained(bone, i);
        bone.translation_accumulator = {};
        bone.rotation_accumulator = {};
        bone.translation_applied = false;
        bone.rotation_applied = false;
    }
    for (const auto& contribution : contributions) {
        const float weight = checked(contribution.weight);
        if (weight < 0) throw std::runtime_error("Negative animation contribution weights are unsupported");
        if (weight == 0) continue;
        if (contribution.sample.bones.size() != next.bones.size())
            throw std::runtime_error("Animation pose contribution bone count mismatch");
        for (std::size_t index = 0; index < next.bones.size(); ++index) {
            auto& bone = next.bones[index];
            const auto& sample = contribution.sample.bones[index];
            if (sample.bone_index != bone.bone_index)
                throw std::runtime_error("Animation pose contribution bone order mismatch");
            if (sample.translation) {
                finite(*sample.translation);
                for (std::size_t component = 0; component < 3; ++component) {
                    const float product = checked((*sample.translation)[component] * weight);
                    bone.translation_accumulator[component] = checked(bone.translation_accumulator[component] + product);
                }
                bone.translation_accumulator[3] = 1;
            }
            if (sample.rotation) {
                finite(*sample.rotation);
                for (std::size_t component = 0; component < 4; ++component) {
                    const float product = checked((*sample.rotation)[component] * weight);
                    bone.rotation_accumulator[component] = checked(bone.rotation_accumulator[component] + product);
                }
            }
        }
    }
    for (auto& bone : next.bones) {
        if (!zero(bone.translation_accumulator)) {
            bone.translation_accumulator[3] = 1;
            for (std::size_t i = 0; i < 4; ++i) bone.local[12 + i] = bone.translation_accumulator[i];
            bone.translation_cache = bone.translation_accumulator;
            bone.translation_applied = true;
        } else if (zero(bone.translation_cache)) {
            bone.translation_cache = {bone.rest_offset.x, bone.rest_offset.y, bone.rest_offset.z, 1};
        }
        if (!zero(bone.rotation_accumulator)) {
            bone.rotation_accumulator = normalize(bone.rotation_accumulator);
            apply_basis(bone, bone.rotation_accumulator);
            bone.rotation_cache = bone.rotation_accumulator;
            bone.rotation_applied = true;
        } else if (zero(bone.rotation_cache)) {
            // Matrix->quaternion plus conjugation yields identity for the
            // constructor-derived saved inverse-bind basis represented here.
            bone.rotation_cache = {0, 0, 0, 1};
        }
    }
    state = std::move(next);
}

void write_nonroot_pose(std::ostream& out, const NonRootPose& state) {
    out << std::setprecision(9) << "{\"bones\":[";
    auto vector = [&](const auto& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << values[i]; }
        out << ']';
    };
    for (std::size_t i = 0; i < state.bones.size(); ++i) {
        if (i) out << ',';
        const auto& bone = state.bones[i];
        out << "{\"bone_index\":" << bone.bone_index << ",\"local\":";
        vector(bone.local);
        out << ",\"translation_cache\":"; vector(bone.translation_cache);
        out << ",\"rotation_cache\":"; vector(bone.rotation_cache);
        out << ",\"translation_accumulator\":"; vector(bone.translation_accumulator);
        out << ",\"rotation_accumulator\":"; vector(bone.rotation_accumulator);
        out << ",\"translation_applied\":" << (bone.translation_applied ? "true" : "false")
            << ",\"rotation_applied\":" << (bone.rotation_applied ? "true" : "false") << '}';
    }
    out << "]}\n";
}
}
