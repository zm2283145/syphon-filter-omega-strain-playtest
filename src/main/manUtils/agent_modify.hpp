#pragma once
// The agent appearance editor (GuiAgentModify, AGENTMOD.GUI; research/FRONTEND.md "Agent appearance"):
// the part cache 0055dda0 the front end fills (frontCharacter 0022a220) and the search, step and
// count functions the editor runs over it. Interpreter goldens: tools/ee/gen_agent_modify.py.
//
// A key is a 16-byte part name (00407ec0(name, 15)): byte 0 the sex ('1' male, '2' female, '_' any),
// 1..4 the category ("LEGS"), 5 a sub-slot, 6..8 the set ("SYN", "BLA" blank, "ALL"), 9 the skin tone
// ('_' any, or a digit), 10..13 the item ("NONE"), 14 the LOD digit. The cache masks +20..+b0 select
// byte ranges of a key (see `agent_mask`). A packing is the 0x440-byte appearance block (model +a0,
// agent *(agent + 9a4)): pair j's mesh name at +40 + 0x20 j, its texture name at +50 + 0x20 j.
#include "man_sort.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

using AgentPartKey = std::array<std::uint8_t, 16>;
using AgentPacking = std::array<std::uint8_t, 0x440>;

// The cache masks (0055dda0 +20..+b0; set before the front end, read from the boot image).
enum class AgentMask { m20, m30, m40, m50, m60, m70, m80, m90, ma0, mb0 };
const AgentPartKey& agent_mask(AgentMask m);

AgentPartKey agent_part_key(std::string_view name);   // 00407ec0(dst, 15, name)
std::string agent_part_name(const AgentPartKey& key); // up to the first NUL

// 0055dda0. `meshes` = +2c10 (count +c0), `textures` = +bc10 (count +cc), `rules` = the SWAPIT.TBL
// records at *(+e8) (count +e4). `locked_*` = the copies 00403040 takes (+d4 / +d8, counts +dc / +e0).
// `sort` = the MANSORT2.TXT records at +11c10 (count +ec).
struct AgentPartCache {
    std::vector<AgentPartKey> meshes;
    std::vector<AgentPartKey> textures;
    // +c8: the mesh count after the last preregistration (00404060 / 00404aa0 set it to +c0; the run-time
    // loads of 00404750 grow +c0 only). +d0: the texture count after 00403d20 (00403a20's loads grow +cc only).
    std::size_t mesh_static_count{}, texture_static_count{};
    std::vector<std::array<std::uint8_t, 0x80>> rules;
    bool rules_enabled = true;   // D_00493ab0 (1 in the boot image)
    std::optional<std::vector<AgentPartKey>> locked_meshes, locked_textures;
    // D_00571300: the keys of 00404aa0's ".BON" members (bone-scale presets), at D_00571304 + 0x10 i.
    std::vector<AgentPartKey> bone_presets;
    // D_005712f8[i] / D_005712f0[i]: the data of 00404060's ".BIN" morph-target name lists (i = 0 for
    // "FEMALE", 1 for any other name not starting '1' / '2') and their 16-byte entry counts.
    std::array<std::vector<std::uint8_t>, 2> morph_lists;
    std::array<std::size_t, 2> morph_list_counts{};
    std::vector<ManSortRecord> sort;
};

// 00403490(C, "GAMEDATA\FRONTEND\MANSORT2.TXT") (game init 00169574): nothing when +ec is already
// nonzero, else the records of parse_man_sort (man_sort.hpp, Checkpoint 132cp6) at +11c10.
void agent_cache_load_sort(AgentPartCache& cache, std::span<const std::uint8_t> text);

// 00390570(model + 60): MANSORT2's part order for a model, through sort_man_parts (man_sort.hpp, Checkpoint
// 132cp6) over the packing's 32 pairs (mesh +40 + 20 i, texture +50 + 20 i), which it reorders. The count
// loop (003905bc lq / beqz) tests a mesh key's low 8 bytes; sort_man_parts tests all 16, so a key zero in
// its low 8 bytes but not its high 8 throws, as do 32 nonzero pairs (the loop reads past the packing).
// `record[pos]` is model +2e10 (sort_man_parts' +2db0 of the argument), `value[pos]` +2e30 (the class).
struct AgentPartOrder {
    std::size_t count{};                     // model +2420
    std::array<std::int8_t, 32> record{};    // model +2e10
    std::array<std::int8_t, 32> value{};     // model +2e30
};
AgentPartOrder agent_reorder_parts(const AgentPartCache& cache, AgentPacking& packing);

