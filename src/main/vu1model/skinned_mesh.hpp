#pragma once
#include "geometry.hpp"
#include "morph.hpp"
#include <optional>

namespace omega {
using SkinMatrix = std::array<float, 16>; // Column major, model space.
struct SkinVertex {
    Vec3 position{}, normal{};
    std::array<float, 2> uv{};
};
struct SkinInfluenceGroup {
    std::uint16_t output_address{}, palette_address{};
    std::uint32_t bone{}, first_vertex{};
    std::vector<float> weights; // One per affected vertex; single group uses implicit ones.
};
struct SkinPrimitive {
    std::uint32_t vertex_index{}; // Absolute retained output slot, not chunk-local.
    bool adc{}; // Suppress this triangle, retaining strip history.
    std::uint8_t raw_index{};
};
struct SkinChunk {
    std::uint32_t packet_quadwords{}, output_base{};
    std::uint16_t control_flags{}, microprogram_entry{};
    std::vector<std::uint16_t> microprogram_entries; // Ordered invocations, including render-only suffixes.
    std::optional<std::array<std::uint32_t, 4>> initial_constants;
    std::vector<SkinVertex> vertices;
    std::vector<SkinInfluenceGroup> groups; // Original execution order.
    std::vector<SkinPrimitive> primitives;
    // VU qwords from 4 + 2*vertices as the weight UNPACK (raw words, group
    // padding included) and the signed V4-8 primitive UNPACK (every byte,
    // padding included, sign-extended) leave them.
    std::vector<std::array<std::uint32_t, 4>> unpack_tail;
};
struct SkinnedMesh {
    std::uint32_t bone_count{};
    std::vector<SkinChunk> chunks;
};
struct SkinPaletteBone {
    SkinMatrix position{}, normal{}; // Normal uses only first three columns.
};
// The proved base-shape branch bypasses delta streams. Nonzero counts use the
// separate deformation API with explicit external payloads and ordered weights.
struct SkinMorphContext { std::uint32_t active_targets{}; };
struct SkinMorphTarget { std::size_t shape_index{}; float weight{}; };
struct SkinnedVertex {
    std::array<float, 4> position{}, normal{};
    std::array<float, 2> uv{};
};
struct SkinnedMeshGeometry {
    std::vector<SkinnedVertex> vertices; // New versions appended for each chunk's writes.
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::uint32_t> triangle_chunks;
};
// Bounded COMMON packet layout: format 3, 1..21 bones. No material or render flags
// are inferred from on-disc bytes. All game-derived results belong under local/.
SkinnedMesh decode_skinned_mesh(std::span<const std::uint8_t> bytes,
                               std::uint32_t bone_count = 21);
// Port-side speed only (132vc1): the chunks skinned on the PS Vita's NEON path and on the scalar path since the start
// (both 0 where the NEON path is not built).
void skin_neon_counts(std::uint64_t& neon, std::uint64_t& scalar);
// Port-only (132vc1 hw6, port-side speed only; the same bits): left * right column by column on Advanced SIMD (the
// PS Vita), each product and sum rounded as the scalar matrix products of make_skin_palette and the character's
// placement do (VMUL then VADD in their order, flush-to-zero as the play threads' VFP mode). False, `out` unspecified,
// when it is not available (another target, another FPSCR mode) or when an input, a product or a sum is an infinity
// or NaN: the caller then runs its scalar product, which gives its result or its error as before.
bool skin_matrix_multiply_neon(const SkinMatrix& left, const SkinMatrix& right, SkinMatrix& out);
// Model-space palette: W*(S*B), normal W*B. Empty scales means identity.
std::vector<SkinPaletteBone> make_skin_palette(std::span<const SkinMatrix> world,
    std::span<const SkinMatrix> inverse_bind, std::span<const SkinMatrix> scales = {});
// Ordered model-space weighting; no weight or normal normalization. Single-bone
// rendering transforms all input vertices, even if group.Y is smaller. This is not
// VU projection, lighting, or pixel-accurate original rendering.
// Morph entries 16/18/20/22 require explicit zero-target base-shape context.
// Other admitted entries are 8/10/24/26/28/30; unknown programs fail at skinning.
// The flare entries 12/14 transform every vertex by the first record's position
// matrix only and leave the normal zero (the program writes colour 0x7f, unlit).
// Entries 8/10 and 16/18 require a single influence group.
std::vector<SkinnedVertex> skin_mesh_chunk(const SkinChunk& chunk,
    std::span<const SkinPaletteBone> palette, std::optional<SkinMorphContext> morph = std::nullopt);
// Processes retained output slots across chunks and resolves each triangle to
// the vertex versions present at that submission, before subsequent overwrites.
SkinnedMeshGeometry skin_skinned_mesh(const SkinnedMesh& mesh,
    std::span<const SkinPaletteBone> palette, std::optional<SkinMorphContext> morph = std::nullopt);
// Checkpoint 132win10 (port-side speed only): skin_skinned_mesh into caller-owned geometry (cleared first, its
// vectors' capacity reused across frames); the result is identical.
void skin_skinned_mesh_into(const SkinnedMesh& mesh, std::span<const SkinPaletteBone> palette,
    std::optional<SkinMorphContext> morph, SkinnedMeshGeometry& output);
// Ordered unsigned-byte shape deltas applied before palette skinning. Returns
// copied attributes; indices, influences, UVs and command metadata are retained.
// Face targets affect XYZ and the first packed vector's W (normal Z). Hand
// targets affect XYZ only; target order is VU bank 69 then bank 85.
SkinnedMesh deform_skinned_mesh(const SkinnedMesh& mesh, const MorphData& morph,
    std::span<const SkinMorphTarget> targets);
SkinnedMeshGeometry skin_morphed_mesh(const SkinnedMesh& mesh,
    std::span<const SkinPaletteBone> palette, const MorphData& morph,
    std::span<const SkinMorphTarget> targets);
// skin_morphed_mesh into caller-owned geometry; scratch keeps the deformed mesh's storage between calls.
void skin_morphed_mesh_into(const SkinnedMesh& mesh, std::span<const SkinPaletteBone> palette, const MorphData& morph,
    std::span<const SkinMorphTarget> targets, SkinnedMesh& scratch, SkinnedMeshGeometry& output);
std::vector<std::array<std::uint32_t, 3>> skinned_mesh_triangles(const SkinChunk& chunk);
void write_skinned_mesh(std::ostream& output, const SkinnedMesh& mesh);
}
