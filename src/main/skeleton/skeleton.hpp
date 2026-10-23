#pragma once
#include "geometry.hpp"

namespace omega {
struct SkeletonBone {
    int parent{-1};
    Vec3 local_offset;
    std::array<std::uint8_t, 32> record_bytes{}; // Unnamed fields remain uninterpreted.
};
struct SkeletonDefinition {
    std::array<std::uint32_t, 3> header_words{};
    std::vector<SkeletonBone> bones;
};
// Structural SKEL reader only; local offsets are not final skinned/rest transforms.
SkeletonDefinition decode_skeleton(std::span<const std::uint8_t> bytes);
void write_skeleton(std::ostream& output, const SkeletonDefinition& skeleton);
}