// 004037f0(C, "GAMEDATA\FRONTEND\SWAPIT.TBL"): the file is the rule table; +e4 counts the 0x80-byte
// records from its start while the record's first 16 bytes are nonzero and the record starts before
// the end. A counted record that runs past the file (the original reads past its buffer) throws.
void agent_cache_load_rules(AgentPartCache& cache, std::span<const std::uint8_t> swapit);
// 00403d20(C, FRONTEND\MANTEX.HOG): every member, key 00407ec0(name, 15) with byte 14 = '0', added
// unless an equal key is held (exact 16-byte compare).
void agent_cache_register_textures(AgentPartCache& cache, std::span<const std::string> members);
// 00404060 (Skf_Register, FRONTEND\MORPH.HOG): first frees the 00403040 copies (+d4 = +d8 = 0); every
// member with a nonzero size whose text after its last '.' is ".SKM" or ".SKF" goes through 00403fd0.
// 00404aa0 (FRONTEND\MANSKM.HOG): every ".SKM" member through 00403fd0 (".BON" members are bone-scale
// presets, not keys). 00403fd0: added unless a key equal under mask +80 (all but byte 14) is held.
struct AgentArchiveMember { std::string name; std::size_t size{}; };
void agent_cache_register_morphs(AgentPartCache& cache, std::span<const AgentArchiveMember> members);
void agent_cache_register_meshes(AgentPartCache& cache, std::span<const std::string> members);
// 00404aa0's ".BON" branch: every ".BON" member with a nonzero size appends 00407ec0(name, 15).
void agent_cache_register_bone_presets(AgentPartCache& cache, std::span<const AgentArchiveMember> members);
// 00404060's ".BIN" branch for a member with a nonzero size: a name starting '1' / '2' is a hand
// morph list (D_005714f0, animation state: ignored here); any other is list 0 when 00407ec0(name, 15)
// is "FEMALE", else list 1; its count is the entries up to the first whose low 8 bytes are zero.
void agent_cache_register_morph_list(AgentPartCache& cache, std::string_view name, std::span<const std::uint8_t> data);

// 004073c0(packing, &n): the compact appearance 00334000 stores at agent +9a8 (the first 0x100
// bytes). Shorts: the pair count; the index of packing +0 among `bone_presets` (strcmp); of +10 in
// morph list [sex 004067e0] (strcmp); of +20 among the meshes under mask +80; +30 "MALE.SKEL" 1 /
// "FEMALE.SKEL" 0; then for pairs 0..count (the terminator included) the mesh index (the first key
// equal under mask +80, kept only below +c8 `mesh_static_count`) and the texture index (the first
// exact key, kept only below +d0 `texture_static_count`; a run-time load is never sent by index).
// Every -1 short then appends its 16 source bytes (packing + 0x10 k). A sex other than 0 / 1
// indexes past the lists: throws.
std::vector<std::uint8_t> agent_packing_compact(const AgentPartCache& cache, const AgentPacking& p);

// 00406ec0(dst, compact): the inverse (002c6b70's remote human, GuiAgentInfo 003608ac). The count
// short n (n <= 0 or n >= 0x20: 1) places the 16-byte blocks after 2 * n + 6 shorts. Slot 0: -1 a
// block, else 00407ec0(D_00571304 + 0x10 i) with i (unsigned) >= the preset count taken as 0; slot 1:
// -1 a block, else (after the pairs) entry i of morph list [004067e0(dst)] (i >= its count: 0);
// slot 2: -1 a block, else mesh key i (+2c10, unchecked); slot 3: the zero quad, then -1 a block,
// 1 "MALE.SKEL", any other "FEMALE.SKEL"; pairs 0..n: mesh key / texture key i (+bc10) or a block;
// then pair n + 1 zeroed. The other bytes of `dst` are kept. Throws where the original reads outside
// its tables (an index past the keys, an empty preset list, sex 2, a block or short past `compact`)
// or writes past the packing (n = 31: the zeroed pair at +440).
void agent_packing_expand(const AgentPartCache& cache, std::span<const std::uint8_t> compact, AgentPacking& dst);

