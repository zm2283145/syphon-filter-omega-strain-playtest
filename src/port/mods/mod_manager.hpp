#pragma once
// Port-only (checkpoint 132md5, research/MODDING.md "Managing mods"): the mod management the launcher's mod panel
// (launcher_mod_panel.hpp) and omega_mod_tool share, over list_mods / ModSet (mod_set.hpp) and the port settings. Not
// game code. Nothing here runs unless a player or a modder asks for it (the panel, the tool): a session's resolution
// is ModSet::scan's alone.
//   - the state: every mod folder (list_mods, the settings' mod.<folder>= choices applied), mods=, what each enabled
//     mod supplies, and the conflicts of the enabled set as omega_mod_tool conflicts prints them;
//   - enable / disable: mod.<folder>=0|1 in the port settings (the in-game MODS rows and omega_mod_tool do the same);
//   - the order: a mod moved up or down the resolution order gets a new manifest priority= (the load order is the
//     manifest's, research/MODDING.md "Several mods"); the fewest manifests change (move_mod).

#include "mod_set.hpp"
#include "port_settings.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega::mods {

// omega_mod_tool conflicts' lines for an enabled set and its scan messages: "ignored: <message>" per ignored entry,
// "shadowed: <key> from <winner> (used) over <others>", "layered: <key> (<winner>): <detail>", then the summary
// "<n> enabled mod(s): <s> shadowed entr(y|ies), <l> layered, <i> ignored".
struct ConflictSummary {
    std::vector<std::string> lines;
    std::size_t shadowed = 0, layered = 0, ignored = 0;
};
ConflictSummary summarize_conflicts(const ModSet& set, const std::vector<std::string>& messages);

struct ModState {
    std::filesystem::path folder;                     // the mods folder (absolute)
    bool mods_on = true;                              // mods= (false: every mod off)
    std::vector<ModListing> listing;                  // list_mods with the settings' choices
    std::map<std::string, std::map<std::string, std::size_t>> used;  // enabled folder -> kind -> entries it supplies
    std::vector<std::string> messages;                // the enabled set's scan log
    ConflictSummary conflicts;
    std::string settings_problem;                     // a malformed settings file (read as the defaults)
    ModVolumes volumes;                               // 132md13: mod_volume.<folder>= (folder upper-cased)
    // Checkpoint 132md10: every entry of the enabled set (the winner and the mods it shadows), in the set's key order,
    // and the enabled mods' load order (resolution order: the first wins, then the disc).
    struct Owner {
        std::string kind, key, winner;
        std::vector<std::string> shadowed;
    };
    std::vector<Owner> owners;
    std::vector<std::string> load_order;
};
// The settings file may be missing (the defaults). A malformed one is reported in settings_problem.
ModState read_mod_state(const std::filesystem::path& mods_folder, const std::filesystem::path& settings_file);
// The mods folder a session uses: `flag` (--mods) if given, else the settings' mods_folder=, else "mods".
std::filesystem::path session_mods_folder(const PortSettings& s, const std::filesystem::path& flag = {});

// mod.<folder>=0|1 (the folder's earlier lines of any case removed). Throws when the folder name cannot be a settings
// key ('=' or '#', or leading / trailing blanks).
void set_mod_choice(PortSettings& s, const std::string& folder, bool on);
// Checkpoint 132md13: mod_volume.<folder>=<percent> (0..400; the folder's earlier lines of any case removed). Throws as
// set_mod_choice, and on a percent past 400.
void set_mod_volume(PortSettings& s, const std::string& folder, unsigned percent);
// mods=0|1.
void set_mods_on(PortSettings& s, bool on);

// The manifest text with `key`=value: the first line of that key (any case) rewritten in place (its line ending
// kept), else the line appended. The result must still parse (parse_manifest throws otherwise).
std::string with_manifest_value(std::string_view text, std::string_view key, std::string_view value);

// A move of listing[index] (a valid manifest) by `delta` places in the resolution order (-1: up, toward first;
// +1: down). Returns the manifests whose priority= changes: either the mod that ends on top of the swapped pair is
// raised over the other (and mods above it raised as the order needs) or the one below lowered under it (and mods
// below lowered); the fewer manifests change, on a tie the moved mod's own (equal priorities resolve by folder name).
// Empty when the move is not possible (an end of the list, an invalid manifest).
struct PriorityChange {
    std::string folder;
    std::filesystem::path root;
    int from = 0, to = 0;
    std::filesystem::path archive;   // checkpoint 132md10: a packed mod (its manifest member is rewritten in the .zip)
};
std::vector<PriorityChange> move_mod(const std::vector<ModListing>& listing, std::size_t index, int delta);
// Writes each change's manifest.txt priority= (with_manifest_value; the file is read again first and must still
// have the priority the change expects). A packed mod's manifest is replaced inside its .zip (replace_zip_member: the
// other members copied unchanged).
void apply_priority_changes(const std::vector<PriorityChange>& changes);

// The mod panel's texts. A row: "ON   10  Name 1.2 by Author  [folder]" ("OFF", "BAD" for an invalid manifest:
// "BAD   -  <problem>  [folder]"); a '*' after the state marks a mod.<folder>= choice over the manifest. A packed mod
// (132md10) shows its file name: "[hd.zip]".
std::string mod_row_text(const ModListing& listing);
// The selected mod's details (none selected: an empty block) followed by the enabled set's conflicts and its scan log.
// Checkpoint 132md10: the details add the archive, game_version (a mismatch flagged) and the notes.<platform>= lines
// (this platform's first), the mod's place in the load order and, per contested entry, which mod wins it ("wins
// <entry> over <mods>" / "loses <entry> to <mod>"; up to max_detail_entries lines, then a count); the set's block
// starts with the load order.
inline constexpr std::size_t max_detail_entries = 200;
std::vector<std::string> mod_details(const ModState& state, std::optional<std::size_t> selected);

}  // namespace omega::mods
