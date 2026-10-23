#pragma once
// Port-only (checkpoint 132md2, research/MODDING.md "Stage 3"): the mod layer's character model replacements in the
// play session. Not game code. A mod's models\<disc path>\<member>.gltf replaces that skinned part (SKM) in the
// renderer only: the replacement (gfx::ReplacementModel, validated against the character's skeleton) is skinned by the
// character's committed palette in place of the part's own skinning; the part's texture is the glTF's image (full
// resolution) when it has one. Everything else (the animation, hit volumes, gore arrays of unreplaced parts, collision,
// gameplay) stays the original's. With no mods installed nothing here runs (the callers see null overrides).

#include "animated_character.hpp"
#include "character_model.hpp"
#include "frontend_lights.hpp"
#include "gui_draw.hpp"
#include "gun_model_draw.hpp"
#include "skeleton.hpp"
#include "static_model.hpp"
#include "texture.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::mods {

struct CharacterModelOverrides {
    // Per part: the replacement drawn at LOD 0 / LOD 1 (null: the original part).
    std::vector<std::shared_ptr<const gfx::ReplacementModel>> lod0, lod1;
    // 132md3: per part, LOD 0's morph targets bound to the part's shape table (gfx::bind_morphs; empty: none drawn).
    std::vector<std::vector<std::int16_t>> lod0_shapes;
    // 132md3: per part, its LOD 0 key and whether its first weighted shapes were logged (substitute_models).
    std::vector<std::string> lod0_keys;
    std::shared_ptr<std::atomic<bool>[]> shapes_logged;
};

// A part's model key: member_key(asset_key(archive), member), e.g. "GAMEDATA/COMMON/MANSKM.HOG/<NAME>.SKM".
std::string part_key(const std::vector<std::string>& archive, std::string_view member);

// The installed mods' replacements of a character's parts. lod0_keys[i]: part i's member key (asset_key /
// member_key: "GAMEDATA/COMMON/MANSKM.HOG/<NAME>.SKM"); lod1_keys[i]: its LOD 1 mesh's key (the LOD 0 key when LOD 1
// draws LOD 0's mesh; empty: no LOD 1 drawn). LOD 1 takes its own key's replacement, else LOD 0's (a replaced part
// does not change model with distance). A model that fails to load or to match `skeleton` is logged (mods::note) and
// the original drawn. Null when no mods are installed, they have no models, or none of the keys is replaced.
// Loads are cached per installation, key and skeleton name.
// 132md3: shape_tables[i] (optional, per part): the shape names of part i's morph binding (MALE.BIN / FEMALE.BIN for a
// face, its hand table for a hand; null: none). A LOD 0 replacement's morph targets bind to them (gfx::bind_morphs); a
// target the table lacks rejects the replacement (logged, the original drawn); on a part without a table its targets
// are not drawn (logged). LOD 1 draws no morph (as the original's far LOD).
std::shared_ptr<const CharacterModelOverrides> character_model_overrides(const SkeletonDefinition& skeleton,
    const std::string& skeleton_name, std::span<const std::string> lod0_keys, std::span<const std::string> lod1_keys,
    std::span<const ShapeNames* const> shape_tables = {});

// The shape table of each of a character's parts by its morph binding: faces[table] for a face, hands[table][h] for
// hand h (null: no binding, or a table past 1).
std::vector<const ShapeNames*> part_shape_tables(std::span<const AnimatedCharacterPart> parts, std::uint8_t table,
    const std::array<ShapeNames, 2>& faces, const std::array<std::array<ShapeNames, 2>, 2>& hands);

// A replaced part's glTF image becomes its texture's GPU image (Texture::replacement; images[i] for LOD 0, marked
// textured, lod1_images[i] when LOD 1 has its own image). Parts without a glTF image keep their textures.
void attach_model_images(const CharacterModelOverrides& models, std::vector<Texture>& images, std::vector<bool>& textured,
                         std::vector<std::optional<Texture>>& lod1_images);

