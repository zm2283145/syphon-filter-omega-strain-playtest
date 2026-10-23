// Port-only (checkpoint 132md4): see mod_fingerprint.hpp.
#include "mod_fingerprint.hpp"

#include "data_overrides.hpp"
#include "mod_archive.hpp"
#include "mod_set.hpp"
#include "online_hash.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <mutex>
#include <utility>

namespace omega::mods {

namespace fs = std::filesystem;

online::ModClass entry_class(std::string_view kind, std::string_view key) {
    if (kind == "data") return online::ModClass::Gameplay;   // 132md16
    if (kind != "files") return online::ModClass::Cosmetic;
    const auto slash = key.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? key : key.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return online::ModClass::Gameplay;
    const std::string ext = upper(name.substr(dot + 1));
    static constexpr std::array<std::string_view, 4> kPresentation = {"TDX", "FNT", "VAG", "VPK"};
    return std::find(kPresentation.begin(), kPresentation.end(), ext) != kPresentation.end() ? online::ModClass::Cosmetic
                                                                                               : online::ModClass::Gameplay;
}

std::string mods_fingerprint(const ModSet& set) {
    if (set.mods().empty()) return {};
    std::string text;
    for (const Mod& mod : set.mods()) {
        text += "mod " + upper(mod.folder) + "\n";
        std::vector<std::pair<std::string, fs::path>> files;
        // Checkpoint 132md10: a packed mod's members (relative to its root) fingerprint as the folder's files do.
        for (const auto& path : mod_files_below(mod.root)) files.emplace_back(path.lexically_relative(mod.root).generic_string(), path);
        std::sort(files.begin(), files.end());
        for (const auto& [rel, path] : files) {
            const auto bytes = read_file(path);
            text += rel + " " + online::to_hex(online::sha256(std::span<const std::uint8_t>(bytes))) + "\n";
        }
    }
    return online::to_hex(online::sha256(std::string_view(text)));
}

std::string gameplay_fingerprint(const ModSet& set) {
    std::vector<std::string> lines;
    for (const auto& e : set.entries()) {
        if (e.kind != "files" || entry_class(e.kind, e.key) != online::ModClass::Gameplay) continue;
        const auto bytes = read_file(e.source.path);
        lines.push_back("files " + e.key + " " + online::to_hex(online::sha256(std::span<const std::uint8_t>(bytes))) + "\n");
    }
    for (const auto& d : set.data()) {
        char value[64];
        const auto [end, error] = std::to_chars(value, value + sizeof value, d.item.value);
        lines.push_back("data " + data_entry_key(d.item) + " = " + std::string(value, error == std::errc{} ? end : value) + "\n");
    }
    if (lines.empty()) return {};
    std::sort(lines.begin(), lines.end());
    std::string text;
    for (const auto& line : lines) text += line;
    return online::to_hex(online::sha256(std::string_view(text)));
}

online::ModReport online_report(const ModSet* set) {
    online::ModReport out;
    if (!set || set->mods().empty()) return out;
    out.mods = static_cast<std::uint32_t>(set->mods().size());
    for (const auto& e : set->entries())
        (entry_class(e.kind, e.key) == online::ModClass::Gameplay ? out.gameplay_entries : out.cosmetic_entries) += 1;
    out.mod_class = out.gameplay_entries ? online::ModClass::Gameplay
                    : out.cosmetic_entries ? online::ModClass::Cosmetic
                                           : online::ModClass::None;
    out.mod_fingerprint = mods_fingerprint(*set);
    out.gameplay_fingerprint = gameplay_fingerprint(*set);
    for (const Mod& mod : set->mods()) {
        std::string item = mod.folder + (mod.manifest.version.empty() ? std::string() : " " + mod.manifest.version);
        if (!out.summary.empty()) item = ", " + item;
        if (out.summary.size() + item.size() > online::kMaxModSummary) {
            if (out.summary.size() + 5 <= online::kMaxModSummary) out.summary += ", ...";
            break;
        }
        out.summary += item;
    }
    return out;
}

online::ModReport active_online_report() {
    static std::mutex mutex;
    static std::uint64_t cached_generation = ~0ull;
    static online::ModReport cached;
    std::lock_guard<std::mutex> lock(mutex);
    const std::uint64_t g = generation();
    if (g != cached_generation) {
        cached = online_report(active());
        cached_generation = g;
    }
    return cached;
}

}  // namespace omega::mods
