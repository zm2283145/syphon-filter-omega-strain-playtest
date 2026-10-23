#include "appearance.hpp"
#include <algorithm>
#include <stdexcept>

namespace omega {
std::optional<std::string> AppearancePacking::slot(std::size_t index) const {
    if (index >= capacity / 16) throw std::runtime_error("Appearance slot index out of range");
    const auto start = index * 16;
    if (!std::all_of(initialized.begin() + start, initialized.begin() + start + 16,
                     [](bool value) { return value; })) return std::nullopt;
    std::string result;
    for (std::size_t i = start; i < start + 16; ++i) {
        if (!bytes[i]) return result;
        if (bytes[i] > 127) return std::nullopt;
        result.push_back(static_cast<char>(bytes[i]));
    }
    return std::nullopt;
}

AppearancePacking pack_appearance(std::span<const std::uint8_t> source) {
    if (source.size() > 1024 * 1024) throw std::runtime_error("Appearance input exceeds 1 MiB");
    AppearancePacking result;
    result.input_size = source.size();
    auto put = [&](std::size_t at, std::uint8_t value) {
        if (at >= result.capacity) throw std::runtime_error("Appearance packing exceeds destination");
        result.bytes[at] = value;
        result.initialized[at] = true;
    };
    std::size_t cursor = 0;
    for (auto byte : source) {
        if (byte >= 'a' && byte <= 'z') byte -= 'a' - 'A';
        // The original writes before testing the limit, even for discarded bytes.
        put(cursor++, byte);
        if (cursor >= result.capacity) throw std::runtime_error("Appearance reaches unsafe packing limit");
        if (byte == 0 || byte == ' ' || byte == '#' || byte == '$' || byte == '*' ||
            byte == '\t' || byte == '\n') --cursor;
        else if (byte == '\r') {
            put(cursor - 1, 0);
            while (cursor % 16) put(cursor++, 0);
        }
    }
    put(cursor, 0);
    while (cursor % 16) put(cursor++, 0);

    auto require_known = [&](std::size_t at, std::size_t count) {
        if (at > result.capacity || count > result.capacity - at)
            throw std::runtime_error("Appearance pair table exceeds destination");
        if (!std::all_of(result.initialized.begin() + at,
                         result.initialized.begin() + at + count, [](bool value) { return value; }))
            throw std::runtime_error("Appearance pair table depends on unknown destination bytes");
    };
    std::size_t terminal = 0x40;
    for (;; terminal += 0x20) {
        require_known(terminal, 8);
        if (std::all_of(result.bytes.begin() + terminal, result.bytes.begin() + terminal + 8,
                        [](std::uint8_t value) { return value == 0; })) break;
    }
    require_known(terminal, 16);
    if (terminal > result.capacity - 0x40)
        throw std::runtime_error("Appearance sentinel copy exceeds destination");
    for (std::size_t i = 0; i < 16; ++i) {
        put(terminal + 0x20 + i, result.bytes[terminal + i]);
        put(terminal + 0x30 + i, result.bytes[terminal + i]);
    }
    result.terminator_offset = terminal;
    return result;
}

void write_appearance(std::ostream& out, const AppearancePacking& appearance) {
    out << "{\"input_size\":" << appearance.input_size
        << ",\"terminator_offset\":" << appearance.terminator_offset
        << ",\"pair_count\":" << appearance.pair_count() << ",\"bytes\":[";
    for (std::size_t i = 0; i < appearance.capacity; ++i) {
        if (i) out << ',';
        if (appearance.initialized[i]) out << static_cast<unsigned>(appearance.bytes[i]);
        else out << "null";
    }
    out << "],\"slots\":[";
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < appearance.capacity / 16; ++i) {
        if (i) out << ',';
        const auto value = appearance.slot(i);
        if (!value) { out << "null"; continue; }
        out << '"';
        for (const unsigned char ch : *value) {
            if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
            else if (ch < 32 || ch == 127) out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
            else out << static_cast<char>(ch);
        }
        out << '"';
    }
    out << "]}";
}
}
