#pragma once
// Port-only (checkpoint 132md2, research/MODDING.md "Stage 3"): character model replacement through glTF 2.0. Not
// game code. The original skeleton (SKEL) and a skinned part (SKM) export as a glTF rigged to that skeleton; a mod's
// glTF rigged to the same skeleton loads as a ReplacementModel (validated against the skeleton: bone count, names,
// hierarchy and bind pose within tolerance) and is skinned by the game's committed palette in place of the part.
//
// Space: the game's model space is Z-up with the character facing +Y; glTF is Y-up facing +Z. The conversion is the
// rotation (x, y, z) -> (-x, z, y), its own inverse. Joints are named bone_NN (NN the SKEL record index, two digits;
// a "_<anything>" suffix is allowed on import, e.g. bone_14_head); the bind pose is the SKEL's (translations only).

#include "gltf.hpp"
#include "image_files.hpp"
#include "morph.hpp"
#include "skeleton.hpp"
#include "skinned_mesh.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::gfx {

inline constexpr std::size_t replacement_vertex_limit = 1u << 20;
inline constexpr std::size_t replacement_influence_limit = 8;

struct ReplacementVertex {
    std::array<float, 3> position{}, normal{};   // game model space, bind pose
    std::array<float, 2> uv{};
    std::array<std::uint8_t, replacement_influence_limit> bones{};
    std::array<float, replacement_influence_limit> weights{};   // normalised (sum 1)
    std::uint8_t count = 0;
};
// Checkpoint 132md3: a morph target (glTF primitive "targets", named by mesh.extras.targetNames, Blender's shape keys):
// the vertices it moves and their game bind-space position / normal offsets at weight 1.
struct ReplacementMorph {
    std::string name;
    std::vector<std::uint32_t> vertices;   // ascending
    std::vector<std::array<float, 3>> position, normal;
};
struct ReplacementModel {
    std::vector<ReplacementVertex> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::optional<mods::RgbaImage> image;   // the first textured material's base colour (tinted by its factor)
    std::size_t bone_count = 0;
    std::vector<ReplacementMorph> morphs;   // distinct names, in first-seen order
    // Port-only (checkpoint 132md8): per bone up to the largest one a vertex names, the bind-space box (minimum xyz,
    // maximum xyz) of the vertices it influences (an unused bone: minimum > maximum); load_replacement fills it
    // (replacement_bone_boxes). Empty: computed when needed.
    std::vector<std::array<float, 6>> bone_boxes;
    // Port-only (checkpoint 132md11, research/MODDING.md "Speed"): coarser levels of a dense part (build_replacement_lods;
    // empty: drawn whole), finest first, each indexing `vertices`; drawn on the GPU skinning path by screen size.
    struct Lod {
        std::vector<std::array<std::uint32_t, 3>> triangles;
        float error = 0;   // simplify_levels' estimate of its distance from the full part, in model units
    };
    std::vector<Lod> lods;
};

// The game-space <-> glTF-space rotation (its own inverse).
std::array<float, 3> to_gltf(std::array<float, 3> v);
inline std::array<float, 3> from_gltf(std::array<float, 3> v) { return to_gltf(v); }

// The bind tolerance: 0.5% of the skeleton's bind extent (largest bone distance from the root), at least 1e-3.
float bind_tolerance(const SkeletonDefinition& skeleton);

// Checks a document's skin against the skeleton. Returns, for each bone, the document node of its joint; throws
// std::runtime_error naming the first mismatch (count, a name, the hierarchy, a bind translation).
std::vector<std::size_t> validate_skeleton(const gltf::Document& doc, const SkeletonDefinition& skeleton);

// The replacement from a validated document: every primitive of every skinned mesh node merged (triangles; strips and
// fans converted), vertices brought to bind space (the joints' rest skinning, then game space), weights renormalised,
// missing normals computed, missing UVs zero. A document with no mesh is an empty (hidden) part. Throws on any
// mismatch or unsupported content.
ReplacementModel load_replacement(const gltf::Document& doc, const SkeletonDefinition& skeleton);
ReplacementModel load_replacement(const std::filesystem::path& file, const SkeletonDefinition& skeleton);

