#include "mod_manager.hpp"

#include "mod_archive.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace omega::mods {
namespace {

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string text;
    for (const auto& item : items) text += (text.empty() ? "" : separator) + item;
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void set_other(PortSettings& s, const std::string& key, const std::string& value, bool any_case) {
    bool set = false;
    for (auto it = s.other.begin(); it != s.other.end();) {
        const bool same = any_case ? upper(it->first) == upper(key) : it->first == key;
        if (!same) {
            ++it;
            continue;
        }
        if (set) {
            it = s.other.erase(it);
            continue;
        }
        *it = {key, value};
        set = true;
        ++it;
    }
    if (!set) s.other.emplace_back(key, value);
}

// listing[a] resolves before listing[b] with these priorities (list_mods / ModSet::from_mods' order).
bool before(int pa, const std::string& a, int pb, const std::string& b) {
    if (pa != pb) return pa > pb;
    return upper(a) < upper(b);
}

}  // namespace

ConflictSummary summarize_conflicts(const ModSet& set, const std::vector<std::string>& messages) {
    ConflictSummary summary;
    for (const auto& m : messages)
        if (m.find(": ignored ") != std::string::npos) {
            summary.lines.push_back("ignored: " + m);
            ++summary.ignored;
        }
    for (const auto& c : set.conflicts()) {
        if (c.kind == "shadowed") {
            ++summary.shadowed;
            summary.lines.push_back("shadowed: " + c.key + " from " + c.winner + " (used) over " + join(c.others, ", "));
        } else {
            ++summary.layered;
            summary.lines.push_back("layered: " + c.key + " (" + c.winner + "): " + c.detail);
        }
    }
    summary.lines.push_back(std::to_string(set.mods().size()) + " enabled mod(s): " + std::to_string(summary.shadowed) +
                            " shadowed entr" + (summary.shadowed == 1 ? "y" : "ies") + ", " + std::to_string(summary.layered) +
                            " layered, " + std::to_string(summary.ignored) + " ignored");
    return summary;
}

std::filesystem::path session_mods_folder(const PortSettings& s, const std::filesystem::path& flag) {
    if (!flag.empty()) return flag;
    const std::string folder = mods_folder_setting(s);
    if (!folder.empty()) return std::filesystem::path(std::u8string(folder.begin(), folder.end()));
    return "mods";
}

ModState read_mod_state(const std::filesystem::path& mods_folder, const std::filesystem::path& settings_file) {
    ModState state;
    state.folder = std::filesystem::absolute(mods_folder);
    ModChoices choices;
    std::error_code error;
    if (std::filesystem::exists(settings_file, error)) {
        try {
            const auto s = read_port_settings(settings_file);
            state.mods_on = mods_setting(s);
            for (const auto& [folder, on] : mod_choices_setting(s)) choices[folder] = on;
            for (const auto& [folder, percent] : mod_volumes_setting(s)) state.volumes[folder] = percent;
        } catch (const std::exception& e) {
            state.settings_problem = e.what();
            choices.clear();
            state.volumes.clear();
        }
    }
    state.listing = list_mods(state.folder, choices);
    const auto set = ModSet::scan(state.folder, state.messages, choices);
    set->set_volumes(state.volumes);
    for (const auto& entry : set->entries()) {
        ++state.used[entry.source.mod->folder][entry.kind];
        state.owners.push_back({entry.kind, entry.key, entry.source.mod->folder, entry.shadowed});
    }
    for (const auto& mod : set->mods()) state.load_order.push_back(mod.folder);
    state.conflicts = summarize_conflicts(*set, state.messages);
    return state;
}

void set_mod_choice(PortSettings& s, const std::string& folder, bool on) {
    if (folder.empty() || folder.find_first_of("=#") != std::string::npos || folder.front() == ' ' || folder.back() == ' ')
        throw std::runtime_error("the folder name '" + folder + "' cannot be a settings key");
    set_other(s, "mod." + folder, on ? "1" : "0", true);
}

