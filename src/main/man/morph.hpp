#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace omega {
enum class MorphKind { face, hand };
struct MorphShape {
    std::vector<std::optional<std::vector<std::uint8_t>>> chunks;
};
struct MorphData {
    MorphKind kind{};
    std::uint32_t chunk_count{};
    std::vector<MorphShape> shapes;
};
using ShapeNames = std::vector<std::array<std::uint8_t,16>>;
ShapeNames decode_shape_names(std::span<const std::uint8_t> bytes);
// Raw unsigned VIF input bytes, not already-scaled vertex displacements.
MorphData decode_morph(std::span<const std::uint8_t> bytes, MorphKind kind,
                      std::uint32_t shape_count, std::uint32_t chunk_count);
void write_shape_names(std::ostream& output, const ShapeNames& names);
void write_morph(std::ostream& output, const MorphData& morph);
}