// Skins the replacement by `palette` (the character's committed, placed palette) into `output` (cleared first; one
// triangle_chunks entry of 0 per triangle).
void skin_replacement_into(const ReplacementModel& model, std::span<const SkinPaletteBone> palette, SkinnedMeshGeometry& output);
// Port-only (checkpoint 132md7, research/MODDING.md "Speed"): vertex v of skin_replacement_into's output (position p,
// normal q; `offsets`: replacement_morph_offsets' or null), by the same arithmetic, for a caller writing its own
// vertices (a draw split across a worker pool). Throws on a bone outside the palette.
void skin_replacement_vertex(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                             const std::vector<std::array<float, 3>>* offsets, std::size_t v, std::array<float, 3>& p,
                             std::array<float, 3>& q);
// One replaced part of a draw: its model, palette and morph offsets (null: none).
struct ReplacementDraw {
    const ReplacementModel* model = nullptr;
    std::vector<SkinPaletteBone> palette;
    const std::vector<std::array<float, 3>>* offsets = nullptr;
};
// skin_replacement_into / skin_morphed_replacement_into of a draw (its offsets: the morph's).
void skin_replacement_draw_into(const ReplacementDraw& draw, SkinnedMeshGeometry& output);

// Port-only (checkpoint 132md8, research/MODDING.md "Speed"): GPU skinning of a replacement part. The part's bind
// vertices go to the GPU once (GpuSkinVertex, 72 bytes); each draw uploads its palette as rows (gpu_palette_rows) and
// the vertex shader skins as skin_replacement_vertex does (the same sums, in the GPU's float order). A part with a
// morph (a face or hand), no triangles or a palette past gpu_palette_limit stays on the CPU path.
inline constexpr std::size_t gpu_palette_limit = 128;
struct GpuSkinVertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, replacement_influence_limit> bones{};   // past the vertex's count: 0
    std::array<float, replacement_influence_limit> weights{};        // past the vertex's count: 0
};
static_assert(sizeof(GpuSkinVertex) == 72);
// Rows per bone in gpu_palette_rows: the position matrix's rows 0..2 (x, y, z, translation), and the normal's (w 0).
inline constexpr std::size_t gpu_palette_rows_per_bone = 6;
std::vector<GpuSkinVertex> gpu_skin_vertices(const ReplacementModel& model);
// `rows` (resized to 6 * gpu_palette_limit float4, the shader's arrays): bone b's position rows at 3b..3b+2, its
// normal rows at 3 * (gpu_palette_limit + b)..; the rest 0. Throws past gpu_palette_limit bones. (Two arrays of
// float3x4, not 6 rows per bone in one: FXC took 3 s or more to compile the latter's indexing, against 0.3 s.)
void gpu_palette_rows(std::span<const SkinPaletteBone> palette, std::vector<std::array<float, 4>>& rows);
// The bones the vertices name: the largest + 1 (0: no vertex).
std::size_t replacement_bones_used(const ReplacementModel& model);
// The per-bone bind boxes of ReplacementModel::bone_boxes.
std::vector<std::array<float, 6>> replacement_bone_boxes(const ReplacementModel& model);
// The part skins on the GPU with a palette of `palette_size` bones.
bool gpu_skinnable(const ReplacementModel& model, std::size_t palette_size);
// A box holding every vertex skinned by `palette` (no morph): the union over the used bones of each bind box moved by
// its bone's position matrix (its 8 corners). Conservative: a skinned vertex is a weighted mean (weights summing to 1)
// of its bones' moved positions. False: no vertex.
bool replacement_bounds(const ReplacementModel& model, std::span<const SkinPaletteBone> palette, std::array<float, 3>& minimum,
                        std::array<float, 3>& maximum);
// Port-only (checkpoint 132md11, research/MODDING.md "Speed"): automatic levels of detail. A part without morphs with at
// least replacement_lod_triangles triangles gets mesh_simplify.hpp's levels (each about a quarter of the previous one,
// down to 512 triangles); the original parts have at most about 2000. Returns the levels made.
inline constexpr std::size_t replacement_lod_triangles = 4096;
std::size_t build_replacement_lods(ReplacementModel& model);
// The level to draw at `pixels_per_unit` (screen pixels per model unit at the part, at its nearest): 0 the full part,
// k lods[k - 1], the coarsest whose error is at most `max_pixels` on screen (infinite / NaN scale: the full part).
inline constexpr float replacement_lod_pixels = 0.5F;
std::size_t replacement_lod(const ReplacementModel& model, float pixels_per_unit, float max_pixels = replacement_lod_pixels);