void set_mod_volume(PortSettings& s, const std::string& folder, unsigned percent) {
    if (folder.empty() || folder.find_first_of("=#") != std::string::npos || folder.front() == ' ' || folder.back() == ' ')
        throw std::runtime_error("the folder name '" + folder + "' cannot be a settings key");
    if (percent > 400) throw std::runtime_error("an audio volume is 0..400 percent, not " + std::to_string(percent));
    set_other(s, "mod_volume." + folder, std::to_string(percent), true);
}

void set_mods_on(PortSettings& s, bool on) { set_other(s, "mods", on ? "1" : "0", false); }

std::string with_manifest_value(std::string_view text, std::string_view key, std::string_view value) {
    const std::string wanted = lower(key);
    const std::string line = std::string(key) + "=" + std::string(value);
    std::string out;
    bool written = false;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        const bool newline = end != std::string_view::npos;
        if (!newline) end = text.size();
        const std::string raw(text.substr(begin, end - begin));
        begin = end + 1;
        std::string name = raw.substr(0, raw.find('='));
        name.erase(0, name.find_first_not_of(" \t"));
        if (const auto last = name.find_last_not_of(" \t\r"); last != std::string::npos) name.erase(last + 1);
        else name.clear();
        if (!written && raw.find('=') != std::string::npos && lower(name) == wanted) {
            out += line + (raw.ends_with('\r') ? "\r" : "") + (newline ? "\n" : "");
            written = true;
            continue;
        }
        out += raw + (newline ? "\n" : "");
    }
    if (!written) {
        const bool crlf = out.find("\r\n") != std::string::npos;
        if (!out.empty() && !out.ends_with('\n')) out += crlf ? "\r\n" : "\n";
        out += line + (crlf ? "\r\n" : "\n");
    }
    (void)parse_manifest(out);
    return out;
}

std::vector<PriorityChange> move_mod(const std::vector<ModListing>& listing, std::size_t index, int delta) {
    std::size_t valid = 0;
    while (valid < listing.size() && listing[valid].manifest) ++valid;
    if (index >= valid || (delta != -1 && delta != 1)) return {};
    if ((delta < 0 && index == 0) || (delta > 0 && index + 1 >= valid)) return {};
    std::vector<std::size_t> order(valid);
    for (std::size_t i = 0; i < valid; ++i) order[i] = i;
    const std::size_t target = delta < 0 ? index - 1 : index + 1;
    std::swap(order[index], order[target]);
    std::vector<int> original(valid);
    for (std::size_t i = 0; i < valid; ++i) original[i] = listing[order[i]].manifest->priority;
    auto name = [&](std::size_t i) -> const std::string& { return listing[order[i]].folder; };
    // The swapped pair is at top / top + 1. Either the new top is raised over the new bottom (and mods above it are
    // raised as the order needs), or the new bottom lowered under the new top (and mods below it lowered).
    const std::size_t top = std::min(index, target);
    auto raised = original;
    {
        auto& p = raised;
        auto above = [&](std::size_t i) { return p[i + 1] + (upper(name(i)) < upper(name(i + 1)) ? 0 : 1); };
        p[top] = above(top);
        for (std::size_t i = top; i-- > 0;)
            if (!before(p[i], name(i), p[i + 1], name(i + 1))) p[i] = above(i);
    }
    auto lowered = original;
    {
        auto& p = lowered;
        auto below = [&](std::size_t i) { return p[i - 1] - (upper(name(i - 1)) < upper(name(i)) ? 0 : 1); };
        p[top + 1] = below(top + 1);
        for (std::size_t i = top + 2; i < valid; ++i)
            if (!before(p[i - 1], name(i - 1), p[i], name(i))) p[i] = below(i);
    }
    auto count = [&](const std::vector<int>& p) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < valid; ++i) n += p[i] != original[i];
        return n;
    };
    // The fewest manifests change; on a tie the moved mod's own priority changes (raised when moved up).
    const auto nr = count(raised), nl = count(lowered);
    const auto& p = nr < nl || (nr == nl && delta < 0) ? raised : lowered;
    std::vector<PriorityChange> changes;
    for (std::size_t i = 0; i < valid; ++i) {
        const auto& m = listing[order[i]];
        if (p[i] != m.manifest->priority) changes.push_back({m.folder, m.root, m.manifest->priority, p[i], m.archive});
    }
    return changes;
}

