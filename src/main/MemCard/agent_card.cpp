#include "agent_card.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace omega {
namespace {

constexpr std::size_t list_decode_max = 0x10;  // 00292e80's decode calls
constexpr std::uint32_t list_encode_max = 0x20;
constexpr std::size_t shared_name_offset = 0;      // sp + f0: the encoded name every renamed slot points at
constexpr std::size_t shared_prefix_offset = 0x20;  // sp + 110: the prefix copy
constexpr std::size_t shared_size = 0x40;          // to sp + 130 (the pass-1 decode scratch)

// 0040e370's tolower (0012b270): ctype bit 0 adds 0x20. Bytes >= 0x80 index
// the table with a negative offset (float constants); not modelled.
int card_fold(std::uint8_t c) {
    if (c >= 0x80) throw std::runtime_error("Agent card: a name byte >= 0x80 reaches the case fold");
    return c >= 'A' && c <= 'Z' ? c + 0x20 : c;
}

// 0040e370(pattern, entry): forward while both have characters and the
// pattern is not at '*' ('?' matches anything), then backward from both ends
// until either start or a '*'. No length test: "A" matches "AA".
bool card_name_match(std::string_view pattern, std::string_view entry) {
    std::size_t i = 0;
    for (; i < pattern.size() && i < entry.size() && pattern[i] != '*'; ++i) {
        if (pattern[i] == '?') continue;
        if (card_fold(static_cast<std::uint8_t>(pattern[i])) != card_fold(static_cast<std::uint8_t>(entry[i]))) return false;
    }
    auto p = static_cast<std::ptrdiff_t>(pattern.size()) - 1;
    auto e = static_cast<std::ptrdiff_t>(entry.size()) - 1;
    for (; p >= 0 && e >= 0; --p, --e) {
        const auto pc = static_cast<std::uint8_t>(pattern[static_cast<std::size_t>(p)]);
        if (pc == '*') break;
        if (pc == '?') continue;
        if (card_fold(pc) != card_fold(static_cast<std::uint8_t>(entry[static_cast<std::size_t>(e)]))) return false;
    }
    return true;
}

// Disk names keep 0-9 a-z # - and '.' (not last); every other byte is %XX
// (upper-case hex), so names that differ only in case stay apart on a
// case-insensitive file system and '\' / '|' never reach it.
bool disk_plain(std::uint8_t c, bool last) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '#' || c == '-' || (c == '.' && !last);
}

std::string to_disk_name(std::string_view name) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<std::uint8_t>(name[i]);
        if (disk_plain(c, i + 1 == name.size())) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::optional<std::string> from_disk_name(std::string_view disk) {
    std::string out;
    for (std::size_t i = 0; i < disk.size(); ++i) {
        if (disk[i] != '%') {
            out.push_back(disk[i]);
            continue;
        }
        if (i + 2 >= disk.size()) return std::nullopt;
        int v = 0;
        for (int k = 1; k <= 2; ++k) {
            const char h = disk[i + static_cast<std::size_t>(k)];
            if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
            else if (h >= 'A' && h <= 'F') v = v * 16 + (h - 'A' + 10);
            else return std::nullopt;
        }
        out.push_back(static_cast<char>(v));
        i += 2;
    }
    if (out.empty() || to_disk_name(out) != disk) return std::nullopt;  // only the canonical spelling
    return out;
}

std::size_t name_length(std::span<const std::uint8_t> name, const char* what) {
    const auto end = std::find(name.begin(), name.end(), std::uint8_t{0});
    if (end == name.end()) throw std::runtime_error(std::string(what) + ": the name has no terminating NUL");
    return static_cast<std::size_t>(end - name.begin());
}

}  // namespace

bool decode_agent_file_name(std::optional<std::span<const std::uint8_t>> name,
    std::optional<std::span<std::uint8_t>> out, std::int32_t* slot, std::int32_t max, bool font_ready,
    std::int32_t map_count) {
    if (!font_ready || map_count == 0) return false;
    if (!name || !out || slot == nullptr) return false;
    const auto length = name_length(*name, "Agent file name decode");
    if (length < 2) return false;
    const auto pairs = static_cast<std::int32_t>((length - 1) / 2);
    if (!(pairs < max)) return false;
    if ((length & 1) == 0) return false;
    const auto put = [&](std::size_t at, std::uint8_t v) {
        if (at >= out->size()) throw std::runtime_error("Agent file name decode: write past the output");
        (*out)[at] = v;
    };
    std::size_t p = 0;
    for (std::int32_t k = 0; k < pairs; ++k, p += 2) {
        const std::uint32_t hi = (*name)[p];
        const std::uint32_t lo = (*name)[p + 1];
        if (hi < '0' || hi >= '5') return false;
        if (lo < 0x40) return false;
        const std::uint32_t low = (lo - 0x40) & 0xff;
        if (low >= 0x3e) return false;
        const std::uint32_t index = (hi - '0') * 0x3e + low;
        if (!(index < static_cast<std::uint32_t>(map_count))) return false;
        put(static_cast<std::size_t>(k), static_cast<std::uint8_t>(index + 0x20));
    }
    put(static_cast<std::size_t>(pairs), 0);
    const std::int32_t digit = static_cast<std::int32_t>((*name)[p]) - '0';
    if (digit < 0 || digit >= 10) return false;
    *slot = digit;
    return true;
}

