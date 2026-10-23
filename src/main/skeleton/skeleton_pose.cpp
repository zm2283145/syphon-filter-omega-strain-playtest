#include "skeleton_pose.hpp"
#include <cmath>
#include <functional>
#include <iomanip>
#include <stdexcept>

namespace omega {
namespace {
using Matrix = std::array<float, 16>;
void finite(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite skeleton pose calculation");
}
Matrix compose(const Matrix& parent, const Matrix& local) {
    Matrix result{};
    for (unsigned column = 0; column < 4; ++column) {
        for (unsigned row = 0; row < 4; ++row) {
            float value = parent[row] * local[column * 4]; finite(value);
            for (unsigned k = 1; k < 4; ++k) {
                const float term = parent[k * 4 + row] * local[column * 4 + k]; finite(term);
                value = value + term; finite(value);
            }
            result[column * 4 + row] = value;
        }
    }
    return result;
}
Matrix inverse(const Matrix& model) {
    Matrix result = model;
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned row = 0; row < 3; ++row) result[column * 4 + row] = model[row * 4 + column];
    if (model[12] != 0 || model[13] != 0 || model[14] != 0) {
        const std::array<float, 3> negative{0.0F - model[12], 0.0F - model[13], 0.0F - model[14]};
        for (unsigned row = 0; row < 3; ++row) {
            float value = result[row] * negative[0];
            value = value + result[4 + row] * negative[1];
            value = value + result[8 + row] * negative[2];
            finite(value); result[12 + row] = value;
        }
    }
    return result;
}
}
std::vector<SkeletonBindBone> skeleton_bind_pose(const SkeletonDefinition& skeleton) {
    const auto count = skeleton.bones.size();
    if (!count || count > 256) throw std::runtime_error("Skeleton pose count outside supported range");
    for (const auto& bone : skeleton.bones) {
        if (bone.parent < -1 || bone.parent >= static_cast<int>(count))
            throw std::runtime_error("Invalid skeleton pose parent");
        finite(bone.local_offset.x); finite(bone.local_offset.y); finite(bone.local_offset.z);
    }
    std::vector<SkeletonBindBone> result(count);
    std::vector<unsigned char> visited(count);
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (visited[index] == 2) return;
        if (visited[index] == 1) throw std::runtime_error("Skeleton pose parent cycle");
        visited[index] = 1;
        const auto& bone = skeleton.bones[index];
        Matrix local{1,0,0,0, 0,1,0,0, 0,0,1,0,
                     bone.local_offset.x,bone.local_offset.y,bone.local_offset.z,1};
        auto& output = result[index];
        output.parent = bone.parent;
        if (bone.parent < 0) output.model = local;
        else {
            visit(static_cast<std::size_t>(bone.parent));
            output.model = compose(result[static_cast<std::size_t>(bone.parent)].model, local);
        }
        output.inverse_bind = inverse(output.model);
        visited[index] = 2;
    };
    for (std::size_t i = 0; i < count; ++i) visit(i);
    return result;
}
std::vector<std::array<float, 16>> skeleton_world_pose(const SkeletonDefinition& skeleton,
    std::span<const std::array<float, 16>> local_transforms) {
    const auto count = skeleton.bones.size();
    if (!count || count > 256 || local_transforms.size() != count)
        throw std::runtime_error("Animated hierarchy extent mismatch");
    for (std::size_t i = 0; i < count; ++i) {
        const auto parent = skeleton.bones[i].parent;
        if (parent < -1 || parent >= static_cast<int>(count))
            throw std::runtime_error("Invalid animated hierarchy parent");
        for (const auto value : local_transforms[i]) finite(value);
        const auto& m = local_transforms[i];
        if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
            throw std::runtime_error("Animated hierarchy requires affine transforms");
    }
    std::vector<Matrix> result(count);
    std::vector<unsigned char> visited(count);
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (visited[index] == 2) return;
        if (visited[index] == 1) throw std::runtime_error("Animated hierarchy parent cycle");
        visited[index] = 1;
        const auto parent = skeleton.bones[index].parent;
        if (parent < 0) result[index] = local_transforms[index];
        else {
            visit(static_cast<std::size_t>(parent));
            result[index] = compose(result[static_cast<std::size_t>(parent)], local_transforms[index]);
        }
        visited[index] = 2;
    };
    for (std::size_t i = 0; i < count; ++i) visit(i);
    return result;
}
void write_skeleton_bind_pose(std::ostream& out, std::span<const SkeletonBindBone> bones) {
    out << std::setprecision(9) << "{\"bones\":[";
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (i) out << ',';
        const auto& bone = bones[i];
        out << "{\"parent\":" << bone.parent << ",\"model\":[";
        for (std::size_t j = 0; j < 16; ++j) { if (j) out << ','; out << bone.model[j]; }
        out << "],\"inverse_bind\":[";
        for (std::size_t j = 0; j < 16; ++j) { if (j) out << ','; out << bone.inverse_bind[j]; }
        out << "]}";
    }
    out << "]}";
}
}