// Replaces the geometry of each replaced part (models[i] non-null) by its replacement skinned by `palette`.
void substitute_models(const std::vector<std::shared_ptr<const gfx::ReplacementModel>>& models,
                       std::span<const SkinPaletteBone> palette, std::vector<AnimatedCharacterGeometry>& geometry);
// 132md3: the same for a drawn character: LOD 0 replacements (distant false) with bound morph targets are deformed by
// the character's current shape slots (morph_face_targets / morph_hand_targets of c.morph(), by each part's binding)
// before skinning by its committed palette. The first time a part's bound targets carry weight it is logged once
// ("shapes drawn", with those shapes and weights).
void substitute_models(const CharacterModelOverrides& models, bool distant, const AnimatedCharacter& character,
                       std::vector<AnimatedCharacterGeometry>& geometry);

// Stage 3b: a level model's replacement (terrain piece, object, NI model or sky: a VUM member, key e.g.
// "GAMEDATA/TORONTO1/DATA.HOG/<X>.HOG/<X>.VUM"), loaded against `original` (the disc's decoded mesh) and cached per
// key. Logged (mods::note) when loaded, with how far it extends outside the original's declared bounds, or when
// rejected (gfx::load_static_replacement's error, e.g. a skinned glTF), the original then drawn. Null when no
// mods are installed, they have no models, the key has none, or it was rejected. The model's collision is never
// replaced.
std::shared_ptr<const gfx::StaticReplacement> static_model_override(const std::string& key, const Mesh& original);

// The texture a replacement material's glTF image draws as: `base` (the original material's bound texture, kept for
// its sizes and CPU readers; animation frames dropped) with the image as its GPU replacement; `base` with no pixels
// (an untextured material): the image's own texture.
Texture model_image_texture(const RgbaImage& image, const Texture& base);

// ---- 132md3: gun models (research/MODDING.md "Gun models") ----
// A gun model (GunModel, 0038d530 / 0038cd90 on a gun HOG) draws its parts with eleven matrices (gun_slot_matrices):
// matrix 0 the attach point, matrix k (1..10) its parent's moved by the bone offset +150+32k. As a skeleton: 11 bones
// bone_00..bone_10, bone 0 the root at the origin, bone k under its parent (word +15c+32k) at that offset. A part's
// vertices are local to their bone's matrix; exported (and replaced) at the bind pose: the bones' offsets summed.

// The model's name (bytes +0.. up to NUL or '.', upper case): its gun HOG is the archive member <name>.HOG.
std::string gun_model_name(const GunModel& model);
// The gun's skeleton (throws on an undefined byte or a parent not below its bone, as gun_slot_matrices does).
SkeletonDefinition gun_skeleton(const GunModel& model);
// The gun HOG member (an .SKM) holding part p's LOD `lod` record (its descriptors, +2c0 / +340 + 16p word +8),
// upper case; empty when the part has no record (word +0 or the chunk count 0) or no member holds it.
std::string gun_part_member(const GunModel& model, std::span<const std::uint8_t> hog, std::size_t part, std::int32_t lod);
// A gun part's model key: "<archive key>/<NAME>.HOG/<member>" (archive "GAMEDATA/COMMON/WPNHOG.HOG" or
// "GAMEDATA/<MISSION>/WPN.HOG").
std::string gun_part_key(std::string_view archive_key, std::string_view name, std::string_view member);

struct GunModelOverrides {
    std::string name;
    // [lod][part] (null: the original drawn); a LOD 1 part without its own replacement takes LOD 0's.
    std::array<std::array<std::shared_ptr<const gfx::ReplacementModel>, 8>, 2> parts;
    std::array<std::array<std::string, 8>, 2> keys;
    std::array<std::array<float, 3>, 11> bind{};   // the bones' bind translations (model space)
    const gfx::ReplacementModel* part(std::size_t p, std::int32_t lod) const {
        return p < 8 && (lod == 0 || lod == 1) ? parts[static_cast<std::size_t>(lod)][p].get() : nullptr;
    }
};
// The installed mods' replacements of a gun model's parts (+40c, at most 8; `hog` the model's gun HOG): for each
// part and LOD the mission's key (GAMEDATA/<mission>/WPN.HOG/..., when `mission` is not empty) then COMMON's
// (WPNHOG.HOG); the first key a mod supplies is loaded against gun_skeleton(model) (a mismatch is logged and the
// part drawn as the disc's). Null when no mods are installed, they have no models, or no part is replaced. Cached
// per installation, key and skeleton.
std::shared_ptr<const GunModelOverrides> gun_model_overrides(const GunModel& model, std::span<const std::uint8_t> hog,
                                                             std::string_view mission);
