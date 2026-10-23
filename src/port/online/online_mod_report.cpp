// PORT-ONLY (checkpoint 132md4): see online_mod_report.hpp.
#include "online_mod_report.hpp"

#include <algorithm>
#include <cctype>

namespace omega::online {

namespace {
bool hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}  // namespace

std::vector<std::uint8_t> encode_mod_report(const ModReport& report) {
    return ExtWriter()
        .u8(static_cast<std::uint8_t>(report.mod_class))
        .str(report.mod_fingerprint)
        .u32(report.mods)
        .u32(report.cosmetic_entries)
        .u32(report.gameplay_entries)
        .str(report.summary.substr(0, kMaxModSummary))
        .str(report.gameplay_fingerprint)
        .bytes();
}

std::optional<ModReport> decode_mod_report(std::span<const std::uint8_t> body) {
    ExtReader r(body);
    ModReport out;
    const std::uint8_t mod_class = r.u8();
    out.mod_fingerprint = r.str();
    out.mods = r.u32();
    out.cosmetic_entries = r.u32();
    out.gameplay_entries = r.u32();
    out.summary = r.str();
    if (r.ok() && r.at_end()) out.gameplay_known = mod_class != static_cast<std::uint8_t>(ModClass::Gameplay);
    else out.gameplay_fingerprint = r.str();
    if (!r.ok() || mod_class > static_cast<std::uint8_t>(ModClass::Gameplay)) return std::nullopt;
    if (!out.mod_fingerprint.empty() && !hex64(out.mod_fingerprint)) return std::nullopt;
    if (!out.gameplay_fingerprint.empty() && !hex64(out.gameplay_fingerprint)) return std::nullopt;
    if (out.summary.size() > kMaxModSummary) return std::nullopt;
    out.mod_class = static_cast<ModClass>(mod_class);
    return out;
}

std::optional<ModPolicy> parse_mod_policy(std::string_view text) {
    std::string t(text);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "any") return ModPolicy::Any;
    if (t == "cosmetic") return ModPolicy::Cosmetic;
    if (t == "none") return ModPolicy::None;
    if (t == "gameplay") return ModPolicy::Gameplay;
    return std::nullopt;
}

const char* mod_policy_name(ModPolicy policy) {
    switch (policy) {
    case ModPolicy::Cosmetic: return "cosmetic";
    case ModPolicy::None: return "none";
    case ModPolicy::Gameplay: return "gameplay";
    default: return "any";
    }
}

const char* mod_class_name(ModClass mod_class) {
    switch (mod_class) {
    case ModClass::Cosmetic: return "cosmetic";
    case ModClass::Gameplay: return "gameplay";
    default: return "none";
    }
}

std::optional<std::string> mod_refusal(ModPolicy policy, const ModReport* report, bool original) {
    if (policy == ModPolicy::Any || policy == ModPolicy::Gameplay || original) return std::nullopt;
    if (!report) return std::string("no mod report (a port build without one); this server allows ") +
                        (policy == ModPolicy::None ? "no mods" : "cosmetic mods only");
    if (report->mod_class == ModClass::Gameplay)
        return "gameplay-changing mods (" + std::to_string(report->gameplay_entries) + " gameplay entries); this server allows " +
               (policy == ModPolicy::None ? "no mods" : "cosmetic mods only");
    if (policy == ModPolicy::None && report->mod_class != ModClass::None)
        return "cosmetic mods (" + std::to_string(report->cosmetic_entries) + " entries); this server allows no mods";
    return std::nullopt;
}

bool no_gameplay_mods(const ModReport& report) { return report.gameplay_known && report.gameplay_fingerprint.empty(); }

std::optional<std::string> gameplay_refusal(const ModReport* joiner, bool joiner_original, const ModReport* host,
                                            bool host_original) {
    // No report (an original session, a port build without the mod report, a client that sent none) counts as no
    // gameplay mods: the policy is the default, and sessions that played together before it still do. A pre-132md16
    // report of the gameplay class (gameplay_known false) has gameplay mods that cannot be compared: always refused.
    const auto empty = [](const ModReport* r, bool original) { return original || !r || no_gameplay_mods(*r); };
    const auto print = [](const ModReport* r, bool original) -> std::string {
        if (original || !r) return "none (no mod report)";
        if (no_gameplay_mods(*r)) return "none";
        if (!r->gameplay_known) return "unknown (an older build's report)";
        return r->gameplay_fingerprint.substr(0, 8);
    };
    const bool joiner_empty = empty(joiner, joiner_original), host_empty = empty(host, host_original);
    if (joiner_empty && host_empty) return std::nullopt;
    if (!joiner_empty && !host_empty && joiner->gameplay_known && host->gameplay_known &&
        joiner->gameplay_fingerprint == host->gameplay_fingerprint)
        return std::nullopt;
    return "your gameplay mods differ from the host's (yours " + print(joiner, joiner_original) + ", the host's " +
           print(host, host_original) + "); this game requires the host's gameplay mods (cosmetic mods may differ)";
}

std::string describe_mod_report(const ModReport& report) {
    if (report.mod_class == ModClass::None && report.mods == 0) return "mods none";
    std::string out = std::string("mods ") + mod_class_name(report.mod_class) + ", " + std::to_string(report.mods) +
                      " enabled, " + std::to_string(report.cosmetic_entries) + " cosmetic / " +
                      std::to_string(report.gameplay_entries) + " gameplay entries";
    if (!report.mod_fingerprint.empty()) out += ", fingerprint " + report.mod_fingerprint.substr(0, 16);
    if (!report.gameplay_known) out += ", gameplay set unknown";
    else if (!report.gameplay_fingerprint.empty()) out += ", gameplay " + report.gameplay_fingerprint.substr(0, 16);
    if (!report.summary.empty()) out += " (" + report.summary + ")";
    return out;
}

}  // namespace omega::online
