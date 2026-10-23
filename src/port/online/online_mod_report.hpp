#pragma once
// PORT-ONLY (not in the original; checkpoint 132md4, research/ONLINE.md §6.9, research/MODDING.md "Online: the mod
// report"): what a port client tells omega_server about its mods, and the server's mod policy. The original's PS2
// sessions read only their discs and never send (nor receive) this.
//  * The client sends ext ModReport (0xF060) right after its hello: the class of the mods it has installed (none /
//    cosmetic / gameplay, the client's own classification of every asset its mods replace), a SHA-256 fingerprint of
//    the enabled mods' files, counts and a short summary ("folder version, ...").
//  * The server's mod_policy (config key mod_policy=, --mod-policy) decides who may create or join a game: gameplay
//    (checkpoint 132md16, the default: a joiner's gameplay set must be the game creator's; cosmetic mods free), any
//    (nothing checked), cosmetic (no gameplay-changing mods) or none (no mods at all). An original (PS2) session
//    cannot report and is let through by cosmetic / none (its gameplay set is empty under gameplay); a port session
//    that sent no report counts as unknown and is refused by cosmetic / none (and by gameplay against a creator with
//    gameplay mods).
//  * OmegaServer applies the policy (132on2c folded 132md4's ModGate into it; online_server.cpp on_ext /
//    on_create_game / on_join_game): it keeps each port session's last report (ext::ModReport, cap::Mods in
//    online_protocol.hpp) and answers a refused CreateGame / JoinGame before its world logic (RequestDenied, plus an
//    ext::Notice when the session has cap::Notices). The report is self-declared: a modified client can lie. It
//    keeps honest players with gameplay mods out of games that ask for none; it is not anti-cheat.

#include "online_protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

enum class ModClass : std::uint8_t { None = 0, Cosmetic = 1, Gameplay = 2 };

struct ModReport {
    ModClass mod_class = ModClass::None;
    std::string mod_fingerprint;  // 64 lower-case hex digits (SHA-256); empty with no mods
    std::uint32_t mods = 0;       // enabled mods
    std::uint32_t cosmetic_entries = 0, gameplay_entries = 0;
    std::string summary;          // "folder version, ..." (at most kMaxModSummary characters)
    // Checkpoint 132md16: SHA-256 over the gameplay entries alone (mods::gameplay_fingerprint: files\ replacements of
    // the gameplay class with their bytes' hashes, data_overrides= fields with their values; not the folder names).
    // Empty: no gameplay entries. Two sessions with equal ones play the same game whatever their cosmetic mods.
    std::string gameplay_fingerprint;
    // False: a report from a build before 132md16 (no gameplay fingerprint) of the gameplay class; its gameplay set is
    // unknown. Such a report of class none / cosmetic has an empty gameplay set.
    bool gameplay_known = true;
};
constexpr std::size_t kMaxModSummary = 200;

// [u8 class][str fingerprint][u32 mods][u32 cosmetic][u32 gameplay][str summary][str gameplay fingerprint]
// (ExtWriter's layout; the last field is 132md16's: older readers ignore it, and a body without it decodes with
// gameplay_known false for the gameplay class).
std::vector<std::uint8_t> encode_mod_report(const ModReport& report);
// nullopt on a short body, a class above Gameplay, a fingerprint (either) that is neither empty nor 64 hex digits, or
// a summary above kMaxModSummary.
std::optional<ModReport> decode_mod_report(std::span<const std::uint8_t> body);

enum class ModPolicy { Any, Cosmetic, None, Gameplay };
// "gameplay" | "any" | "cosmetic" | "none" (any case)
std::optional<ModPolicy> parse_mod_policy(std::string_view text);
const char* mod_policy_name(ModPolicy policy);
const char* mod_class_name(ModClass mod_class);

// Why `policy` refuses a session (nullopt: allowed). `report` null: the session sent none; `original`: a PS2 session.
// The gameplay policy refuses nothing here (creating a game is free); a join asks gameplay_refusal.
std::optional<std::string> mod_refusal(ModPolicy policy, const ModReport* report, bool original);

// Checkpoint 132md16 (the gameplay policies, LAN and online): why a joiner may not play in a game whose creator / host
// has `host`; nullopt: their gameplay sets are equal. A null report (none sent) and an original (PS2) session count as
// an empty set (so sessions without mods play as before the policy); a pre-132md16 gameplay-class report
// (gameplay_known false) cannot be compared and is refused.
std::optional<std::string> gameplay_refusal(const ModReport* joiner, bool joiner_original, const ModReport* host,
                                            bool host_original);
// Whether a report's gameplay set is empty (known and no gameplay entries).
bool no_gameplay_mods(const ModReport& report);

// One line about a report (logs, the server console's "mods").
std::string describe_mod_report(const ModReport& report);

}  // namespace omega::online
