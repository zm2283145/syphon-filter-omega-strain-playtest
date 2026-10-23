#include "man_sort.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Man sort: " + what); }

constexpr std::size_t record_size = 0x30, pair_slots = 0x20;
constexpr std::int32_t unmatched = 0xfa00;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// sscanf 00128c68(line, "%s %d %f %f %f %f %f %f %f", ...) into the raw record slot, in conversion order.
int scan_line(const std::string& line, std::array<std::uint8_t, record_size>& slot) {
    const char* p = line.c_str();
    while (is_space(*p)) ++p;
    if (!*p) return -1;
    const char* end = p;
    while (*end && !is_space(*end)) ++end;
    const auto length = static_cast<std::size_t>(end - p);
    if (length + 1 > record_size) fail("00403490: a %s token of " + std::to_string(length) + " characters writes past its record");
    std::memcpy(slot.data(), p, length);
    slot[length] = 0;
    p = end;
    int fields = 1;
    char* next = nullptr;
    errno = 0;
    const long value = std::strtol(p, &next, 10);
    if (next == p) return fields;
    const auto word = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
    std::memcpy(slot.data() + 0x10, &word, 4);
    p = next;
    ++fields;
    for (std::size_t k = 0; k < 7; ++k) {
        // newlib's %f takes [sign] digits [. digits] [e [sign] digits] (no inf / nan / hex), converts the
        // text with strtod and stores it as a float.
        while (is_space(*p)) ++p;
        const char* q = p;
        if (*q == '+' || *q == '-') ++q;
        std::size_t digits = 0;
        while (*q >= '0' && *q <= '9') { ++q; ++digits; }
        if (*q == '.') { ++q; while (*q >= '0' && *q <= '9') { ++q; ++digits; } }
        if (!digits) return fields;
        if (*q == 'e' || *q == 'E') {
            const char* e = q + 1;
            if (*e == '+' || *e == '-') ++e;
            if (*e >= '0' && *e <= '9') { q = e; while (*q >= '0' && *q <= '9') ++q; }
        }
        const std::string number(p, q);
        // The interpreter goldens (and a 300-string probe of 00128c68) give strtod's double truncated
        // toward zero to a float, not rounded to nearest.
        const double d = std::strtod(number.c_str(), nullptr);
        auto f = static_cast<float>(d);
        if (std::fabs(static_cast<double>(f)) > std::fabs(d)) f = std::nextafter(f, 0.0f);
        std::memcpy(slot.data() + 0x14 + 4 * k, &f, 4);
        p = q;
        ++fields;
    }
    return fields;
}

ManSortRecord decode(const std::array<std::uint8_t, record_size>& slot) {
    ManSortRecord r;
    std::memcpy(r.key.data(), slot.data(), 16);
    std::memcpy(&r.thermal_class, slot.data() + 0x10, 4);
    std::memcpy(&r.fov, slot.data() + 0x14, 4);
    std::memcpy(r.location.data(), slot.data() + 0x18, 12);
    std::memcpy(r.rotation.data(), slot.data() + 0x24, 12);
    return r;
}

bool nonzero(const ManSortKey& k) {
    for (const auto b : k) if (b) return true;
    return false;
}
// Masked equality over [first, last] (the static masks of the part cache 0055dda0 and 00390570's own).
bool equal_bytes(const ManSortKey& a, const ManSortKey& b, std::size_t first, std::size_t last) {
    for (std::size_t i = first; i <= last; ++i) if (a[i] != b[i]) return false;
    return true;
}
}

std::vector<ManSortRecord> parse_man_sort(std::span<const std::uint8_t> text) {
    std::vector<ManSortRecord> out;
    std::array<std::uint8_t, record_size> slot{};
    std::string line;
    std::size_t s = 0;
    const std::size_t n = text.size();
    while (s < n) {
        auto c = static_cast<char>(text[s++]);
        line.push_back(c);
        if (c == 0 || c == '#' || c == '$' || c == '*' || c == '\t' || c == '\n') line.pop_back();
        if (c == '/') {
            do {
                if (s >= n) fail("00403490 reads past the file after '/'");
                c = static_cast<char>(text[s++]);
            } while (s < n && c != '\r');
        }
        if (c != '\r') continue;
        line.pop_back();                 // the '\r' (or the '/') becomes the terminator
        ++s;                             // the byte after '\r'
        const int fields = scan_line(line, slot);
        if (fields == 9) {
            out.push_back(decode(slot));
            slot = {};
        } else if (fields != -1) {
            std::fill(slot.begin(), slot.begin() + 16, std::uint8_t{0});
        }
        line.clear();
    }
    return out;
}

std::uint8_t ManSortOrder::class_of(std::size_t index) const {
    for (std::size_t pos = 0; pos < order.size(); ++pos) if (order[pos] == index) return thermal_class[pos];
    fail("no position holds pair " + std::to_string(index));
}