// 00403040(C, list): locks the parts `names` lists (00332500's rows: AgentData_FilterAwards): on the
// first call copies the keys (+d4 / +d8); a name with a '.' is skipped; every mesh key equal to
// 00407ec0(name, 15) becomes the zero quad D_005716e0, and only when none was, every equal texture key.
void agent_cache_lock(AgentPartCache& cache, std::span<const std::string> names);
// 00403310(C): with +d4 set, copies the saved keys back (their saved counts, at least one entry each)
// and frees the copies.
void agent_cache_unlock(AgentPartCache& cache);

// The editors' slider position cvt.w.s(100.0f / (count - 1) * position) (00341c50 / 0028d610), with
// the EE's x / 0 = FLT_MAX and saturating conversion.
std::int32_t agent_slider_value(std::int32_t count, std::int32_t position);

std::size_t agent_pair_count(const AgentPacking& p);              // 00406dc0 (model +2420)
std::int32_t agent_packing_sex(const AgentPacking& p);            // 004067e0: 1 male, 0 female, 2 other
std::int8_t agent_packing_tone(const AgentPacking& p);            // 00406750
std::int32_t agent_pair_of(const AgentPacking& p, std::string_view five); // 004068b0

// The part functions (packing = model +a0, pair count = model +2420; the part records +1f20 and
// texture handles the original copies along are draw state, not modelled).
std::int32_t agent_texture_step(const AgentPartCache& c, std::int32_t dir, AgentPartKey mesh, AgentPartKey tex,
    std::int32_t start, std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p);        // 004002b0
std::int32_t agent_texture_step2(const AgentPartCache& c, std::int32_t dir, AgentPartKey mesh, AgentPartKey tex,
    std::int32_t start, std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p, std::size_t count); // 00400620
std::int32_t agent_colour_step(const AgentPartCache& c, std::int32_t dir, AgentPartKey mesh, AgentPartKey tex,
    std::int32_t start, std::int32_t pair, std::int32_t sex, AgentPacking& p, std::size_t count);      // 00400990
std::int32_t agent_mesh_step(const AgentPartCache& c, std::int32_t dir, AgentPartKey name, std::int32_t start,
    std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p, std::size_t count);         // 00401970
std::int32_t agent_mesh_step4(const AgentPartCache& c, std::int32_t dir, AgentPartKey name, std::int32_t start,
    std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p);                             // 00401570
void agent_texture_find(const AgentPartCache& c, AgentPartKey key, std::int32_t pair, std::int32_t sex,
    std::int8_t tone, AgentPacking& p);                                                                   // 004022e0
void agent_mesh_rules(const AgentPartCache& c, AgentPacking& p, std::size_t count, AgentPartKey old_name,
    AgentPartKey new_name, std::int32_t sex, std::int8_t tone);                                           // 00401290
void agent_texture_rules(const AgentPartCache& c, AgentPacking& p, std::size_t count, AgentPartKey old_name,
    AgentPartKey new_name);                                                                               // 00400f20
std::int32_t agent_count_meshes(const AgentPartCache& c, AgentPartKey name, std::int32_t& pos, std::int32_t sex,
    bool item);                                                                                           // 00402200
std::int32_t agent_count_textures(const AgentPartCache& c, AgentPartKey key, std::int32_t& pos, std::int32_t sex,
    std::int8_t tone, std::uint32_t flags);                                                               // 00401ff0
std::int32_t agent_count_pairs(const AgentPartCache& c, AgentPartKey mesh, AgentPartKey tex, std::int32_t& pos,
    std::int32_t sex, std::int8_t tone, std::uint32_t flags);                                             // 00401e40

// The category table D_00492a00 (16 entries {kind, pair, position}) and the names D_00492a40.
inline constexpr std::array<const char*, 16> agent_modify_categories{
    "hairstyle", "haircolor", "hat", "eyewear", "beard", "makeup", "shirt", "vest", "pants", "boots",
    "leftarm", "rightarm", "back", "leftarm2", "rightarm2", "hat2"};
struct AgentModifyCategory { std::uint8_t kind{}; std::int8_t pair{-1}; std::uint16_t position{}; };

