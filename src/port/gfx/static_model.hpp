#pragma once
// Port-only (checkpoint 132md2b, research/MODDING.md "Stage 3b"): level static model replacement through glTF 2.0. Not
// game code. A level model (a VUM: terrain pieces and the sky in the mission's DATA.HOG, objects and NI models in
// OBJECTS.HOG) exports as a glTF of its drawn mesh; a mod's glTF loads as a Mesh that replaces the drawn mesh only. The
// model's COL collision, declared bounds (visibility, shadow receivers, NI boxes), header flags and texture list stay
// the original's, so gameplay is unchanged.
//
// Space: as the character models' (character_model.hpp): game Z-up, glTF Y-up, (x, y, z) -> (-x, z, y). Terrain pieces
// are in world space, objects in their model space (the population's placement applies on load, as for the original).
// Vertex colours (the game's baked light, 0x80 = 1.0) export as COLOR_0 RGB at half scale (0.5 = 0x80). Materials
// export as material_NN (NN the VUM material index); on import a primitive whose material is named material_NN
// draws with that original material's shader, flags and passes, any other with the first drawn material's; its base
// colour image, when it has one, replaces that material's texture for this model only.
//
// Checkpoint 132md3: models with colour patch sets and animated materials are replaceable too. The patch sets (the PNT
// lights' vertex colour toggles, 003767e0) are remapped to the replacement's vertices: each takes its nearest original
// vertex's per-set change, scaled by the ratio of its colour to that vertex's (an unchanged re-export keeps the
// original's changes exactly). The surfaces drawn with animated materials (00376ad0 makes one animator per surface, in
// order, some drawing the game's random numbers) are laid out as the original's: one surface per original animated
// surface, in its order, with that material, so the animators (count, order, parameters) are the original's.

#include "character_model.hpp"
#include "geometry.hpp"
#include "gltf.hpp"
#include "image_files.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::gfx {

inline constexpr std::size_t static_vertex_limit = 1u << 20;
inline constexpr std::size_t static_triangle_limit = 1u << 21;
// Vertices per generated batch (the terrain point-light pass lights a batch by its bounding sphere).
inline constexpr std::uint32_t static_batch_vertices = 64;

struct StaticReplacement {
    Mesh mesh;                                              // game space, ready for append_terrain
    std::vector<std::optional<mods::RgbaImage>> images;     // per mesh material: its glTF image (none: the original's)
    std::vector<std::uint32_t> inherited;                   // per mesh material: the original material it copies
    float overshoot = 0;   // how far the geometry reaches outside the original's declared bounds (0: inside)
    std::size_t patched_vertices = 0;   // replacement vertices a colour patch set changes (132md3)
    std::size_t animated_surfaces = 0;  // surfaces laid out as the original's animated ones (132md3)
    std::vector<std::string> notes;     // what was kept or ignored (logged with the replacement)
};

// Why a level model cannot be replaced (empty: it can): it has no material.
std::string static_model_blocker(const Mesh& original);
// What a replacement of the model keeps from the disc (132md3; empty: nothing): its colour patch sets (remapped to
// the replacement's vertices) and its animated materials (the animators kept).
std::string static_model_remaps(const Mesh& original);

// The original's colour patch sets remapped to `replacement`'s vertices (game space, as the original's): per set the
// same key, spans of the replacement vertices whose change is not zero. A replacement vertex takes its nearest
// original vertex's change per set (equal distances: the lower index), as the signed change d that the original's
// off state (byte - change, wrapping) gives; d scales by the ratio of the replacement's colour byte to the original's
// (d kept when the byte is 0), then clamps so the off state stays in 0..255. Colour bytes: lround(colour * 128),
// at most 255. `changed` (optional) receives the replacement vertices any set changes.
std::vector<ColorPatchSet> remap_color_patches(const Mesh& original, const Mesh& replacement, std::size_t* changed = nullptr);

// The replacement from a document: every mesh node of the default scene (node transforms applied), triangles (strips
// and fans converted), missing normals computed, missing UVs zero, missing colours 0x80 (colours clamp at byte 255).
// Throws std::runtime_error naming the first problem: a blocker above, a skin, a non-triangle primitive, bad
// accessors, the size limits. A document with no mesh is an empty (hidden) model. Colour patches are remapped and
// animated materials laid out as above; a primitive drawn with an animated material keeps the disc's texture (its
// glTF image is ignored, noted: retexture it under textures\).
StaticReplacement load_static_replacement(const gltf::Document& doc, const Mesh& original);
StaticReplacement load_static_replacement(const std::filesystem::path& file, const Mesh& original);

// Export of a VUM's drawn mesh: one node, one primitive per drawn material (its referenced vertices), materials
// material_NN with their texture's image (images[i]: the image of mesh.textures[i], null: untextured). Written with
// write_exported (character_model.hpp).
ExportedModel export_static_model(const Mesh& mesh, std::span<const mods::RgbaImage* const> images, const std::string& stem);

}  // namespace omega::gfx
