#include "appearance_parts.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace omega {
namespace {

using Key = std::array<std::uint8_t, 16>;

constexpr std::size_t appearance_bytes = 0x440;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Appearance parts 00405a60: " + what); }

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t o) {
    if (o + 4 > b.size()) fail("the MANTEX.HOG header is truncated");
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

const std::uint8_t* at(std::span<std::uint8_t> app, std::size_t o, std::size_t n) {
    if (o + n > app.size()) fail("a slot read past the 0x440-byte appearance block");
    return app.data() + o;
}

// 001298b8 strncmp(app + o, s, 5) == 0.
bool prefix_equal(std::span<std::uint8_t> app, std::size_t o, const char* s) {
    for (std::size_t k = 0; k < 5; ++k) {
        const std::uint8_t c = *at(app, o + k, 1);
        if (c != static_cast<std::uint8_t>(s[k])) return false;
        if (c == 0) return true;
    }
    return true;
}

// The slot scan of 00405a60 for one "PATCn".
std::optional<std::size_t> find_patch_slot(std::span<std::uint8_t> app, const char* name) {
    std::size_t p = 0x40, idx = 0;
    while (true) {
        std::uint64_t low = 0;
        std::memcpy(&low, at(app, p, 16), 8);  // lq + beqz: the low doubleword
        if (low == 0) break;
        if (prefix_equal(app, p + 1, name)) return idx;
        p += 0x20;
        ++idx;
    }
    if (prefix_equal(app, p + 0x11, name)) return idx;
    return std::nullopt;
}

bool contains(std::span<const Key> names, const Key& k) {  // pceqw / ppach == -1
    return std::find(names.begin(), names.end(), k) != names.end();
}

// 00406040 (PATC1, PATC2) and 00405f10 (PATC3): the slot's texture name
// +50 + 0x20 idx steps until it is in the names (32 tries).
void step_texture(std::span<std::uint8_t> app, std::size_t idx, std::span<const Key> names, bool two_digits) {
    const std::size_t o = 0x50 + 0x20 * idx;
    Key buf{};
    std::memcpy(buf.data(), at(app, o, 16), 16);
    if (contains(names, buf)) return;
    for (int tries = 0; tries < 0x20; ++tries) {
        if (two_digits) {  // 00406040
            if (buf[0xd] == '-') {
                if (buf[0xc] == '9') {
                    buf[0xc] = '1';
                    buf[0xd] = '0';
                } else {
                    ++buf[0xc];
                }
            } else if (buf[0xd] == '9') {
                ++buf[0xc];
                buf[0xd] = '0';
            } else {
                ++buf[0xd];
            }
        } else {  // 00405f10
            if (buf[0xc] == '9') {
                ++buf[0xb];
                buf[0xc] = '0';
            } else {
                ++buf[0xc];
            }
        }
        if (contains(names, buf)) {
            std::memcpy(app.data() + o, buf.data(), 16);
            return;
        }
    }
}

}  // namespace

std::vector<std::string> hog_member_names(std::span<const std::uint8_t> hog) {
    const auto count = le32(hog, 4), names = le32(hog, 12), data = le32(hog, 16);
    if (names > data || data > hog.size()) fail("the MANTEX.HOG names block is outside the file");
    std::vector<std::string> out;
    std::size_t p = names;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string s;
        while (true) {
            if (p >= data) fail("a MANTEX.HOG member name runs past the names block");
            const auto c = hog[p++];
            if (c == 0) break;
            s += static_cast<char>(c);
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::array<std::uint8_t, 16> appearance_name_key(std::string_view src) {
    Key k{};
    for (std::size_t t = 0; t < 15 && t < src.size(); ++t) {
        if (src[t] == '.' || src[t] == 0) break;
        k[t] = static_cast<std::uint8_t>(src[t]);
    }
    return k;
}

void patch_appearance_parts(std::span<std::uint8_t> app, std::span<const std::string> locked,
    std::span<const std::string> members) {
    if (app.size() != appearance_bytes) fail("the appearance block is not 0x440 bytes");
    std::vector<Key> names;
    names.reserve(members.size());
    for (const auto& m : members) names.push_back(appearance_name_key(m));
    // 00405e10
    for (const auto& row : locked) {
        if (row.find('\0') != std::string::npos) fail("a part row name holds a NUL");
        if (row.find('.') != std::string::npos) continue;  // 00129088 strchr
        const Key key = appearance_name_key(row);
        for (auto& n : names)
            if (n == key) {
                n = Key{};
                break;
            }
    }
    const auto patc1 = find_patch_slot(app, "PATC1");
    const auto patc2 = find_patch_slot(app, "PATC2");
    const auto patc3 = find_patch_slot(app, "PATC3");
    if (patc1) step_texture(app, *patc1, names, true);
    if (patc2) step_texture(app, *patc2, names, true);
    if (patc3) step_texture(app, *patc3, names, false);
}

}  // namespace omega
