#pragma once
// Port-only (checkpoint 132md4, research/MODDING.md "Online: the mod report"): the report a port client sends
// omega_server about its mods (online_mod_report.hpp): the class of every asset the enabled mods replace and a
// fingerprint of their files. Not game code.
//  * cosmetic: textures\ (path and by-hash), models\ (character parts keep the original skeleton, level models the
//    original collision), sounds\, and files\ replacements of presentation-only types (.TDX textures, .FNT fonts,
//    .VAG / .VPK sound streams);
//  * gameplay: every other files\ replacement (mission data, levels, collision, skeletons, appearance manifests,
//    strings, whole archives): anything that can change what the game does; and (checkpoint 132md16) every
//    data_overrides= field (weapon, NPC and ToHit tuning);
//  * none: no mod installed, or none whose entries apply.
// "Cosmetic" is not "fair": an image or a model can still hide or reveal things; a server that wants an unmodded
// game asks for mod_policy=none.

#include "online_mod_report.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace omega::mods {

class ModSet;

// The class of one ModSet entry ("files" / "textures" / "by-hash" / "models" / "sounds" / "data" and its key).
online::ModClass entry_class(std::string_view kind, std::string_view key);

// SHA-256 over every enabled mod's files in resolution order: per mod "mod <FOLDER>\n", then per file (sorted by
// its '/'-joined relative path) "<path> <SHA-256 of its bytes>\n". Empty with no mods.
std::string mods_fingerprint(const ModSet& set);
// Checkpoint 132md16: SHA-256 over the set's gameplay entries alone, sorted: "files <KEY> <SHA-256 of the winning
// file's bytes>\n" per gameplay-class files\ entry and "data <entry key> = <value>\n" per data_overrides= field (the
// winning mod's). Folder names, versions and cosmetic entries do not count: two sets that change the game alike
// match. Empty when the set has no gameplay entries.
std::string gameplay_fingerprint(const ModSet& set);

// The report for `set` (null: no mods).
online::ModReport online_report(const ModSet* set);
// The report for the installed set (active()), computed once per install generation (the fingerprint reads every
// mod file).
online::ModReport active_online_report();

}  // namespace omega::mods
