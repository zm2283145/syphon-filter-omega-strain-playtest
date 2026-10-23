#pragma once
// Port-only (checkpoint 132md16, research/MODDING.md "Gameplay data overrides"): a mod's data_overrides= files, the
// tuning tables a mod may change by field name. Not game code: the PS2 game reads its tables from the disc (and one
// from its program image) and has no override of any of them. Three tables:
//   [weapon <record id | display key>]   COMMON\WEAPONS.WDB, the weapon database the loader 00147400 reads (0x140-byte
//                                        records after a 0x10-byte header; COMBAT.md section 1, WEAPONS.md). Every
//                                        reader (the play, the front end's loadout and award screens) goes through
//                                        the disc layer, which serves the patched file.
//   [npc <LEVEL | *> <id | player | *>]  GAMEDATA\<LEVEL>\DATA.POP's NPC: records (001b6678 / make_npc_roster): the two
//                                        parameter words, health and armor (make_actor_health 0x3398 / 0x339c).
//                                        [npc <LEVEL> <id>] and [npc <LEVEL> player] take health= / armor=; [npc
//                                        <LEVEL> *] and [npc * *] take health_percent= / armor_percent= (every AI
//                                        record of the level or of every level; record 0, the player, excluded).
//   [tohit]                              game::ToHit, the ToHitSettings static at 0048ac80 (engine_globals.cpp;
//                                        COMBAT.md "ToHit"): in the program image, not on the disc; the play's
//                                        combat aim (ToHitSettings) and the mission scripts' game::ToHit start from
//                                        it.
// The game has no difficulty table (ACTOR_DAMAGE_AND_HEALTH.md): [npc * *]'s percentages are the port's knob for one.
// A file is INI: "[section]" lines, "field = value" lines, '#' / ';' comments. Names are case-insensitive; values are
// range-checked (data_fields). Every value is what the game uses: a weapon whose damage type (+bc) is 1 has its damage
// words divided by 5 at load (00147400), so the port stores 5x the value given there.
#include "mod_set.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
struct ToHitSettings;
}

namespace omega::mods {

enum class DataFieldType : std::uint8_t { i32, f32, u8 };

struct DataField {
    std::string_view name;       // as written in a file (lower case; ToHit: the script's names)
    std::size_t offset;          // within a WDB record; the NPC record's parameter words (0 / 4); ToHit: index * 4
    DataFieldType type;
    double min, max;
    std::string_view meaning;
};
// The fields a table ("weapon" / "npc" / "tohit") offers; empty for any other name.
std::span<const DataField> data_fields(std::string_view table);
const DataField* find_data_field(std::string_view table, std::string_view name);

// One file's overrides, in file order. Throws "<file> line N: ..." on a malformed line, an unknown section, table or
// field, a field the section does not take, a value of the wrong type or out of range, or a field given twice in one
// section.
std::vector<DataOverride> parse_data_overrides(std::string_view text, std::string_view file);

// "weapon BARETTA_92F damage_near" (the entry key ModSet::entries gives).
std::string data_entry_key(const DataOverride& item);

// The disc file keys a set's data overrides patch: "GAMEDATA/COMMON/WEAPONS.WDB" and "GAMEDATA/<LEVEL>/DATA.POP".
bool is_data_file_key(std::string_view key);

// What one patched value was (the notes, the mod tool's listing).
struct DataChange {
    std::string what;     // "WEAPONS.WDB record 112 SocomSilenced damage_near" / "TORONTO1 DATA.POP NPC 133140 health"
    double from = 0, to = 0;
    std::string mod;      // the folder whose value is used
};
// Patches `bytes` (the disc's or a files\ replacement's) of a data file key with the set's overrides (the first mod
// in resolution order winning each field). nullopt: the key is not a data file or no override applies to it. A
// selector naming no record (a weapon id past the count, a key no record has, an NPC id the level lacks) changes
// nothing (the mod tool warns). Throws on a malformed file.
std::optional<std::vector<std::uint8_t>> apply_data_overrides(const ModSet& set, std::string_view key,
                                                              std::span<const std::uint8_t> bytes,
                                                              std::vector<DataChange>* changes = nullptr);

// The set's [tohit] values over `settings` (the play's combat aim); returns how many fields changed.
std::size_t apply_tohit_overrides(const ModSet& set, ToHitSettings& settings);
// The set's [tohit] values by the script's field names ("npcExtraAimTime"), the winning mod's.
std::vector<std::pair<std::string, float>> tohit_overrides(const ModSet& set);

// The weapons database's record count and display keys (the mod tool's checks); throws on a malformed file.
std::vector<std::string> weapon_record_keys(std::span<const std::uint8_t> wdb);

}  // namespace omega::mods