std::optional<AgentFileName> decode_agent_file_name(std::string_view file_name, std::int32_t max, bool font_ready,
    std::int32_t map_count) {
    if (file_name.find('\0') != std::string_view::npos)
        throw std::runtime_error("Agent file name decode: a NUL inside the name");
    std::vector<std::uint8_t> name(file_name.begin(), file_name.end());
    name.push_back(0);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(std::clamp(max, 1, 0x1000)));
    std::int32_t slot = 0;
    if (!decode_agent_file_name(std::span<const std::uint8_t>(name), std::span<std::uint8_t>(out), &slot, max, font_ready,
            map_count))
        return std::nullopt;
    return AgentFileName{std::string(reinterpret_cast<const char*>(out.data())), slot};
}

AgentSlotListResult build_agent_slot_list(std::span<const std::string> directory, AgentSlotListScreen& screen,
    bool font_ready, std::int32_t map_count, const AgentCardRename& rename) {
    for (const auto& d : directory) {
        if (d.size() >= agent_card_name_limit) throw std::runtime_error("Agent slot list: a card name of 0x20 bytes or more");
        if (d.find('\0') != std::string::npos) throw std::runtime_error("Agent slot list: a NUL inside a card name");
    }
    constexpr std::int32_t none = -1, shared = -2;
    std::array<std::int32_t, agent_card_slots> slots;
    slots.fill(none);
    // Pass 1. The digit test (ctype bit 2) only gates names the decoder
    // rejects anyway (hi must be '0'..'4').
    for (std::size_t i = 0; i < directory.size(); ++i) {
        const auto& d = directory[i];
        if (d.find('.') != std::string::npos || d.empty() || d[0] < '0' || d[0] > '9') continue;
        const auto decoded = decode_agent_file_name(d, static_cast<std::int32_t>(list_decode_max), font_ready, map_count);
        if (!decoded) continue;
        if (decoded->slot >= static_cast<std::int32_t>(agent_card_slots))
            throw std::runtime_error("Agent slot list: a decoded slot past the 8-entry vector (heap overrun)");
        slots[static_cast<std::size_t>(decoded->slot)] = static_cast<std::int32_t>(i);
    }
    // Pass 2: sp + f0 .. sp + 130, -1 = never written (stack garbage).
    std::array<std::int32_t, shared_size> stack;
    stack.fill(-1);
    AgentSlotListResult result;
    for (const auto& d : directory) {
        const auto dot = d.find('.');
        if (dot == std::string::npos) continue;
        const std::string_view tail = std::string_view(d).substr(dot);
        bool agt = true;
        for (std::size_t k = 0; k < 4 && agt; ++k) {
            const auto want = static_cast<std::uint8_t>(".agt"[k]);
            if (k >= tail.size()) {
                agt = false;
                break;
            }
            agt = card_fold(static_cast<std::uint8_t>(tail[k])) == want;
        }
        if (!agt || tail.size() < 5) continue;
        std::size_t free = 0;
        while (free < agent_card_slots && slots[free] != none) ++free;
        if (free == agent_card_slots) continue;
        result.update_popup = true;
        for (std::size_t k = 0; k < dot; ++k) stack[shared_prefix_offset + k] = static_cast<std::uint8_t>(d[k]);
        stack[shared_prefix_offset + dot] = 0;
        // 003fd550 into sp + f0, writing pair by pair.
        if (!font_ready || map_count == 0) continue;
        if (!(static_cast<std::uint32_t>(dot) * 2 + 1 < list_encode_max)) continue;
        bool ok = true;
        std::size_t o = shared_name_offset;
        for (std::size_t k = 0; k < dot && ok; ++k) {
            const std::int32_t c = static_cast<std::uint8_t>(d[k]);
            const std::int32_t index = c - 0x20;
            if (c < 0x20 || !(index < map_count)) {
                ok = false;
                break;
            }
            stack[o++] = '0' + index / 0x3e;
            stack[o++] = '@' + index % 0x3e;
        }
        if (!ok) continue;
        stack[o++] = '0' + static_cast<std::int32_t>(free);
        stack[o] = 0;
        std::string encoded;
        for (std::size_t k = shared_name_offset; stack[k] != 0; ++k) encoded.push_back(static_cast<char>(stack[k]));
        if (rename) rename(d, encoded);
        result.renames.emplace_back(d, encoded);
        slots[free] = shared;
    }
    // Buttons "0".."7".
    bool any_filled = false, focus_lost = false;
    for (std::size_t i = 0; i < agent_card_slots; ++i) {
        auto& button = screen.buttons[i];
        if (!button.found || !button.is_button) continue;
        bool filled = false;
        if (slots[i] != none) {
            std::string name;
            if (slots[i] == shared) {
                for (std::size_t k = shared_name_offset;; ++k) {
                    if (k >= shared_size || stack[k] < 0)
                        throw std::runtime_error("Agent slot list: a renamed slot's name runs into unwritten stack");
                    if (stack[k] == 0) break;
                    name.push_back(static_cast<char>(stack[k]));
                }
            } else {
                name = directory[static_cast<std::size_t>(slots[i])];
            }
            if (const auto decoded =
                    decode_agent_file_name(name, static_cast<std::int32_t>(list_decode_max), font_ready, map_count)) {
                result.text_kind[i] = AgentSlotText::name;
                result.text[i] = decoded->name;
                button.flags |= 4;
                any_filled = filled = true;
            }
        }
        if (filled) continue;
        if (!screen.empty_selectable && screen.focus == static_cast<std::int32_t>(i)) focus_lost = true;
        result.text_kind[i] = AgentSlotText::empty;
        if (screen.empty_selectable) button.flags |= 4;
        else button.flags &= static_cast<std::uint16_t>(~4u);
    }
    result.refocus = focus_lost || (any_filled && screen.focus == agent_slot_focus_screen);
    return result;
}

