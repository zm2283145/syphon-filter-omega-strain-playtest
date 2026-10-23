#include "parts_table.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {

namespace {

// newlib's _ctype_ space and digit bits for 0..0x7f; the EE indexes the
// table with a signed char, so a byte >= 0x80 reads the data in front of it.
// Only is_digit throws: a byte is_space rejects always reaches is_digit next.
bool is_space(std::uint8_t c) {
    return c == ' ' || (c >= 9 && c <= 13);
}
bool is_digit(std::uint8_t c) {
    if (c >= 0x80) throw std::runtime_error("parts atoi: strtol's ctype lookup reads a byte >= 0x80");
    return c >= '0' && c <= '9';
}

}  // namespace

std::int32_t parts_atoi(std::span<const std::uint8_t> text) {
    std::size_t i = 0;
    // Every scan ends at the NUL, so nothing reads past it.
    auto next = [&]() -> std::uint8_t { return i < text.size() ? text[i++] : 0; };
    std::uint8_t c = next();
    while (is_space(c)) c = next();
    bool negative = false;
    if (c == '-') {
        negative = true;
        c = next();
    } else if (c == '+') {
        c = next();
    }
    // Base 10: a letter's digit value (10 and up) ends the number like any
    // other non-digit. The cutoff for a negative number is 2^63 / 10, cutlim 8.
    constexpr std::uint64_t base = 10;
    std::uint64_t cutoff = negative ? std::uint64_t{1} << 63 : (std::uint64_t{1} << 63) - 1;
    const std::uint64_t cutlim = cutoff % base;
    cutoff /= base;
    std::uint64_t acc = 0;
    int any = 0;
    for (; is_digit(c); c = next()) {
        const std::uint64_t digit = c - '0';
        if (any < 0 || acc > cutoff || (acc == cutoff && digit > cutlim)) {
            any = -1;  // overflowed: later digits are consumed, not accumulated
        } else {
            any = 1;
            acc = acc * base + digit;
        }
    }
    if (any < 0) acc = negative ? std::uint64_t{1} << 63 : (std::uint64_t{1} << 63) - 1;
    else if (negative) acc = 0 - acc;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(acc));
}

void parse_parts_prn(std::span<const std::uint8_t> bytes, std::vector<PartRow>& rows) {
    constexpr std::size_t line_buffer = 0x200;
    constexpr std::size_t name_buffer = 16;
    bool header = true;
    std::vector<std::pair<std::size_t, std::int32_t>> columns;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t end = pos;
        while (end < bytes.size() && bytes[end] != 0x0d) ++end;
        if (end - pos >= line_buffer) throw std::runtime_error("PARTS.PRN line of 0x200 bytes or more overruns the line buffer");
        auto line = bytes.subspan(pos, end - pos);
        for (std::size_t k = 0; k < line.size(); ++k) {
            if (line[k] == 0) {
                line = line.first(k);
                break;
            }
        }
        pos = end + 2;  // the CR and the byte after it, LF or not
        const std::size_t length = line.size();
        if (length == 0) {
            header = true;
            columns.clear();
            continue;
        }
        if (header) {
            header = false;
            for (std::size_t at = 0; at < length; ++at) {
                if (line[at] == ' ') continue;
                columns.emplace_back(at, parts_atoi(line.subspan(at)));
                while (at < length && line[at] != ' ') ++at;
            }
            continue;
        }
        for (const auto& [offset, value] : columns) {
            if (offset >= length || line[offset] == ' ') continue;
            std::size_t stop = offset;
            while (stop < length && line[stop] != ' ') ++stop;
            if (stop - offset >= name_buffer) throw std::runtime_error("PARTS.PRN name of 16 characters or more overruns the row");
            rows.push_back({std::string(line.begin() + static_cast<std::ptrdiff_t>(offset),
                                        line.begin() + static_cast<std::ptrdiff_t>(stop)),
                            value});
        }
    }
}

std::vector<PartRow> parse_parts_prn(std::span<const std::uint8_t> bytes) {
    std::vector<PartRow> rows;
    parse_parts_prn(bytes, rows);
    return rows;
}

}  // namespace omega
