#pragma once
// PORT-ONLY (not in the original; checkpoint 132md5, research/MODDING.md "LAN: the mod report at join",
// research/LAN_PLAY.md): the LAN game's mod check at join, with no omega_server. The original's PS2 games read only
// their discs and exchange nothing about mods.
//  * A joiner appends its mod report (online_mod_report.hpp: the class none / cosmetic / gameplay, the SHA-256
//    fingerprint of the enabled mods' files, counts and summary; mods::active_online_report) to its JOIN_REQ as the
//    transport's join extension: "SFMR", u8 version 1, then encode_mod_report's bytes.
//  * The host's LanModGate is the transport's join filter. Its policy (port setting lan_mod_policy=, play's
//    --lan-mod-policy) decides: gameplay (checkpoint 132md16, the default: the joiner's gameplay set, its report's
//    gameplay fingerprint, must be the host's own; cosmetic mods may differ), any (every joiner, its class only
//    shown), cosmetic (no gameplay-changing mods), none (no mods) or equal (the joiner's fingerprint must be the
//    host's own: the same mod files, or both none). A refused joiner gets JOIN_NAK Refused with the reason, which its join modal shows. The host logs (and
//    the front end shows) each joiner's name, class and fingerprint, accepted or refused.
// The report is self-declared, as online: a modified client can lie. It keeps honest players with different or
// gameplay-changing mods out of a game that asks; it is not anti-cheat.
#include "lan_transport.hpp"
#include "online_mod_report.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::lan {

enum class LanModPolicy { Any, Cosmetic, None, Equal, Gameplay };
// gameplay | any | cosmetic | none | equal (any case)
std::optional<LanModPolicy> parse_lan_mod_policy(std::string_view text);
const char* lan_mod_policy_name(LanModPolicy policy);

// The join extension that carries `report`, and the report back from one (nullopt: empty, not a mod report, or a
// malformed one).
std::vector<std::uint8_t> lan_mod_extension(const online::ModReport& report);
std::optional<online::ModReport> lan_mod_report(std::span<const std::uint8_t> extension);

// Why `policy` refuses a joiner whose report is `joiner` (null: it sent none) in a game whose host has `host`;
// nullopt: allowed. The text is the joiner's notice ("Your mods ...; this game allows ...").
std::optional<std::string> lan_mod_refusal(LanModPolicy policy, const online::ModReport* joiner,
                                           const online::ModReport& host);

// A short line about a report for the host's notice: "mods none", "mods cosmetic (2, fingerprint 1a2b3c4d)",
// "no mod report".
std::string lan_mod_brief(const online::ModReport* report);

class LanModGate {
public:
    using ReportFn = std::function<online::ModReport()>;
    using LogFn = std::function<void(const std::string& line)>;

    // `own` is the host's report (asked at each check: the equal policy compares with it).
    LanModGate(LanModPolicy policy, ReportFn own, LogFn log);

    LanModPolicy policy() const { return policy_; }
    // The join filter (LanJoinFilter's arguments). Logs "LAN joiner NAME (address): <brief>, accepted (policy P)"
    // once per accepted join, and "... refused (policy P): <why>" once per joiner and reason (the joiner retries
    // its JOIN_REQ every 250 ms, and each retry is refused again).
    std::optional<std::string> check(const LanAddress& from, const std::string& name,
                                     std::span<const std::uint8_t> extension);

private:
    LanModPolicy policy_;
    ReportFn own_;
    LogFn log_;
    std::map<std::string, std::string> refused_;  // "address name" -> the reason last logged
};

}  // namespace omega::lan
