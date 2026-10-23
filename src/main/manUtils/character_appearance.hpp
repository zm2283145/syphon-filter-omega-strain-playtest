#pragma once
// Character appearance resolution (CHARACTER_APPEARANCE.md): what the actor model
// init 001b3460 -> 00392b50 -> 0038db30 (model = actor +60) does with a packed SKL
// manifest before any part is loaded. Interpreter-verified against 0038db30 with
// 00404750 (part mesh), 00403a20 (part texture), 00390570 (classification) and
// 0038c920 (part records) hooked. The result names every archive member the
// original would request; nothing is opened here. The classification 00390570
// (FRONTEND/MANSORT2.TXT) runs separately (man_sort.hpp, on `state`): `parts` is the order before it.
#include "appearance.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
// One mission SKL.HOG member as the registry 0055d4f8 holds it (00407830, archive
// index order): `key` = 00407ec0(member name, 0x1f), the name up to its first '.',
// case kept; `packing` = 00406570 of the member bytes.
struct AppearanceRegistryEntry {
    std::string key;
    AppearancePacking packing;
};
// 00407ec0(name, 0x1f): the characters before the first '.' (or NUL). A stem of
// 31 or more characters leaves the original key unterminated and throws.
std::string appearance_registry_key(std::string_view member_name);

// A part mesh as 00404750 requests it (cache 0055dda0, archive MANSKM.HOG
// 0057124c). The cache is searched first: a preregistered key (see below) serves
// the request, blank or not. Otherwise `blank` (bytes 6..8 equal "BLA", cache +0
// under mask +60) loads nothing and the part has no mesh, and a non-blank name
// opens the two LOD members: the name with byte 14 replaced by '0' / '1'
// ("%s.SKM"). An absent '1' member copies the '0' slot; an absent '0' leaves no
// mesh. The draw (003870c0) picks LOD 1 by distance (+2be8), LOD 0 when +2c08.
struct AppearanceMeshRequest {
    std::string name;
    bool blank{};
    std::array<std::string, 2> members{};
};
// A part texture as 00403a20 requests it (archive MANTEX.HOG 005711b0), after
// the same cache search. `blank`: bytes 6..8 equal "BLA" and the name does not
// start with "_MAKE": the part takes the handles of texture-cache entry 0 (the
// first texture registered in the session), nothing is loaded. Otherwise
// "%s.TDX" with byte 14 of the formatted name set to '0' / '1'. An absent '0'
// member uses 00381000 (COMMON/ERROR.TDX); an absent '1' reuses '0'.
struct AppearanceTextureRequest {
    std::string name;
    bool blank{};
    std::array<std::string, 2> members{};
};
// The naming rules above applied to one 16-byte slot (at most 15 characters).
AppearanceMeshRequest appearance_mesh_request(std::string_view name);
AppearanceTextureRequest appearance_texture_request(std::string_view name);
struct AppearancePart {
    AppearanceMeshRequest mesh;          // pair slot 0
    AppearanceTextureRequest texture;    // pair slot 1 (+22c0/+2300 per pair)
};
struct CharacterAppearance {
    std::string name;                    // model +20: 00407ec0(name, 0x1f), uppercased
    std::string bone_scale_preset;       // slot 0 (00392b50 -> 003913d0)
    std::string shape;                   // slot 1 (0038c920 shape key, +2bc2)
    std::string skeleton;                // slot 3 = model +70 = actor +d0 (003aa160)
    std::optional<std::uint8_t> gender_3260; // 001b3460: 2 for MALE.SKEL, 1 for FEMALE.SKEL, else unchanged
    AppearanceMeshRequest hard_mesh;     // slot 2 as 00405940 requests it (not a drawn part)
    std::vector<AppearancePart> parts;   // pairs before the terminator, order before 00390570
    std::optional<AppearanceTextureRequest> extra_texture; // terminator pair's texture: +2bd0 = 1
    std::optional<std::size_t> variant;  // registry index merged by 00406950
    char tone_digit{'_'};                // +2c0d = 00406750 before the merge
    AppearancePacking state;             // final model +40..+480
};
// 0038db30(model, name, registry packing of `name`, serial). `name` is the
// population record's model string (actor +3318) and `serial` actor +31f0: -1 for
// roles 0, 1 and 3, the 004ee968 counter value for roles 2 and 4. The packing
// comes from the registry entry whose key equals the uppercased stem (00407da0);
// an absent entry (the original then loads COMMON/SKL.HOG) throws.
//   serial < 0: 00406e00 removes every pair with "BLA" in either name.
//   serial >= 0 and name equal to "PC_MALE.SKL" (any case): pairs 0/1 become the
//     terminator pair and the one after it (no parts remain).
//   otherwise: 00406950 merges registry entry s + serial % n, where [s, s+n) is the
//     first run of keys that start with the model name (n = 1: nothing merged).
// Reading a manifest byte the packer did not write, or growing the pair table
// past 0x440 bytes, throws.
CharacterAppearance resolve_character_appearance(std::string_view name, std::int32_t serial,
                                                 std::span<const AppearanceRegistryEntry> registry);
