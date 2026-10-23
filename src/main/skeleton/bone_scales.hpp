#pragma once
#include "skinned_mesh.hpp"
namespace omega {
struct BoneScalePreset {
    std::vector<Vec3> scales;
    std::vector<std::uint32_t> unused_words;
    std::size_t stored_bytes{};
};
// Original preset consumer reads XYZ from 16-byte records. Fourth words and
// bounded archive storage padding have no inferred effect on bone transforms.
BoneScalePreset decode_bone_scales(std::span<const std::uint8_t> bytes,unsigned bones=21);
std::vector<SkinMatrix> bone_scale_matrices(const BoneScalePreset& preset);
void write_bone_scales(std::ostream& output,const BoneScalePreset& preset);
}
