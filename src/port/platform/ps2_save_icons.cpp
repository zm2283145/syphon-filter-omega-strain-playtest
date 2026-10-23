#include "ps2_save_icons.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

std::string c_string(const std::vector<std::uint8_t>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    if (end == bytes.end()) throw std::runtime_error("PS2 save icons: an unterminated string in the executable");
    return {bytes.begin(), end};
}

void put_string(std::vector<std::uint8_t>& out, std::size_t offset, const std::string& s) {
    if (s.empty() || s.size() + 1 > 0x40) throw std::runtime_error("PS2 save icons: bad icon file name");
    std::memcpy(out.data() + offset, s.data(), s.size());
    out[offset + s.size()] = 0;  // strcpy's terminator
}

}  // namespace

std::vector<std::uint8_t> ps2_title_sjis(const std::string& title, std::span<const std::uint8_t> table, std::size_t max) {
    if (table.size() < Ps2SaveIconAddresses::sjis_entries * 4) throw std::runtime_error("PS2 save icons: short Shift-JIS table");
    const std::size_t n = std::min(title.size(), max);
    std::vector<std::uint8_t> out;
    out.reserve(n * 2 + 2);
    for (std::size_t i = 0; i < n; ++i) {
        const int c = static_cast<signed char>(title[i]);  // lb
        std::uint16_t code = 0;
        if (c >= 0x61 && c < 0x7b) code = static_cast<std::uint16_t>(c + 0x8220);
        else if (c >= 0x41 && c < 0x5b) code = static_cast<std::uint16_t>(c + 0x821f);
        else if (c >= 0x31 && c < 0x3a) code = static_cast<std::uint16_t>(c + 0x821f);
        else
            for (std::size_t e = 0; e < Ps2SaveIconAddresses::sjis_entries; ++e)
                if (static_cast<signed char>(table[e * 4 + 2]) == c) {
                    code = static_cast<std::uint16_t>(table[e * 4] | table[e * 4 + 1] << 8);
                    break;
                }
        // sh ((code & 0xff) << 8) + (code >> 8): the high byte first in memory.
        out.push_back(static_cast<std::uint8_t>(code >> 8));
        out.push_back(static_cast<std::uint8_t>(code & 0xff));
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::vector<std::uint8_t> ps2_icon_sys(const Ps2SaveIconSources& s) {
    if (s.icon_sys.size() != Ps2SaveIconAddresses::icon_sys_size || s.magic.size() != 4)
        throw std::runtime_error("PS2 save icons: bad icon.sys source sizes");
    auto out = s.icon_sys;
    put_string(out, 0x144, s.icon_name);
    put_string(out, 0x104, s.icon_name);
    put_string(out, 0x184, s.icon_name);
    // strncpy(+0, "PS2D", 4): copies up to the first NUL, pads with zeros.
    for (std::size_t i = 0, end = 4; i < 4; ++i) {
        if (s.magic[i] == 0 && end == 4) end = i;
        out[i] = i < end ? s.magic[i] : 0;
    }
    const auto title = ps2_title_sjis(s.title, s.sjis_table);
    std::memcpy(out.data() + 0xc0, title.data(), title.size());  // at most 0x42 bytes, inside the 0x44-byte field
    if (std::memcmp(out.data(), "PS2D", 4) != 0) throw std::runtime_error("PS2 save icons: icon.sys has no PS2D magic");
    return out;
}

Ps2SaveIconSources ps2_save_icon_sources(const std::function<std::vector<std::uint8_t>(std::uint32_t, std::uint32_t)>& read) {
    using A = Ps2SaveIconAddresses;
    const auto word = [&](std::uint32_t a) {
        const auto b = read(a, 4);
        return std::uint32_t(b.at(0)) | std::uint32_t(b.at(1)) << 8 | std::uint32_t(b.at(2)) << 16 | std::uint32_t(b.at(3)) << 24;
    };
    Ps2SaveIconSources s;
    s.icon_sys = read(A::icon_sys, A::icon_sys_size);
    s.magic = read(A::magic, 4);
    s.title = c_string(read(A::title, 0x40));
    s.sjis_table = read(A::sjis_table, A::sjis_entries * 4);
    s.icon_name = c_string(read(word(A::icon_name_pointer), 0x20));
    s.icon_folder = c_string(read(A::icon_folder, 0x20));
    return s;
}

std::vector<Ps2CardFile> ps2_save_icon_files(const Ps2SaveIconSources& s,
                                             const std::function<std::vector<std::uint8_t>(const std::string&)>& read_disc) {
    auto icon = read_disc(s.icon_folder + s.icon_name);
    if (icon.size() < 20 || icon[0] != 0 || icon[1] != 0 || icon[2] != 1 || icon[3] != 0)
        throw std::runtime_error("PS2 save icons: " + s.icon_folder + s.icon_name + " is not a PS2 icon");
    return {{s.icon_name, std::move(icon)}, {"icon.sys", ps2_icon_sys(s)}};
}

}  // namespace omega