// A replacement skinned by the draw's eleven matrices: bone k's palette entry is matrices[k] after the bone's inverse
// bind (its bind translation removed), so a vertex at the bind pose lands where the original's vertex local to
// matrix k does.
void skin_gun_replacement_into(const GunModelOverrides& gun, const gfx::ReplacementModel& model,
                               const GunSlotMatrices& matrices, SkinnedMeshGeometry& output);
// Part p's texture id at `lod` (+3c0 / +3d0 + 2p; -1 when undefined or p past 8): the original image a
// replacement's image is attached over (model_image_texture's base).
std::int32_t gun_part_texture_id(const GunModel& model, std::size_t part, std::int32_t lod);

// ---- 132md11: automatic levels of detail (research/MODDING.md "Speed") ----
// Whether character / gun part loads build gfx::build_replacement_lods' coarser levels for a dense part (on by
// default; play's --mods-lod off and --mods-cpu-skinning turn it off: only the GPU skinning draw uses them). A part
// with levels is logged with their triangle counts, largest error and build time. Applies to parts loaded afterwards.
void set_model_lods(bool enabled);
bool model_lods();

// ---- 132md7: speed of high-resolution replacements (research/MODDING.md "Speed") ----
// What substitute_models (the drawn character's form) skins, per part (draws[i].model null: not replaced): the
// replacement, the character's committed palette and its morph offsets (in scratch[i]), logging the first weighted
// shapes as it does. A caller skins the vertices itself (gfx::skin_replacement_vertex, e.g. split across a worker
// pool straight into a mesh: render/draw_prepare.hpp PartSource); the values are substitute_models'.
void replacement_draws(const CharacterModelOverrides& models, bool distant, const AnimatedCharacter& character,
                       std::vector<gfx::ReplacementDraw>& draws, std::vector<std::vector<std::array<float, 3>>>& scratch);
// The same for skin_gun_replacement_into.
gfx::ReplacementDraw gun_replacement_draw(const GunModelOverrides& gun, const gfx::ReplacementModel& model,
                                          const GunSlotMatrices& matrices);
// The 003836b0 preview vertices of a skinned replacement part (the GuiVertex list the weapon previews build per
// triangle corner: the position with w 1; colour frontend_light_colour(lights, normal), or (c, c, c) for a flare part;
// alpha `alpha`; uv + 2048), each vertex lit once (the values lighting each corner gives).
std::vector<GuiVertex> gun_preview_vertices(const SkinnedMeshGeometry& geometry, const FrontendLightSet& lights, bool flare,
                                            std::int32_t c, std::int32_t alpha);
// The weapon previews' replaced parts kept across frames (GuiDrawBatch::shared_vertices): an entry is reused while its
// part is drawn with the same eleven matrices, flare program and alpha, so a still preview is skinned and lit once.
class GunPreviewCache {
public:
    struct Entry {
        GunSlotMatrices matrices{};
        bool flare = false;
        std::int32_t alpha = 0;
        std::uint32_t slot = 0;   // the host texture slot the part draws with
        std::shared_ptr<const std::vector<GuiVertex>> vertices;
    };
    // `model`'s entry when it was built from these inputs (bitwise), else null.
    const Entry* find(const gfx::ReplacementModel* model, const GunSlotMatrices& matrices, bool flare, std::int32_t alpha) const;
    const Entry& store(const gfx::ReplacementModel* model, Entry entry);
    void clear() { entries_.clear(); }

private:
    std::map<const gfx::ReplacementModel*, Entry> entries_;
};

}  // namespace omega::mods
