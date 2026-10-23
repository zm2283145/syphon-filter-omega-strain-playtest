#pragma once
#include "animation_sampling.hpp"

namespace omega {
struct NonRootBonePose {
    std::uint32_t bone_index{};
    Vec3 rest_offset;
    std::array<float, 16> local{};
    std::array<float, 4> translation_cache{}, rotation_cache{};
    std::array<float, 4> translation_accumulator{}, rotation_accumulator{};
    bool translation_applied{}, rotation_applied{};
};
struct NonRootPose {
    std::vector<NonRootBonePose> bones;
};
struct WeightedNonRootSample {
    float weight{};
    NonRootClipSample sample;
};

// Constructor-derived, unscaled skeleton domain: saved inverse-bind bases are
// identity. Arbitrarily rotated/scaled saved bind bases are not represented.
NonRootPose make_initial_nonroot_pose(const SkeletonDefinition& skeleton);
// Ordered contribution accumulation followed by retained-state finalization.
// On rejection, state remains unchanged. Zero weights skip sample inspection.
void update_nonroot_pose(NonRootPose& state, std::span<const WeightedNonRootSample> contributions);
void write_nonroot_pose(std::ostream& output, const NonRootPose& state);
}