void apply_priority_changes(const std::vector<PriorityChange>& changes) {
    for (const auto& change : changes) {
        const auto path = change.root / "manifest.txt";
        const auto bytes = read_file(path);
        const std::string text(bytes.begin(), bytes.end());
        if (parse_manifest(text).priority != change.from)
            throw std::runtime_error(change.folder + "'s manifest changed since it was read (priority is no longer " +
                                     std::to_string(change.from) + ")");
        const auto out = with_manifest_value(text, "priority", std::to_string(change.to));
        if (!change.archive.empty()) {
            const auto inside = path.lexically_relative(change.archive).generic_u8string();
            replace_zip_member(change.archive, std::string(inside.begin(), inside.end()), std::vector<std::uint8_t>(out.begin(), out.end()));
            continue;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << out;
        if (!file) throw std::runtime_error("Cannot write " + path.string());
    }
}

std::string mod_row_text(const ModListing& listing) {
    auto pad = [](std::string text, std::size_t width, bool right) {
        if (text.size() >= width) return text;
        return right ? std::string(width - text.size(), ' ') + text : text + std::string(width - text.size(), ' ');
    };
    const auto file = listing.archive.filename().u8string();
    const std::string shown = listing.archive.empty() ? listing.folder : std::string(file.begin(), file.end());
    if (!listing.manifest) return pad("BAD", 5, false) + pad("-", 6, true) + "  " + listing.problem + "  [" + shown + "]";
    const auto& m = *listing.manifest;
    std::string row = pad(std::string(listing.enabled ? "ON" : "OFF") + (listing.chosen ? "*" : ""), 5, false) +
                      pad(std::to_string(m.priority), 6, true) + "  " + m.name;
    if (!m.version.empty()) row += " " + m.version;
    if (!m.author.empty()) row += " by " + m.author;
    return row + "  [" + shown + "]";
}

std::vector<std::string> mod_details(const ModState& state, std::optional<std::size_t> selected) {
    std::vector<std::string> lines;
    const auto place = [&](const std::string& folder) -> std::size_t {
        for (std::size_t i = 0; i < state.load_order.size(); ++i)
            if (state.load_order[i] == folder) return i + 1;
        return 0;
    };
    if (selected && *selected < state.listing.size()) {
        const auto& l = state.listing[*selected];
        const auto file = l.archive.filename().u8string();
        const std::string archive(file.begin(), file.end());
        if (!l.manifest) {
            lines.push_back((l.archive.empty() ? "Folder " + l.folder : "Archive " + archive) + ": no valid manifest (" + l.problem +
                            "); it is never loaded.");
        } else {
            const auto& m = *l.manifest;
            lines.push_back(m.name + (m.version.empty() ? "" : " " + m.version) + (m.author.empty() ? "" : " by " + m.author) +
                            (l.archive.empty() ? "  (folder " + l.folder + ")" : "  (archive " + archive + ", read in place)"));
            if (!m.description.empty()) lines.push_back(m.description);
            if (!m.game_version.empty())
                lines.push_back("Made for " + m.game_version +
                                (game_version_matches(m.game_version) ? std::string()
                                                                      : " - NOT this game (" + std::string(game_version_text) + "): used anyway"));
            const auto here = std::string(current_platform());
            if (!m.platform.empty())
                lines.push_back("Packed for " + m.platform + (m.platform == here ? " (this platform)" : " (loads here too)"));
            if (const auto it = m.notes.find(here); it != m.notes.end()) lines.push_back("Notes (" + here + ", this platform): " + it->second);
            for (const auto& [platform, note] : m.notes)
                if (platform != here) lines.push_back("Notes (" + platform + "): " + note);
            lines.push_back("Priority " + std::to_string(m.priority) + "; " + (l.enabled ? "enabled" : "disabled") +
                            (l.chosen ? " by the settings (mod." + l.folder + "=" + (l.enabled ? "1" : "0") + ")"
                                      : std::string(" by its manifest (enabled=") + (m.enabled ? "1" : "0") + ")"));
            lines.push_back("Overrides: " + (m.overrides.empty() ? std::string("(undeclared: everything in the folder)")
                                                                 : join(m.overrides, ", ")));
            if (!m.audio_overrides.empty()) {
                std::vector<std::string> audio;
                for (const auto& a : m.audio_overrides) audio.push_back(a.target + "=" + a.file);
                lines.push_back("Audio overrides: " + join(audio, ", "));
            }
            const auto volume = state.volumes.find(upper(l.folder));
            if (volume != state.volumes.end() || m.audio_volume != 100 || !m.audio_overrides.empty())
                lines.push_back("Audio volume " +
                                std::to_string(volume != state.volumes.end() ? volume->second : m.audio_volume) + "% (" +
                                (volume != state.volumes.end() ? "settings mod_volume." + l.folder + "=" : std::string("manifest audio_volume=")) +
                                ")");
            if (l.enabled) {
                std::string used;
                if (const auto it = state.used.find(l.folder); it != state.used.end())
                    for (const auto& [kind, n] : it->second) used += (used.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
                lines.push_back("Supplies: " + (used.empty() ? std::string("nothing") : used));
                if (const auto n = place(l.folder))
                    lines.push_back("Load order: " + std::to_string(n) + " of " + std::to_string(state.load_order.size()) +
                                    " (the first wins each entry, then the disc)");
                std::size_t wins = 0, loses = 0, shown = 0, alone = 0;
                std::vector<std::string> contested;
                for (const auto& o : state.owners) {
                    std::string line;
                    if (o.winner == l.folder) {
                        if (o.shadowed.empty()) {
                            ++alone;
                            continue;
                        }
                        ++wins;
                        line = "  wins  " + o.kind + "/" + o.key + " over " + join(o.shadowed, ", ");
                    } else if (std::find(o.shadowed.begin(), o.shadowed.end(), l.folder) != o.shadowed.end()) {
                        ++loses;
                        line = "  loses " + o.kind + "/" + o.key + " to " + o.winner + " (used)";
                    } else {
                        continue;
                    }
                    if (shown < max_detail_entries) {
                        contested.push_back(std::move(line));
                        ++shown;
                    }
                }
                lines.push_back("Contested entries: wins " + std::to_string(wins) + ", loses " + std::to_string(loses) + "; " +
                                std::to_string(alone) + " supplied by this mod alone");
                lines.insert(lines.end(), contested.begin(), contested.end());
                if (wins + loses > shown) lines.push_back("  ... " + std::to_string(wins + loses - shown) + " more");
            }
        }
        lines.emplace_back();
    }
    if (!state.mods_on) lines.push_back("ALL MODS ARE OFF (mods=0): the game loads its disc only.");
    if (!state.settings_problem.empty()) lines.push_back("The settings file is malformed: " + state.settings_problem);
    if (!state.load_order.empty()) {
        std::string order;
        for (std::size_t i = 0; i < state.load_order.size(); ++i)
            order += (i ? ", " : "") + std::to_string(i + 1) + ". " + state.load_order[i];
        lines.push_back("Load order (the first wins, then the disc): " + order);
    }
    lines.push_back("Conflicts of the enabled mods:");
    for (const auto& line : state.conflicts.lines) lines.push_back("  " + line);
    bool header = false;
    for (const auto& m : state.messages) {
        if (m.find(": ignored ") != std::string::npos) continue;   // already a conflicts line
        if (!header) lines.push_back("Scan:");
        header = true;
        lines.push_back("  " + m);
    }
    lines.push_back("Changes apply at the next start of the game.");
    return lines;
}

}  // namespace omega::mods