FolderAgentCard::FolderAgentCard(std::filesystem::path root) : directory_(std::move(root) / agent_card_directory) {}

bool FolderAgentCard::save_directory_present() const {
    if (!usable()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(directory_, ec);
}

bool FolderAgentCard::create_save_directory() {
    if (!usable() || (sim_ && sim_->mkdir_fail)) return false;
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    return save_directory_present();
}

std::uint32_t FolderAgentCard::card_id() const {
    if (!usable()) return 0xffffffffu;  // 0040efa0: +5a8 = -1
    const std::uint32_t wrong = sim_ && sim_->wrong_id ? 1u : 0u;
    for (const auto& name : list()) {
        if (name.size() < 2 || name[0] != '#') continue;
        if (!std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        return static_cast<std::uint32_t>(std::stoull(name.substr(1))) + wrong;
    }
    return wrong;
}

std::uint64_t FolderAgentCard::free_bytes() const {
    if (!usable()) return 0;
    if (sim_ && sim_->free_kb) return static_cast<std::uint64_t>(*sim_->free_kb) * 1024;
    std::error_code ec;
    auto at = directory_;
    while (!at.empty() && !std::filesystem::exists(at, ec)) at = at.parent_path();
    const auto info = std::filesystem::space(at.empty() ? std::filesystem::current_path() : at, ec);
    return ec ? 0 : info.available;
}

std::vector<std::string> FolderAgentCard::list() const {
    std::vector<std::string> names;
    if (!save_directory_present()) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file()) continue;
        if (auto name = from_disk_name(entry.path().filename().string())) names.push_back(std::move(*name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<std::filesystem::path> FolderAgentCard::find(std::string_view name) const {
    if (name.empty()) throw std::runtime_error("Agent card: an empty name matches the card's first entry");
    for (const auto& entry : list())
        if (card_name_match(name, entry)) return directory_ / to_disk_name(entry);
    return std::nullopt;
}

bool FolderAgentCard::read(std::string_view name, std::span<std::uint8_t> buffer) const {
    if (sim_ && sim_->read_fail) return false;
    const auto path = find(name);
    if (!path) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(*path, ec);
    if (ec || size > buffer.size()) return false;
    std::ifstream in(*path, std::ios::binary);
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uint64_t>(in.gcount()) != size) return false;
    if (sim_ && sim_->corrupt && size != 0) buffer[static_cast<std::size_t>(size / 2)] ^= 0xff;
    return true;
}

std::uint32_t FolderAgentCard::size(std::string_view name) const {  // 0040dbb0 (the entry table's +14)
    const auto path = find(name);
    if (!path) return 0;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(*path, ec);
    if (ec || bytes > 0xffffffffu) return 0;
    return static_cast<std::uint32_t>(bytes);
}

bool FolderAgentCard::write(std::string_view name, std::span<const std::uint8_t> data) {
    if (!save_directory_present() || (sim_ && sim_->write_fail)) return false;
    auto path = find(name);
    if (!path) {
        if (name.size() >= agent_card_name_limit) throw std::runtime_error("Agent card: a name of 0x20 bytes or more");
        if (list().size() >= agent_card_max_entries) return false;
        path = directory_ / to_disk_name(name);
    }
    if (before_change_) before_change_();
    auto temp = *path;
    temp += ".tmp~";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, *path, ec);
    if (ec) std::filesystem::remove(temp, ec);
    return !ec;
}

bool FolderAgentCard::remove(std::string_view name) {
    if (sim_ && sim_->write_fail) return false;
    const auto path = find(name);
    if (!path) return false;
    if (before_change_) before_change_();
    std::error_code ec;
    return std::filesystem::remove(*path, ec) && !ec;
}

bool FolderAgentCard::rename(std::string_view from, std::string_view to) {
    if (sim_ && sim_->write_fail) return false;
    const auto path = find(from);
    if (!path || to.empty() || to.size() >= agent_card_name_limit) return false;
    if (find(to)) return false;
    if (before_change_) before_change_();
    std::error_code ec;
    std::filesystem::rename(*path, directory_ / to_disk_name(to), ec);
    return !ec;
}

AgentCardState FolderAgentCard::state(bool font_ready, std::int32_t map_count) const {
    AgentCardState s;
    s.busy = false;
    s.card_present = !sim_ || sim_->present;
    s.card_formatted = usable();
    s.font_ready = font_ready;
    s.glyph_count = map_count;
    s.card_id = card_id();
    return s;
}

bool FolderAgentCard::format() {  // 0040db10
    if (!sim_) throw std::runtime_error("Agent card: 0040db10 format without a simulated card (the folder card is never unformatted)");
    if (!sim_->present || sim_->format_fail) return false;
    std::error_code ec;
    if (std::filesystem::exists(directory_, ec)) {
        for (int n = 1;; ++n) {
            auto aside = directory_;
            aside += ".formatted-" + std::to_string(n);
            if (std::filesystem::exists(aside, ec)) continue;
            std::filesystem::rename(directory_, aside, ec);
            if (ec) return false;
            break;
        }
    }
    sim_->formatted = true;
    return true;
}

std::optional<AgentCardSimulation> parse_agent_card_simulation(std::string_view spec) {
    if (spec.empty() || spec == "off") return std::nullopt;
    AgentCardSimulation sim;
    std::size_t at = 0;
    while (at <= spec.size()) {
        const auto end = spec.find_first_of(",+", at);
        const std::string_view token = spec.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? spec.size() + 1 : end + 1;
        if (token == "normal") continue;
        if (token == "nocard") sim.present = false;
        else if (token == "unformatted") sim.formatted = false;
        else if (token == "full") sim.free_kb = 0;
        else if (token.starts_with("free=")) {
            const std::string digits(token.substr(5));
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos || digits.size() > 9)
                throw std::runtime_error("Card simulation: bad free=<KB> in '" + std::string(spec) + "'");
            sim.free_kb = static_cast<std::uint32_t>(std::stoul(digits));
        } else if (token == "wrongcard") sim.wrong_id = true;
        else if (token == "readfail") sim.read_fail = true;
        else if (token == "writefail") sim.write_fail = true;
        else if (token == "mkdirfail") sim.mkdir_fail = true;
        else if (token == "formatfail") sim.format_fail = true;
        else if (token == "corrupt") sim.corrupt = true;
        else
            throw std::runtime_error("Card simulation: unknown '" + std::string(token) + "' in '" + std::string(spec) +
                                     "' (normal, nocard, unformatted, full, free=<KB>, wrongcard, readfail, writefail, "
                                     "mkdirfail, formatfail, corrupt, off)");
    }
    return sim;
}

std::string describe_agent_card_simulation(const std::optional<AgentCardSimulation>& sim) {
    if (!sim) return "off";
    std::string s;
    const auto add = [&](const std::string& t) { s += (s.empty() ? "" : ",") + t; };
    if (!sim->present) add("nocard");
    if (!sim->formatted) add("unformatted");
    if (sim->free_kb) add("free=" + std::to_string(*sim->free_kb));
    if (sim->wrong_id) add("wrongcard");
    if (sim->read_fail) add("readfail");
    if (sim->write_fail) add("writefail");
    if (sim->mkdir_fail) add("mkdirfail");
    if (sim->format_fail) add("formatfail");
    if (sim->corrupt) add("corrupt");
    return s.empty() ? "normal" : s;
}

AgentSaveWriter FolderAgentCard::writer() {
    return [this](std::string_view name, std::span<const std::uint8_t> file) { return write(name, file); };
}

AgentLoadReader FolderAgentCard::reader() const {
    return [this](std::string_view name, AgentSaveBuffer& buffer) { return read(name, buffer); };
}

AgentCardRename FolderAgentCard::renamer() {
    return [this](std::string_view from, std::string_view to) { rename(from, to); };
}

}  // namespace omega