// GuiAgentModify's editing state (this +e4 count, +e8 position, +f8 sex, +fc tone, +100 / +104 the
// mesh / texture cache indices, +10d pair, +10e kind, +10f category, +110 previous category) over the
// Checkpoint 132cc6: the agent screens' camera preset (agent_camera.hpp): GuiAgentCreate +80..+ac,
// GuiAgentModify +a0..+cc, a MANSORT2 record's fields.
struct AgentCamera {
    AgentPartKey name{};          // +80
    std::int32_t value{};         // +90
    float half_fov{};             // +94
    std::array<float, 3> loc{};   // +98 / +9c / +a0
    std::array<float, 3> rot{};   // +a4 / +a8 / +ac (+ac: the L2 / R2 turn of 0028ea40 / 00342cd0)
    bool operator==(const AgentCamera&) const = default;
};

// model's packing. `table` is the static D_00492a00: the original keeps it across editor instances.
struct AgentModify {
    AgentPacking packing{};
    std::size_t pair_count{};
    std::int32_t sex{}, count{}, position{};
    std::int8_t tone{};
    std::int32_t mesh_index{}, texture_index{};
    std::int8_t pair{}, previous_category{};
    std::uint8_t kind{};
    std::int8_t category = 16;   // ctor 00343e10: 0x10, no category
    std::int32_t refresh_e0{};   // +e0: the camera transition (003428d0)
    AgentCamera camera{};        // +a0..+cc (Checkpoint 132cc6): record 0 (00344934)
    std::array<AgentModifyCategory, 16> table{};
    std::string slot_text;       // this +7c: "%d/%d" (D_004b9f70), "0/0" (D_004b9f78) or "" (D_004b9f20)
    bool slider_shown{};         // this +78 flags bit 2
    std::int32_t slider_value{}; // 0041e0f0(this, +78, 0xa, value)
    std::uint32_t changes{};     // 004147a0 / 0041e0f0(this, sound, 0x10, 6) calls
    bool dirty{};                // this +158: set by 00341378 (a step with a category and a count); wake clears it
    AgentPartOrder order{};      // the model's +2e10 / +2e30 (00390570)
};
// The ctor 00343e10 from the packing copy (0055d510 + 0x440 * max(D_00493a60, 0)): 00406dc0, the
// blank pairs 004061b0, the MANSORT2 order 00390570 (003441b0), sex 004067e0 (+f8), tone 00406750
// (+fc, model +2c6d), the category pairs 003446d0 (first pair whose mesh or texture bytes 1..5 equal
// D_00492900[sex][category] bytes 1..5, the terminator pair included; a category without one keeps
// the table's previous value), then 00343940 with +10f = 16.
AgentModify agent_modify_open(const AgentPartCache& c, const AgentPacking& packing,
                              std::span<const AgentModifyCategory, 16> table);
void agent_modify_refresh(const AgentPartCache& c, AgentModify& m);                 // 00343940
void agent_modify_select(const AgentPartCache& c, AgentModify& m, std::string_view name); // 00341c50
void agent_modify_step(const AgentPartCache& c, AgentModify& m, bool back, bool forward); // 00341330
// 003437c0(this, pair) / 00343680(this, pair) (GuiAgentModify init, for PATC1 / PATC2 and PATC3): when
// the pair's texture key is not held (004027e0, e.g. locked), steps its item number up to 32 times
// (003437c0: bytes 12..13, "n-" counting to "9-" then "10"; 00343680: bytes 11..12) and on the first
// held key runs 004022e0 (model +2c6c sex, +2c6d tone) and stores the key. Pair -1 reads +30.
void agent_modify_patch_advance(const AgentPartCache& c, AgentModify& m, std::int32_t pair, bool two_digits);
// 00340850's test of one tattoo / patch pair (this +114..+128): the pair's mesh name (pair -1 reads
// packing +20) equals the cache's +0 key "1belt_BLA_BLAN_" under mask +60 (bytes 6..8, the set "BLA").
bool agent_modify_part_blank(const AgentPacking& p, std::int32_t pair);
// GuiAgentModify's save (Square, 0033fb60..0033fb98): pair 004068b0(p, "VEST_"); when held, the hard
// mesh name at +20 takes the vest mesh's bytes 10..13 (mask +70). The caller then copies the block to
// the 0055d510 slot and runs 00406e00 there.
void agent_modify_save_merge(AgentPacking& p);

}  // namespace omega