std::size_t ManSortOrder::position_of(std::size_t index) const {
    for (std::size_t pos = 0; pos < order.size(); ++pos) if (order[pos] == index) return pos;
    fail("no position holds pair " + std::to_string(index));
}

ManSortOrder sort_man_parts(std::span<const ManSortPair> pairs, std::span<const ManSortRecord> records) {
    if (pairs.size() > pair_slots) fail("more than 0x20 pairs");
    std::array<ManSortPair, pair_slots> table{};
    std::copy(pairs.begin(), pairs.end(), table.begin());
    ManSortOrder out;
    out.order.resize(pair_slots);
    for (std::size_t i = 0; i < pair_slots; ++i) out.order[i] = i;
    std::size_t count = 0;
    while (count < pair_slots && nonzero(table[count].mesh)) ++count;
    out.part_count = static_cast<std::uint32_t>(count);
    std::array<std::int32_t, pair_slots> chosen{}, score{};
    for (std::size_t i = 0; i < count; ++i) { chosen[i] = unmatched; score[i] = 0; }
    for (std::size_t k = 0; k < records.size(); ++k) {
        const auto& key = records[k].key;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& part = table[i];
            if (!equal_bytes(part.texture, key, 1, 4)) continue;
            std::int32_t s = 1;
            if (equal_bytes(part.mesh, key, 6, 8)) s += 2;
            if (equal_bytes(part.mesh, key, 10, 13)) s += 1;
            if (equal_bytes(part.mesh, key, 0, 0)) s += 8;
            if (equal_bytes(part.mesh, key, 5, 5)) s += 4;
            if (score[i] < s) { chosen[i] = static_cast<std::int32_t>(k); score[i] = s; }
        }
    }
    std::size_t pos = 0;
    for (;;) {
        std::int32_t best = unmatched;
        std::size_t at = 0;
        bool found = false;
        for (std::size_t a = pos; a < count; ++a) {
            if (chosen[a] < best && chosen[a] != unmatched) { best = chosen[a]; at = a; found = true; }
        }
        if (!found) break;
        chosen[at] = chosen[pos];
        std::swap(table[pos], table[at]);
        std::swap(out.order[pos], out.order[at]);
        out.record[pos] = static_cast<std::uint8_t>(best);
        out.thermal_class[pos] = static_cast<std::uint8_t>(records[static_cast<std::size_t>(best)].thermal_class);
        ++pos;
    }
    for (; pos < pair_slots; ++pos) {
        if (nonzero(table[pos].mesh)) { out.record[pos] = 0; out.thermal_class[pos] = 1; continue; }
        if (records.empty()) fail("00390570 reads record -1 (no MANSORT2 records) for an empty pair");
        out.record[pos] = static_cast<std::uint8_t>(records.size() - 1);
        out.thermal_class[pos] = static_cast<std::uint8_t>(records.back().thermal_class);
    }
    return out;
}

std::vector<ManSortPair> man_sort_pairs(const AppearancePacking& state) {
    std::vector<ManSortPair> out;
    for (std::size_t i = 0; i < pair_slots; ++i) {
        const std::size_t mesh = 0x40 + 0x20 * i, texture = mesh + 0x10;
        if (texture + 16 > AppearancePacking::capacity) break;
        bool known_nonzero = false, all_known = true;
        for (std::size_t b = 0; b < 16; ++b) {
            if (!state.initialized[mesh + b]) all_known = false;
            else if (state.bytes[mesh + b]) known_nonzero = true;
        }
        if (!known_nonzero && !all_known) fail("00390570: pair " + std::to_string(i) + "'s mesh key zero test reads unwritten bytes");
        ManSortPair pair;
        std::copy_n(state.bytes.begin() + static_cast<std::ptrdiff_t>(mesh), 16, pair.mesh.begin());
        std::copy_n(state.bytes.begin() + static_cast<std::ptrdiff_t>(texture), 16, pair.texture.begin());
        out.push_back(pair);
        if (!known_nonzero) break;
        for (std::size_t b = 0; b < 14; ++b)
            if (!state.initialized[mesh + b]) fail("00390570: pair " + std::to_string(i) + " mesh byte " + std::to_string(b) + " unwritten");
        for (std::size_t b = 1; b < 5; ++b)
            if (!state.initialized[texture + b]) fail("00390570: pair " + std::to_string(i) + " texture byte " + std::to_string(b) + " unwritten");
    }
    return out;
}

ManSortKey man_sort_key(std::string_view name) {
    if (name.size() > 15) fail("key longer than 15 characters: " + std::string(name));
    ManSortKey k{};
    std::copy(name.begin(), name.end(), k.begin());
    return k;
}
}