// The front-end player (Pop_LoadNPC 001c1e70, record 0, when D_004ffbb2 is set and app +250 is 0):
// Npc_Construct 001b6ab0 receives 0055d510 + 0x440 * D_00493a60 (the loaded agent's appearance,
// copied there by GuiAgentLoad 00292c54), copies it to model +40 (001b7150) and clears the record's
// model name; Npc_Initialize 001b6608 then passes a null packing and Skel_Bind 00392b50 calls
// 0038db30(model, null, null, -1). Model +20 becomes "CUSTOM" (D_004bc398), +40 is kept, and the
// serial < 0 path (00406e00) and the part requests follow as above.
CharacterAppearance resolve_character_appearance_custom(const AppearancePacking& packing);
// 00406e00 alone: delete every pair with "BLA" (bytes 6..8) in either name, shifting the rest down
// through the first pair whose texture slot is zero (GuiAgentModify's save runs it on the 0055d510 copy).
void appearance_remove_blank_pairs(AppearancePacking& s);
// The body switch of game::SwitchBodies (00175630 -> event 0x15 -> 00390fc0(model, costume
// model +40)): 00390940 then 00406e00 and 00406dc0 (+23c0) on `model` (model +40). Interpreter
// goldens tools/ee/gen_switch_bodies.py. 00390940:
//   1. frees the part handles +2340[i < +23c0] (0036ca70; the caller drops its parts);
//   2. `backup` (+2c14): when set, copies it back over `model` first; otherwise saves a copy
//      (Mem_Alloc 0x440, "man.cc" line 0x313);
//   3. when 004067e0 differs between model and costume, rewrites byte 0 of the costume's
//      pair names ('2' -> '1' when the model's kind is 1, else '1' -> '2') and of its slot 2
//      IN PLACE (the costume's own packing changes);
//   4. HAIR_: a costume hair pair replaces the model's hair first name; else a costume HAT-_
//      pair renames the model's hair mesh bytes 10.. to "HAT-" (non-blank hat, model without
//      a hat) or "HAIR" (blank hat over a model hat), loading it through the mesh cache;
//   5. slot 2 (hard mesh) becomes the costume's (cache lookup / load when it differs);
//   6. every costume pair overwrites the model pair with the same bytes 1..5 (mask +a0), or is
//      inserted before the terminator; a nonzero costume terminator texture becomes the model's.
// The part-name rules 00401290 applies (cache +e4 / +e8) are loaded only by the agent creator
// 004037f0 (frontCharacter 0022a220), not hosted: their count is 0 and 00401290 writes nothing.
// `cache_lookups` lists the names searched with 004046f0 (step 4 / 5) in call order; a miss
// loads it (MeshCache_Load, MANSKM.HOG) and sets +2bdb. 00390570 (classification) is not
// run here (man_sort.hpp), as for 0038db30.
struct BodySwitch {
    std::vector<std::string> cache_lookups;
    bool backup_saved{};                 // step 2 allocated +2c14
};
BodySwitch switch_body_packing(AppearancePacking& model, AppearancePacking& costume,
                               std::optional<AppearancePacking>& backup);
