// PORT-ONLY: the LAN game's mod check at join (see lan_mod_gate.hpp).
#include "lan_mod_gate.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace omega::lan {

namespace {

constexpr std::uint8_t kTag[4] = {'S', 'F', 'M', 'R'};
constexpr std::uint8_t kVersion = 1;

const char* allows(LanModPolicy policy) {
    switch (policy) {
    case LanModPolicy::Cosmetic: return "this game allows cosmetic mods only";
    case LanModPolicy::None: return "this game allows no mods";
    case LanModPolicy::Equal: return "this game requires the host's mods";
    case LanModPolicy::Gameplay: return "this game requires the host's gameplay mods (cosmetic mods may differ)";
    default: return "this game allows any mods";
    }
}

std::string count_of(std::uint32_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string short_fingerprint(const std::string& fingerprint) {
    return fingerprint.empty() ? std::string("none") : fingerprint.substr(0, 8);
}

}  // namespace

std::optional<LanModPolicy> parse_lan_mod_policy(std::string_view text) {
    std::string t(text);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "any") return LanModPolicy::Any;
    if (t == "cosmetic") return LanModPolicy::Cosmetic;
    if (t == "none") return LanModPolicy::None;
    if (t == "equal") return LanModPolicy::Equal;
    if (t == "gameplay") return LanModPolicy::Gameplay;
    return std::nullopt;
}

const char* lan_mod_policy_name(LanModPolicy policy) {
    switch (policy) {
    case LanModPolicy::Cosmetic: return "cosmetic";
    case LanModPolicy::None: return "none";
    case LanModPolicy::Equal: return "equal";
    case LanModPolicy::Gameplay: return "gameplay";
    default: return "any";
    }
}

std::vector<std::uint8_t> lan_mod_extension(const online::ModReport& report) {
    std::vector<std::uint8_t> out(std::begin(kTag), std::end(kTag));
    out.push_back(kVersion);
    const auto body = online::encode_mod_report(report);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<online::ModReport> lan_mod_report(std::span<const std::uint8_t> extension) {
    if (extension.size() < sizeof kTag + 1 || !std::equal(std::begin(kTag), std::end(kTag), extension.begin()) ||
        extension[sizeof kTag] != kVersion)
        return std::nullopt;
    return online::decode_mod_report(extension.subspan(sizeof kTag + 1));
}

std::optional<std::string> lan_mod_refusal(LanModPolicy policy, const online::ModReport* joiner,
                                           const online::ModReport& host) {
    if (policy == LanModPolicy::Any) return std::nullopt;
    if (policy == LanModPolicy::Gameplay) {
        // 132md16: no report is unknown, let through only when the host has no gameplay mods.
        if (!online::gameplay_refusal(joiner, false, &host, false)) return std::nullopt;
        const auto yours = !joiner ? std::string("no mod report came with the join")
                           : !joiner->gameplay_known ? std::string("yours are from an older build and cannot be compared")
                           : online::no_gameplay_mods(*joiner) ? std::string("you have none")
                                                               : "yours " + joiner->gameplay_fingerprint.substr(0, 8);
        const auto hosts = online::no_gameplay_mods(host) ? std::string("the host has none")
                                                          : "the host's " + host.gameplay_fingerprint.substr(0, 8);
        return "Your gameplay mods differ from the host's (" + yours + ", " + hosts + "); " + allows(policy) + ".";
    }
    if (!joiner) return std::string("No mod report came with the join; ") + allows(policy) + ".";
    switch (policy) {
    case LanModPolicy::Cosmetic:
        if (joiner->mod_class == online::ModClass::Gameplay)
            return "Your mods change gameplay (" + count_of(joiner->gameplay_entries, "gameplay entry", "gameplay entries") +
                   "); " +
                   allows(policy) + ".";
        return std::nullopt;
    case LanModPolicy::None:
        if (joiner->mod_class != online::ModClass::None)
            return "You have mods enabled (" + std::string(online::mod_class_name(joiner->mod_class)) + ", " +
                   count_of(joiner->cosmetic_entries + joiner->gameplay_entries, "entry", "entries") + "); " +
                   allows(policy) + ".";
        return std::nullopt;
    case LanModPolicy::Equal:
        if (joiner->mod_fingerprint != host.mod_fingerprint)
            return "Your mods differ from the host's (yours " + short_fingerprint(joiner->mod_fingerprint) +
                   ", the host's " + short_fingerprint(host.mod_fingerprint) + "); " + allows(policy) + ".";
        return std::nullopt;
    default: return std::nullopt;
    }
}

std::string lan_mod_brief(const online::ModReport* report) {
    if (!report) return "no mod report";
    if (report->mod_class == online::ModClass::None && report->mods == 0) return "mods none";
    return std::string("mods ") + online::mod_class_name(report->mod_class) + " (" + std::to_string(report->mods) +
           " enabled, fingerprint " + short_fingerprint(report->mod_fingerprint) +
           (!report->gameplay_known ? std::string(", gameplay unknown")
            : report->gameplay_fingerprint.empty() ? std::string()
                                                   : ", gameplay " + short_fingerprint(report->gameplay_fingerprint)) +
           ")";
}

LanModGate::LanModGate(LanModPolicy policy, ReportFn own, LogFn log)
    : policy_(policy), own_(std::move(own)), log_(std::move(log)) {}

std::optional<std::string> LanModGate::check(const LanAddress& from, const std::string& name,
                                             std::span<const std::uint8_t> extension) {
    const auto report = lan_mod_report(extension);
    const online::ModReport host = own_ ? own_() : online::ModReport{};
    auto why = lan_mod_refusal(policy_, report ? &*report : nullptr, host);
    const std::string who = "LAN joiner " + name + " (" + lan_address_text(from) + "): " +
                            lan_mod_brief(report ? &*report : nullptr);
    const std::string key = lan_address_text(from) + " " + name;
    if (!why) {
        refused_.erase(key);
        if (log_) log_(who + ", accepted (policy " + lan_mod_policy_name(policy_) + ")");
        return std::nullopt;
    }
    auto& last = refused_[key];
    if (last != *why) {
        last = *why;
        if (log_) log_(who + ", refused (policy " + lan_mod_policy_name(policy_) + "): " + *why);
    }
    return why;
}

}  // namespace omega::lan