// The night ghosts of a GPU-skinned part (play_main's NightTrail): a bone's position matrix taken into a frame's camera
// space as the CPU history keeps a vertex (`frame`: the camera's three axes, then its position; x and z over
// t = tan(half fov)), and such a matrix back into the world of another frame (a ghost: where the old palette put the
// part on screen).
SkinMatrix ghost_camera_matrix(const SkinMatrix& position, const std::array<std::array<float, 4>, 4>& frame, float t);
SkinMatrix ghost_world_matrix(const SkinMatrix& camera, const std::array<std::array<float, 4>, 4>& frame, float t);

// Checkpoint 132md3 (research/MODDING.md "Face and hand shapes"): a morph target's name for shape `index` of a part's
// shape table (MORPH.HOG MALE.BIN / FEMALE.BIN for a face, <g>HAND<h>.BIN for a hand): the 16-byte key up to its NUL;
// a key that repeats an earlier one gets "@<index>" (the game finds a key's first entry, 003ffa70).
std::string shape_target_name(const ShapeNames& names, std::size_t index);
// The model's morphs bound to a shape table: per shape index, the morph drawn for it (an index into model.morphs) or -1.
// A name resolves as shape_target_name writes it ("NAME" the first entry with that key, "NAME@<n>" entry n); a name
// the table lacks, or two morphs on one shape, throws.
std::vector<std::int16_t> bind_morphs(const ReplacementModel& model, const ShapeNames& names);
// skin_replacement_into after the part's morph: `targets` (the original's per-frame shape slots, morph_face_targets /
// morph_hand_targets) through `shapes` (bind_morphs), each vertex offset by the weighted sum of its morphs in slot
// order (the original's order, skinned_mesh.cpp deform_skinned_mesh_into) before skinning. `scratch` is reused storage.
void skin_morphed_replacement_into(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                                   std::span<const std::int16_t> shapes, std::span<const SkinMorphTarget> targets,
                                   std::vector<std::array<float, 3>>& scratch, SkinnedMeshGeometry& output);
// 132md7: that morph's per-vertex offsets (position at v, normal at n + v) in `scratch`; null when no bound target is
// weighted (the vertices are skinned as they are).
const std::vector<std::array<float, 3>>* replacement_morph_offsets(const ReplacementModel& model, std::span<const std::int16_t> shapes,
                                                                   std::span<const SkinMorphTarget> targets,
                                                                   std::vector<std::array<float, 3>>& scratch);

// Export: the skeleton (joints bone_NN under an "Armature" node, inverse bind matrices) and each part as a mesh
// skinned to it, at the bind pose (the referenced vertices of the part's identity-palette skinning; weights
// normalised; up to 8 influences as JOINTS_0/1 + WEIGHTS_0/1), its image as the material's base colour texture.
struct ExportPart {
    std::string name;
    const SkinnedMesh* mesh = nullptr;            // null: the skeleton alone
    const mods::RgbaImage* image = nullptr;       // null: untextured material
    // 132md3: a morph part's shapes (MORPH.HOG <stem>.SKF) and its table's names: one glTF morph target per shape
    // (POSITION offsets; a face's NORMAL offsets too), named by shape_target_name in mesh.extras.targetNames.
    const MorphData* morph = nullptr;
    const ShapeNames* shape_names = nullptr;
    // 132md3: the part's vertices are local to their chunk's bone (a gun part: each chunk one bone group, or a flare
    // chunk); they are moved by that bone's bind translation into model space. Fails on a chunk of several groups.
    bool bone_local = false;
};
struct ExportedModel {
    std::string gltf;                               // the .gltf text (buffer <stem>.bin, images <stem>[.<n>].png)
    std::vector<std::uint8_t> bin;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> images;   // file name, PNG bytes
};
ExportedModel export_model(const SkeletonDefinition& skeleton, std::span<const ExportPart> parts, const std::string& stem);
// Writes <folder>\<stem>.gltf, .bin and the PNGs.
void write_exported(const std::filesystem::path& folder, const std::string& stem, const ExportedModel& model);

}  // namespace omega::gfx