// game::RestoreBody (001754f0 -> 00391210): with +2c14 set, copies it back over `model` and
// frees it (Mem_Free "man.cc" 0x2a8); +23c0 is rewritten by ActorModel_Setup. Returns false
// (nothing done) when no backup is held.
bool restore_body_packing(AppearancePacking& model, std::optional<AppearancePacking>& backup);
// The parts ActorModel_Setup 0038c920 / 0038e030 take from a packing after a switch or
// restore: pairs before the terminator (meshes from the mesh cache, 004046f0; a miss leaves the
// part empty), textures of every pair with a nonzero texture slot looked up in the loaded
// texture list (00381f30, '1' at byte 14 for LOD 1; a miss takes 00381000), slot 2 as it
// stands (no 00405940 HARD substitution). `name`, `variant` and `tone_digit` are not touched.
CharacterAppearance appearance_of_packing(std::string name, const AppearancePacking& state);
// 001b3460 (actor vt+0x44, model init; run by the record ctor tail 001b5fe0 and the generator spawn ctor for
// every actor): strcmp(actor +d0, "MALE.SKEL" 0x49e830) stores +3260 = 2 (1b34a8), "FEMALE.SKEL" (0x49e840) 1
// (1b34cc); any other skeleton leaves +3260 as the constructor zeroed it (nullopt).
std::optional<std::uint8_t> actor_gender_3260(std::string_view skeleton);
// 0038c920 on the final packing (model +40): +2c0c = 004067e0(model +40), the shape-table index (005712f0 /
// 005712f8: 1 MALE.BIN, 0 FEMALE.BIN, 2 none), and the base shape key, slot 1 (model +50, all 16 bytes).
std::uint8_t appearance_shape_table(const AppearancePacking& packing);
std::array<std::uint8_t, 16> appearance_shape_key(const AppearancePacking& packing);
// Archive members registered into the part caches before any character is built
// (static; 001c4190 runs 00136a70 -> 001457c0 before 001c3940 -> 001c1e70):
// 00404060 for COMMON/MORPH.HOG, 00404aa0 for GAMEDATA/<mission>/NPCSKM.HOG (its
// ".BON" members become bone-scale presets), 00403d20 for <mission>/NPCTEX.HOG.
// A request whose name matches a registered key under mask +80 (bytes 0..13 and
// 15) is served from that member and its archive is not opened.
// Mesh: a member whose text after its last '.' is exactly ".SKM", keyed by
// 00407ec0(name, 0xf), fills LOD slot (key byte 14 - '0'), 0 unless that is 0 or 1.
// Key byte 15 is a stack byte 00407ec0 does not write; it is taken as 0 (inferred:
// otherwise no preregistered mesh could match a manifest name).
std::optional<std::size_t> appearance_preregistered_slot(const AppearanceMeshRequest& request,
                                                         std::string_view member_name);
// Texture (00403d20): every member, keyed by 00407ec0(name, 0xf) with byte 14 set
// to '0'; it fills LOD slot 0 when byte 14 of the member name is '0', else slot 1
// (an empty slot then takes the other one).
std::optional<std::size_t> appearance_preregistered_texture_slot(const AppearanceTextureRequest& request,
                                                                 std::string_view member_name);

// The draw LOD of one actor model (003870c0 0x387860..0x387938; interpreter goldens
// tools/ee/gen_actor_draw_lod.py). `view_root` is the model root's translation in
// view space (matrix-stack top x skeleton root node world matrix, sp+10d0). Unless
// forced (003870c0 a3 or t0 nonzero), the distance is the VU0 length
// ((x*x + y*y) + z*z, vsqrt; stored to model +2be4) and the index is
// cvt.w.s((distance * half_fov) / 12.0f), 1 when that is 2 or more, and 0 whenever
// the gore counter +2c08 is nonzero (stored to +2be8). `half_fov` is the current
// camera record's +100 (0x5381f0 + 0x1f0 + 0x110 * [0x538c60]). Forced: index 1,
// nothing stored. The index picks the part records +1ec0 + (lod << 9) drawn by
// 00385d40 and the texture list 0038a800 binds (+22c0 for 0, +2300 otherwise).
struct ActorDrawLod {
    std::optional<float> distance;       // +2be4 (absent when forced)
    std::int32_t lod{};                  // s2 (= +2be8 when not forced)
};
ActorDrawLod actor_draw_lod(const std::array<float, 3>& view_root, float half_fov, std::int32_t gore_counter, bool forced = false);
}
