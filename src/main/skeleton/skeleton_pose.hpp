#pragma once
#include "skeleton.hpp"

namespace omega {
struct SkeletonBindBone {
    int parent{-1};
    std::array<float, 16> model{}, inverse_bind{};
};
// Original initial, unit-scale, model-space pose. No actor placement or animation.
std::vector<SkeletonBindBone> skeleton_bind_pose(const SkeletonDefinition& skeleton);
// Animated model-space hierarchy. Input and output matrices are column-major.
// Actor placement is separate; forward parent references are supported.
std::vector<std::array<float, 16>> skeleton_world_pose(const SkeletonDefinition& skeleton,
    std::span<const std::array<float, 16>> local_transforms);
void write_skeleton_bind_pose(std::ostream& output, std::span<const SkeletonBindBone> bones);
}
