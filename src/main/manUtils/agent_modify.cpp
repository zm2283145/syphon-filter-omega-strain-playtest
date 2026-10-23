#include "agent_modify.hpp"

#include "agent_camera.hpp"
#include "agent_create.hpp"
#include "appearance_parts.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {

using Key = AgentPartKey;

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char a[16];
    std::snprintf(a, sizeof a, "%08x", address);
    throw std::runtime_error(std::string(a) + ": " + what);
}

Key bytes_mask(std::initializer_list<int> on) {
    Key k{};
    for (int i : on) k[static_cast<std::size_t>(i)] = 0xff;
    return k;
}

const std::array<Key, 10> masks{
    bytes_mask({9}),                                  // +20
    bytes_mask({0}),                                  // +30
    bytes_mask({5}),                                  // +40
    bytes_mask({1, 2, 3, 4}),                         // +50
    bytes_mask({6, 7, 8}),                            // +60
    bytes_mask({10, 11, 12, 13}),                     // +70
    bytes_mask({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15}),  // +80
    bytes_mask({1, 2, 3, 4, 6, 7, 8}),                // +90
    bytes_mask({1, 2, 3, 4, 5}),                      // +a0
    bytes_mask({1, 2, 3}),                            // +b0
};
const Key& M(AgentMask m) { return masks[static_cast<std::size_t>(m)]; }

bool eq_under(const Key& a, const Key& b, const Key& m) {
    for (std::size_t i = 0; i < 16; ++i)
        if ((a[i] & m[i]) != (b[i] & m[i])) return false;
    return true;
}
Key mask_xor(const Key& a, const Key& b) {
    Key r{};
    for (std::size_t i = 0; i < 16; ++i) r[i] = a[i] ^ b[i];
    return r;
}
// (a & ~m) | (b & m)
Key merge(const Key& a, const Key& b, const Key& m) {
    Key r{};
    for (std::size_t i = 0; i < 16; ++i) r[i] = static_cast<std::uint8_t>((a[i] & ~m[i]) | (b[i] & m[i]));
    return r;
}
bool is_set_all(const Key& k) { return k[6] == 'A' && k[7] == 'L' && k[8] == 'L'; }
// The cache's +0 / +10 keys (boot image): "1belt_BLA_BLAN_" and "1belt_BLA_NONE_".
const Key p0 = agent_part_key("1belt_BLA_BLAN_");
const Key p10 = agent_part_key("1belt_BLA_NONE_");
bool tone_ok(const Key& t, std::int8_t tone) {
    return tone == 0 || t[9] == '_' || t[9] == static_cast<std::uint8_t>(tone);
}
// Byte 0 excluded for a sex: 1 -> '2', 0 -> '1'. `other` is what the function uses for any other sex
// (nullopt: the original compares against an uninitialised register).
std::uint8_t excluded(std::int32_t sex, std::optional<std::uint8_t> other, std::uint32_t address) {
    if (sex == 1) return '2';
    if (sex == 0) return '1';
    if (!other) fail(address, "sex " + std::to_string(sex) + " leaves the excluded byte unset");
    return *other;
}

Key get(const AgentPacking& p, std::size_t offset) {
    if (offset + 16 > p.size()) fail(0x406dc0, "pair past the 0x440-byte block");
    Key k;
    std::memcpy(k.data(), p.data() + offset, 16);
    return k;
}
void put(AgentPacking& p, std::size_t offset, const Key& k) {
    if (offset + 16 > p.size()) fail(0x406dc0, "pair past the 0x440-byte block");
    std::memcpy(p.data() + offset, k.data(), 16);
}
std::size_t mesh_at(std::int32_t pair) {
    if (pair < 0) fail(0x341330, "negative pair");
    return 0x40 + 0x20 * static_cast<std::size_t>(pair);
}
std::size_t tex_at(std::int32_t pair) { return mesh_at(pair) + 0x10; }

// Scans `n` entries from `start + dir` while the index is below n (unsigned), then from 0 (dir != -1)
// or n - 1 through the whole range: the shape of 004002b0 / 00400620 / 00401970 / 00401570.
template <typename F>
std::int32_t wrap_scan(std::size_t n, std::int32_t start, std::int32_t dir, F&& match) {
    for (std::int64_t i = static_cast<std::int64_t>(start) + dir;
         static_cast<std::uint32_t>(i) < n; i += dir)
        if (match(static_cast<std::size_t>(static_cast<std::uint32_t>(i)))) return static_cast<std::int32_t>(i);
    for (std::int64_t i = dir != -1 ? 0 : static_cast<std::int64_t>(n) - 1;
         static_cast<std::uint32_t>(i) < n; i += dir)
        if (match(static_cast<std::size_t>(i))) return static_cast<std::int32_t>(i);
    return -1;
}
bool digit(std::uint8_t c) { return c >= '0' && c <= '9'; }
std::uint8_t tone_scan(const AgentPacking& p) {   // 00406750's loop (also 00400f20)
    for (std::size_t j = 0; j < 32; ++j) {
        const auto m = get(p, 0x40 + 0x20 * j), t = get(p, 0x50 + 0x20 * j);
        if (digit(m[9])) return m[9];
        if (digit(t[9])) return t[9];
        if (t[0] == 0) return '_';
    }
    return '_';
}

std::int32_t slider_value(std::int32_t count, std::int32_t position) {
    // 100.0f / (count - 1) * position, cvt.w.s. The EE FPU gives +-FLT_MAX for x / 0 and clamps an
    // overflowing product to +-FLT_MAX; cvt.w.s of that is 0x7fffffff / 0x80000000.
    const float d = static_cast<float>(count - 1);
    float q = d == 0.0f ? FLT_MAX : 100.0f / d;
    double v = static_cast<double>(q) * static_cast<double>(static_cast<float>(position));
    if (v >= 2147483648.0) return std::numeric_limits<std::int32_t>::max();
    if (v < -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(static_cast<float>(v));
}
void show_position(AgentModify& m) {
    m.slider_value = slider_value(m.count, m.position);
    m.slot_text = m.count ? std::to_string(m.position + 1) + "/" + std::to_string(m.count) : "0/0";
}

}  // namespace

const AgentPartKey& agent_mask(AgentMask m) { return M(m); }

std::int32_t agent_slider_value(std::int32_t count, std::int32_t position) { return slider_value(count, position); }

AgentPartKey agent_part_key(std::string_view name) { return appearance_name_key(name); }

std::string agent_part_name(const AgentPartKey& key) {
    std::string s;
    for (auto c : key) {
        if (!c) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

void agent_cache_load_rules(AgentPartCache& cache, std::span<const std::uint8_t> swapit) {
    cache.rules.clear();
    for (std::size_t at = 0; at < swapit.size(); at += 0x80) {
        if (at + 16 > swapit.size()) fail(0x4037f0, "rule record past the end of SWAPIT.TBL");
        if (std::all_of(swapit.begin() + static_cast<std::ptrdiff_t>(at), swapit.begin() + static_cast<std::ptrdiff_t>(at + 16),
                        [](std::uint8_t b) { return b == 0; }))
            break;
        if (at + 0x80 > swapit.size()) fail(0x4037f0, "rule record past the end of SWAPIT.TBL");
        std::array<std::uint8_t, 0x80> r;
        std::memcpy(r.data(), swapit.data() + at, 0x80);
        cache.rules.push_back(r);
    }
}

void agent_cache_register_textures(AgentPartCache& cache, std::span<const std::string> members) {
    for (const auto& name : members) {
        auto k = agent_part_key(name);
        k[14] = '0';
        if (std::find(cache.textures.begin(), cache.textures.end(), k) == cache.textures.end())
            cache.textures.push_back(k);
    }
    cache.texture_static_count = cache.textures.size();  // 0x403f90: +d0 = +cc
}

namespace {
std::string extension(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot);
}
void mesh_find_or_add(AgentPartCache& cache, const Key& k) {   // 00403fd0
    for (const auto& h : cache.meshes)
        if (eq_under(h, k, M(AgentMask::m80))) return;
    cache.meshes.push_back(k);
}
}  // namespace

void agent_cache_register_morphs(AgentPartCache& cache, std::span<const AgentArchiveMember> members) {
    cache.locked_meshes.reset();
    cache.locked_textures.reset();
    for (const auto& m : members) {
        if (m.size == 0) continue;
        const auto ext = extension(m.name);
        if (ext == ".SKM" || ext == ".SKF") mesh_find_or_add(cache, agent_part_key(m.name));
    }
    cache.mesh_static_count = cache.meshes.size();  // 0x404568: +c8 = +c0
}

void agent_cache_register_meshes(AgentPartCache& cache, std::span<const std::string> members) {
    for (const auto& name : members)
        if (extension(name) == ".SKM") mesh_find_or_add(cache, agent_part_key(name));
    cache.mesh_static_count = cache.meshes.size();  // 0x404d58: +c8 = +c0
}

void agent_cache_register_bone_presets(AgentPartCache& cache, std::span<const AgentArchiveMember> members) {
    for (const auto& m : members)
        if (extension(m.name) == ".BON" && m.size != 0) cache.bone_presets.push_back(agent_part_key(m.name));
}

void agent_cache_load_sort(AgentPartCache& cache, std::span<const std::uint8_t> text) {
    if (!cache.sort.empty()) return;  // +ec != 0
    cache.sort = parse_man_sort(text);
}

AgentPartOrder agent_reorder_parts(const AgentPartCache& cache, AgentPacking& p) {
    std::array<ManSortPair, 32> pairs{};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        std::memcpy(pairs[i].mesh.data(), p.data() + 0x40 + 0x20 * i, 16);
        std::memcpy(pairs[i].texture.data(), p.data() + 0x50 + 0x20 * i, 16);
        const auto& k = pairs[i].mesh;
        if (std::all_of(k.begin(), k.begin() + 8, [](std::uint8_t b) { return b == 0; }) &&
            std::any_of(k.begin() + 8, k.end(), [](std::uint8_t b) { return b != 0; }))
            fail(0x3905bc, "pair " + std::to_string(i) + "'s mesh key is zero in its low 8 bytes only (lq / beqz)");
    }
    const ManSortOrder s = sort_man_parts(pairs, cache.sort);
    if (s.part_count >= 32) fail(0x390570, "32 or more pairs: the count loop reads past the packing");
    const AgentPacking was = p;
    for (std::size_t pos = 0; pos < s.order.size(); ++pos)
        std::memcpy(p.data() + 0x40 + 0x20 * pos, was.data() + 0x40 + 0x20 * s.order[pos], 0x20);
    AgentPartOrder o;
    o.count = s.part_count;
    for (std::size_t pos = 0; pos < 32; ++pos) {
        o.record[pos] = static_cast<std::int8_t>(s.record[pos]);
        o.value[pos] = static_cast<std::int8_t>(s.thermal_class[pos]);
    }
    return o;
}

void agent_cache_register_morph_list(AgentPartCache& cache, std::string_view name, std::span<const std::uint8_t> data) {
    if (data.empty() || name.empty() || name[0] == '1' || name[0] == '2') return;
    const auto k = agent_part_key(name);
    const std::size_t i = std::strcmp(reinterpret_cast<const char*>(k.data()), "FEMALE") == 0 ? 0 : 1;
    std::size_t n = 0;
    for (;; ++n) {
        if (16 * n + 8 > data.size()) fail(0x404060, "morph list " + std::string(name) + " has no terminating entry");
        if (std::all_of(data.begin() + static_cast<std::ptrdiff_t>(16 * n), data.begin() + static_cast<std::ptrdiff_t>(16 * n + 8),
                        [](std::uint8_t b) { return b == 0; }))
            break;
    }
    cache.morph_lists[i].assign(data.begin(), data.end());
    cache.morph_list_counts[i] = n;
}

namespace {
// strcmp over two byte ranges; running off either range throws.
int range_strcmp(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    for (std::size_t i = 0;; ++i) {
        if (i >= a.size() || i >= b.size()) fail(0x4073c0, "an unterminated name runs off its block");
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (a[i] == 0) return 0;
    }
}
}  // namespace

std::vector<std::uint8_t> agent_packing_compact(const AgentPartCache& c, const AgentPacking& p) {
    const std::int32_t sex = agent_packing_sex(p);
    if (sex != 0 && sex != 1) fail(0x4073c0, "sex " + std::to_string(sex) + " indexes past D_005712f8");
    const std::size_t count = agent_pair_count(p);
    const std::span<const std::uint8_t> ps(p);
    std::vector<std::int16_t> s;
    s.push_back(static_cast<std::int16_t>(count));
    std::int16_t v = -1;
    for (std::size_t i = 0; i < c.bone_presets.size(); ++i)
        if (range_strcmp(ps, std::span<const std::uint8_t>(c.bone_presets[i])) == 0) { v = static_cast<std::int16_t>(i); break; }
    s.push_back(v);
    v = -1;
    const auto& list = c.morph_lists[static_cast<std::size_t>(sex)];
    for (std::size_t i = 0; i < c.morph_list_counts[static_cast<std::size_t>(sex)]; ++i)
        if (range_strcmp(std::span<const std::uint8_t>(list).subspan(16 * i), ps.subspan(0x10)) == 0) {
            v = static_cast<std::int16_t>(i);
            break;
        }
    s.push_back(v);
    // The first match only: one past the static count is sent as its key (-1).
    const auto mesh_index = [&](std::size_t at) {
        const auto k = get(p, at);
        for (std::size_t i = 0; i < c.meshes.size(); ++i)
            if (eq_under(c.meshes[i], k, M(AgentMask::m80)))
                return i < c.mesh_static_count ? static_cast<std::int16_t>(i) : std::int16_t{-1};
        return std::int16_t{-1};
    };
    const auto texture_index = [&](std::size_t at) {
        const auto k = get(p, at);
        for (std::size_t i = 0; i < c.textures.size(); ++i)
            if (c.textures[i] == k)
                return i < c.texture_static_count ? static_cast<std::int16_t>(i) : std::int16_t{-1};
        return std::int16_t{-1};
    };
    s.push_back(mesh_index(0x20));
    const char* skel = reinterpret_cast<const char*>(p.data() + 0x30);
    s.push_back(std::strncmp(skel, "MALE.SKEL", 9) == 0 ? 1 : std::strncmp(skel, "FEMALE.SKEL", 11) == 0 ? 0 : -1);
    for (std::size_t j = 0; j <= count; ++j) {
        s.push_back(mesh_index(0x40 + 0x20 * j));
        s.push_back(texture_index(0x50 + 0x20 * j));
    }
    std::vector<std::uint8_t> out;
    for (auto x : s) {
        out.push_back(static_cast<std::uint8_t>(x));
        out.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(x) >> 8));
    }
    for (std::size_t k = 1; k < s.size(); ++k)
        if (s[k] == -1) out.insert(out.end(), p.begin() + static_cast<std::ptrdiff_t>(0x10 * (k - 1)),
                                   p.begin() + static_cast<std::ptrdiff_t>(0x10 * k));
    return out;
}

void agent_packing_expand(const AgentPartCache& c, std::span<const std::uint8_t> src, AgentPacking& dst) {
    const auto shorts = [&](std::size_t i) {
        if (2 * i + 2 > src.size()) fail(0x406ec0, "short " + std::to_string(i) + " past the compact appearance");
        return static_cast<std::int16_t>(src[2 * i] | (src[2 * i + 1] << 8));
    };
    std::int32_t n = shorts(0);
    if (n <= 0 || n >= 0x20) n = 1;
    std::size_t block = 2 + 2 * (2 * static_cast<std::size_t>(n) + 6);
    const auto take = [&](std::size_t at) {
        if (block + 16 > src.size()) fail(0x406ec0, "a key block past the compact appearance");
        if (at + 16 > dst.size()) fail(0x406ec0, "a key past the 0x440-byte packing");
        std::memcpy(dst.data() + at, src.data() + block, 16);
        block += 16;
    };
    const auto key_at = [&](const std::vector<Key>& keys, std::int16_t i, std::size_t at, const char* what) {
        if (i < 0 || static_cast<std::size_t>(i) >= keys.size())
            fail(0x406ec0, std::string(what) + " index " + std::to_string(i) + " reads past its " +
                               std::to_string(keys.size()) + " keys");
        if (at + 16 > dst.size()) fail(0x406ec0, "a key past the 0x440-byte packing");
        std::memcpy(dst.data() + at, keys[static_cast<std::size_t>(i)].data(), 16);
    };
    if (const auto b = shorts(1); b == -1) take(0x00);
    else {
        // An index (unsigned) at or past D_00571300 is 0; 00407ec0(dst, 15, preset) copies up to '.' / 0.
        const auto i = static_cast<std::uint16_t>(b) >= c.bone_presets.size() ? std::int16_t{0} : b;
        if (c.bone_presets.empty()) fail(0x406ec0, "a bone preset index with no presets registered (D_00571304)");
        const auto& preset = c.bone_presets[static_cast<std::size_t>(i)];
        Key k{};
        for (std::size_t t = 0; t < 15 && preset[t] != 0 && preset[t] != '.'; ++t) k[t] = preset[t];
        std::memcpy(dst.data(), k.data(), 16);
    }
    const auto morph = shorts(2);
    if (morph == -1) take(0x10);
    if (const auto h = shorts(3); h == -1) take(0x20);
    else key_at(c.meshes, h, 0x20, "the hard mesh");
    if (const auto s = shorts(4); s == -1) take(0x30);
    else {
        Key k{};
        const char* name = s == 1 ? "MALE.SKEL" : "FEMALE.SKEL";
        std::memcpy(k.data(), name, std::strlen(name));
        std::memcpy(dst.data() + 0x30, k.data(), 16);
    }
    for (std::int32_t j = 0; j <= n; ++j) {
        const auto at = 0x40 + 0x20 * static_cast<std::size_t>(j);
        if (const auto m = shorts(5 + 2 * static_cast<std::size_t>(j)); m == -1) take(at);
        else key_at(c.meshes, m, at, "a pair's mesh");
        if (const auto t = shorts(6 + 2 * static_cast<std::size_t>(j)); t == -1) take(at + 0x10);
        else key_at(c.textures, t, at + 0x10, "a pair's texture");
    }
    const auto end = 0x40 + 0x20 * static_cast<std::size_t>(n + 1);
    if (end + 0x20 > dst.size()) fail(0x406ec0, "pair n + 1's zero quads past the 0x440-byte packing");
    std::fill(dst.begin() + static_cast<std::ptrdiff_t>(end), dst.begin() + static_cast<std::ptrdiff_t>(end + 0x20), std::uint8_t{0});
    if (morph == -1) return;
    const std::int32_t sex = agent_packing_sex(dst);
    if (sex != 0 && sex != 1) fail(0x406ec0, "sex " + std::to_string(sex) + " indexes past D_005712f0");
    const auto list_index = static_cast<std::size_t>(sex);
    const auto i = morph >= static_cast<std::int64_t>(c.morph_list_counts[list_index]) ? std::int16_t{0} : morph;
    if (i < 0) fail(0x406ec0, "morph index " + std::to_string(i) + " reads before its list");
    const auto& list = c.morph_lists[list_index];
    if (16 * static_cast<std::size_t>(i) + 16 > list.size())
        fail(0x406ec0, "morph entry " + std::to_string(i) + " past its list (D_005712f8)");
    std::memcpy(dst.data() + 0x10, list.data() + 16 * static_cast<std::size_t>(i), 16);
}

void agent_cache_lock(AgentPartCache& cache, std::span<const std::string> names) {
    if (!cache.locked_meshes) {
        cache.locked_meshes = cache.meshes;
        cache.locked_textures = cache.textures;
    }
    for (const auto& name : names) {
        if (name.find('.') != std::string::npos) continue;
        const auto k = agent_part_key(name);
        bool hit = false;
        for (auto& m : cache.meshes)
            if (m == k) {
                m = Key{};
                hit = true;
            }
        if (hit) continue;
        for (auto& t : cache.textures)
            if (t == k) t = Key{};
    }
}

void agent_cache_unlock(AgentPartCache& cache) {
    if (!cache.locked_meshes) return;
    // do-while copies: at least one entry each (the count is never 0 once the editor has run).
    const auto restore = [](std::vector<Key>& dst, const std::vector<Key>& src) {
        if (src.empty()) fail(0x403310, "restoring an empty key list (the original copies one entry)");
        std::copy(src.begin(), src.end(), dst.begin());
    };
    restore(cache.meshes, *cache.locked_meshes);
    restore(cache.textures, *cache.locked_textures);
    cache.locked_meshes.reset();
    cache.locked_textures.reset();
}

std::size_t agent_pair_count(const AgentPacking& p) {
    std::size_t n = 0;
    for (;;) {
        const std::size_t at = 0x40 + 0x20 * n;
        if (at + 8 > p.size()) fail(0x406dc0, "no terminator pair inside the block");
        if (std::all_of(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + 8),
                        [](std::uint8_t b) { return b == 0; }))
            return n;
        ++n;
    }
}

std::int32_t agent_packing_sex(const AgentPacking& p) {
    for (std::size_t j = 0; j < 32; ++j) {
        const auto m = get(p, 0x40 + 0x20 * j);
        if (std::memcmp(m.data() + 1, "BODY", 4) == 0) return 2;
        if (m[0] == '1') return 1;
        if (m[0] == '2') return 0;
        if (m[0] == 0) return j == 0 ? 1 : 2;
    }
    return 2;
}

std::int8_t agent_packing_tone(const AgentPacking& p) { return static_cast<std::int8_t>(tone_scan(p)); }

std::int32_t agent_pair_of(const AgentPacking& p, std::string_view five) {
    for (std::size_t j = 0;; ++j) {
        const auto m = get(p, 0x40 + 0x20 * j);
        if (std::all_of(m.begin(), m.begin() + 8, [](std::uint8_t b) { return b == 0; })) {
            const auto t = get(p, 0x50 + 0x20 * j);
            return std::strncmp(reinterpret_cast<const char*>(t.data() + 1), five.data(), 5) == 0
                ? static_cast<std::int32_t>(j) : -1;
        }
        if (std::strncmp(reinterpret_cast<const char*>(m.data() + 1), five.data(), 5) == 0) return static_cast<std::int32_t>(j);
    }
}

std::int32_t agent_texture_step(const AgentPartCache& c, std::int32_t dir, Key mesh, Key tex, std::int32_t start,
                                std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p) {
    Key key = mesh;
    key[5] = tex[5];
    key[9] = tex[9];
    const Key t5 = key[4] == 'X' ? mask_xor(M(AgentMask::m50), M(AgentMask::mb0)) : M(AgentMask::m50);
    const auto ex = excluded(sex, tex[0], 0x4002b0);
    const auto s = wrap_scan(c.textures.size(), start, dir, [&](std::size_t i) {
        const auto& t = c.textures[i];
        return eq_under(t, key, t5) && (eq_under(t, key, M(AgentMask::m60)) || is_set_all(t)) && t[0] != ex &&
               tone_ok(t, tone);
    });
    if (s != -1) put(p, tex_at(pair), c.textures[static_cast<std::size_t>(s)]);
    return s;
}

void agent_texture_find(const AgentPartCache& c, Key key, std::int32_t pair, std::int32_t sex, std::int8_t tone,
                        AgentPacking& p) {
    const auto ex = excluded(sex, std::nullopt, 0x4022e0);
    Key m90 = M(AgentMask::m90), m50 = M(AgentMask::m50);
    if (key[4] == 'X') {
        m90 = mask_xor(m90, M(AgentMask::mb0));
        m50 = mask_xor(m50, M(AgentMask::mb0));
    }
    const auto t9 = static_cast<std::uint8_t>(tone);
    const std::array<std::function<bool(const Key&)>, 6> passes{
        [&](const Key& t) { return t == key && t[9] == t9; },
        [&](const Key& t) { return t == key && t[9] == '_'; },
        [&](const Key& t) { return eq_under(t, key, m90) && t[0] != ex && t[9] == t9; },
        [&](const Key& t) { return eq_under(t, key, m90) && t[0] != ex && t[9] == '_'; },
        [&](const Key& t) { return eq_under(t, key, m50) && t[0] != ex && t[9] == t9; },
        [&](const Key& t) { return eq_under(t, key, m50) && t[0] != ex && t[9] == '_'; },
    };
    for (const auto& pass : passes)
        for (const auto& t : c.textures)
            if (pass(t)) {
                put(p, tex_at(pair), t);
                return;
            }
    put(p, tex_at(pair), key);
}

void agent_mesh_rules(const AgentPartCache& c, AgentPacking& p, std::size_t count, Key old_name, Key new_name,
                      std::int32_t sex, std::int8_t tone) {
    if (!c.rules_enabled) return;
    for (std::size_t j = 0; j < count; ++j) {
        for (const auto& r : c.rules) {
            const auto R = [&](std::size_t o) { Key k; std::memcpy(k.data(), r.data() + o, 16); return k; };
            const Key r0 = R(0), r10 = R(0x10), r20 = R(0x20), r30 = R(0x30), r50 = R(0x50), r70 = R(0x70);
            const Key pm = get(p, mesh_at(static_cast<std::int32_t>(j)));
            if (!eq_under(old_name, r0, r10) || !eq_under(new_name, r20, r30) || pm == new_name ||
                !eq_under(pm, old_name, r10))
                continue;
            const Key t3 = mask_xor(r70, r50);
            Key t2{};
            for (std::size_t b = 0; b < 16; ++b)
                t2[b] = static_cast<std::uint8_t>(t3[b] & ((new_name[b] & r70[b]) | (r50[b] & pm[b])));
            bool found = false;
            for (const auto& k : c.meshes) {
                Key km{};
                for (std::size_t b = 0; b < 16; ++b) km[b] = static_cast<std::uint8_t>(t3[b] & k[b]);
                if (km != t2) continue;
                found = true;
                if (k != pm) {
                    put(p, mesh_at(static_cast<std::int32_t>(j)), k);
                    const Key tk = merge(k, get(p, tex_at(static_cast<std::int32_t>(j))), M(AgentMask::m70));
                    agent_texture_find(c, tk, static_cast<std::int32_t>(j), sex, tone, p);
                }
                break;
            }
            if (found) break;
        }
    }
}

void agent_texture_rules(const AgentPartCache& c, AgentPacking& p, std::size_t count, Key old_name, Key new_name) {
    if (!c.rules_enabled) return;
    Key tmp = new_name;
    if (new_name[9] == p0[9]) tmp[9] = tone_scan(p);
    for (std::size_t j = 0; j < count; ++j) {
        const auto slot = static_cast<std::int32_t>(j);
        bool found = false;
        for (const auto& r : c.rules) {
            const auto R = [&](std::size_t o) { Key k; std::memcpy(k.data(), r.data() + o, 16); return k; };
            const Key r0 = R(0), r10 = R(0x10), r20 = R(0x20), r30 = R(0x30), r50 = R(0x50), r70 = R(0x70);
            const Key pt = get(p, tex_at(slot));
            if (!eq_under(old_name, r0, r10) || !eq_under(new_name, r20, r30) || pt == new_name ||
                !eq_under(pt, old_name, r10))
                continue;
            const Key t7 = mask_xor(r70, r50);
            Key t6{};
            for (std::size_t b = 0; b < 16; ++b)
                t6[b] = static_cast<std::uint8_t>(t7[b] & ((new_name[b] & r70[b]) | (r50[b] & pt[b])));
            for (const auto& t : c.textures) {
                if (t == pt) continue;
                Key tm{};
                for (std::size_t b = 0; b < 16; ++b) tm[b] = static_cast<std::uint8_t>(t7[b] & t[b]);
                if (tm != t6) continue;
                if (t[9] != tmp[9] && t[9] != p0[9]) continue;
                put(p, tex_at(slot), t);
                found = true;
                break;
            }
            if (found) break;
        }
        if (found) continue;
        const Key pt = get(p, tex_at(slot));
        if (pt[9] != tmp[9] && pt[9] != '_') {
            Key k = pt;
            k[9] = tmp[9];
            agent_texture_find(c, k, slot, agent_packing_sex(p), static_cast<std::int8_t>(tmp[9]), p);
        }
    }
}

std::int32_t agent_texture_step2(const AgentPartCache& c, std::int32_t dir, Key mesh, Key tex, std::int32_t start,
                                 std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p,
                                 std::size_t count) {
    Key key = mesh;
    key[5] = tex[5];
    key[9] = tex[9];
    const Key t5 = key[4] == 'X' ? mask_xor(M(AgentMask::m50), M(AgentMask::mb0)) : M(AgentMask::m50);
    const auto ex = excluded(sex, tex[0], 0x400620);
    const auto s = wrap_scan(c.textures.size(), start, dir, [&](std::size_t i) {
        const auto& t = c.textures[i];
        return eq_under(t, key, t5) && t[0] != ex && tone_ok(t, tone);
    });
    if (s == -1) return -1;
    const Key hit = c.textures[static_cast<std::size_t>(s)];
    put(p, tex_at(pair), hit);
    if (key[5] != '_') agent_texture_rules(c, p, count, key, hit);
    return s;
}

std::int32_t agent_colour_step(const AgentPartCache& c, std::int32_t dir, Key mesh, Key tex, std::int32_t start,
                               std::int32_t pair, std::int32_t sex, AgentPacking& p, std::size_t count) {
    Key key = mesh;
    key[5] = tex[5];
    key[9] = tex[9];
    const Key t6 = key[4] == 'X' ? mask_xor(M(AgentMask::m50), M(AgentMask::mb0)) : M(AgentMask::m50);
    const auto ex = excluded(sex, tex[0], 0x400990);
    const auto n = c.textures.size();
    const auto base = [&](std::size_t i) { const auto& t = c.textures[i]; return eq_under(t, key, t6) && t[0] != ex; };
    const auto set = [&](std::size_t i) {
        const auto& t = c.textures[i];
        return base(i) && (eq_under(t, key, M(AgentMask::m60)) || is_set_all(t));
    };
    std::int32_t s = wrap_scan(n, start, dir, set);
    if (s == -1) {
        const std::int64_t from = static_cast<std::int64_t>(start) + dir;
        if (static_cast<std::uint32_t>(from) < n) s = wrap_scan(n, start, dir, base);
        else
            for (std::int64_t i = dir != -1 ? 0 : static_cast<std::int64_t>(n) - 1; static_cast<std::uint32_t>(i) < n; i += dir)
                if (base(static_cast<std::size_t>(i))) {
                    s = static_cast<std::int32_t>(i);
                    break;
                }
    }
    if (s == -1) return -1;
    const Key hit = c.textures[static_cast<std::size_t>(s)];
    put(p, tex_at(pair), hit);
    if (hit[9] != key[9] || key[5] != '_') agent_texture_rules(c, p, count, key, hit);
    return s;
}

std::int32_t agent_mesh_step(const AgentPartCache& c, std::int32_t dir, Key name, std::int32_t start,
                             std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p,
                             std::size_t count) {
    const auto ex = excluded(sex, std::uint8_t{0}, 0x401970);
    const auto s = wrap_scan(c.meshes.size(), start, dir, [&](std::size_t i) {
        const auto& k = c.meshes[i];
        return eq_under(k, name, M(AgentMask::ma0)) && k[0] != ex;
    });
    if (s == -1) return -1;
    const Key hit = c.meshes[static_cast<std::size_t>(s)];
    put(p, mesh_at(pair), hit);
    Key tk = merge(hit, get(p, tex_at(pair)), M(AgentMask::m70));
    tk[9] = static_cast<std::uint8_t>(tone);
    agent_texture_find(c, tk, pair, sex, tone, p);
    if (!eq_under(hit, p10, M(AgentMask::m70))) agent_mesh_rules(c, p, count, name, hit, sex, tone);
    return s;
}

std::int32_t agent_mesh_step4(const AgentPartCache& c, std::int32_t dir, Key name, std::int32_t start,
                              std::int32_t pair, std::int32_t sex, std::int8_t tone, AgentPacking& p) {
    const auto ex = excluded(sex, std::uint8_t{0}, 0x401570);
    Key mask{};
    for (std::size_t b = 0; b < 16; ++b) mask[b] = static_cast<std::uint8_t>(M(AgentMask::ma0)[b] | M(AgentMask::m70)[b]);
    const auto s = wrap_scan(c.meshes.size(), start, dir, [&](std::size_t i) {
        const auto& k = c.meshes[i];
        return eq_under(k, name, mask) && k[0] != ex;
    });
    if (s == -1) return -1;
    const Key hit = c.meshes[static_cast<std::size_t>(s)];
    put(p, mesh_at(pair), hit);
    agent_texture_find(c, merge(hit, get(p, tex_at(pair)), M(AgentMask::m70)), pair, sex, tone, p);
    return s;
}

std::int32_t agent_count_meshes(const AgentPartCache& c, Key name, std::int32_t& pos, std::int32_t sex, bool item) {
    const auto ex = excluded(sex, std::nullopt, 0x402200);
    Key mask = M(AgentMask::m50);
    if (item)
        for (std::size_t b = 0; b < 16; ++b) mask[b] |= M(AgentMask::m70)[b];
    std::int32_t n = 0;
    for (const auto& k : c.meshes) {
        if (k[0] == ex || !eq_under(k, name, mask)) continue;
        if (k == name) pos = n;
        ++n;
    }
    return n;
}

std::int32_t agent_count_textures(const AgentPartCache& c, Key key, std::int32_t& pos, std::int32_t sex,
                                  std::int8_t tone, std::uint32_t flags) {
    const auto ex = excluded(sex, std::nullopt, 0x401ff0);
    Key mask = M(AgentMask::m50);
    if (flags & 4)
        for (std::size_t b = 0; b < 16; ++b) mask[b] |= M(AgentMask::m70)[b];
    std::int32_t n = 0;
    Key last{};
    for (const auto& t : c.textures) {
        if (t[0] == ex || !eq_under(t, key, mask)) continue;
        if ((flags & 1) && !tone_ok(t, tone)) continue;
        if ((flags & 2) && !(eq_under(t, key, M(AgentMask::m60)) || is_set_all(t))) continue;
        if ((flags & 8) && n > 0 && eq_under(t, last, M(AgentMask::m60))) continue;
        if (t == key) pos = n;
        last = t;
        ++n;
    }
    return n;
}

std::int32_t agent_count_pairs(const AgentPartCache& c, Key mesh, Key tex, std::int32_t& pos, std::int32_t sex,
                               std::int8_t tone, std::uint32_t flags) {
    const auto ex = excluded(sex, std::nullopt, 0x401e40);
    std::int32_t total = 0;
    for (const auto& k : c.meshes) {
        if (k[0] == ex || !eq_under(k, mesh, M(AgentMask::m50))) continue;
        std::int32_t sub = -1;
        const auto n = agent_count_textures(c, merge(tex, k, M(AgentMask::m60)), sub, sex, tone, flags);
        if (k == mesh) pos = sub != -1 ? total + sub : total;
        total += n ? n : 1;
    }
    return total;
}

namespace {
// D_00492900: the category patterns, [sex][category] (8 bytes each, bytes 1..5 compared).
constexpr std::array<std::array<const char*, 16>, 2> category_patterns{{
    {"_HAIR_", "_HAIR_", "_HAT-_", "_GLAS_", "_MAKE_", "_MAKE_", "_SHIR_", "_VEST_", "_LEGS_", "_BOOT_",
     "_TATT1", "_TATT2", "_TATT3", "_PATC1", "_PATC2", "_PATC3"},
    {"_HAIR_", "_HAIR_", "_HAT-_", "_GLAS_", "_BEAR_", "_MAKE_", "_SHIR_", "_VEST_", "_LEGS_", "_BOOT_",
     "_TATT1", "_TATT2", "_TATT3", "_PATC1", "_PATC2", "_PATC3"},
}};
// D_00492a00's kinds (byte 0 of each entry).
constexpr std::array<std::uint8_t, 16> category_kinds{4, 1, 3, 3, 2, 2, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1};
}  // namespace

AgentModify agent_modify_open(const AgentPartCache& c, const AgentPacking& packing,
                              std::span<const AgentModifyCategory, 16> table) {
    AgentModify m;
    m.packing = packing;
    m.pair_count = agent_add_blank_pairs(c, m.packing, agent_pair_count(m.packing));  // 00406dc0, 004061b0
    m.order = agent_reorder_parts(c, m.packing);                                        // 00390570
    m.pair_count = m.order.count;
    m.camera = agent_camera_initial(c);                                                 // 00344934
    m.sex = agent_packing_sex(m.packing);
    m.tone = agent_packing_tone(m.packing);
    std::copy(table.begin(), table.end(), m.table.begin());
    for (std::size_t k = 0; k < 16; ++k) m.table[k].kind = category_kinds[k];
    if (m.sex != 0 && m.sex != 1)
        fail(0x3446d0, "sex " + std::to_string(m.sex) + " indexes past the two pattern rows of D_00492900");
    for (std::size_t cat = 0; cat < 16; ++cat) {
        const char* pat = category_patterns[static_cast<std::size_t>(m.sex)][cat];
        for (std::size_t v = 0; v <= m.pair_count; ++v) {
            const auto mesh = get(m.packing, 0x40 + 0x20 * v), tex = get(m.packing, 0x50 + 0x20 * v);
            if (std::memcmp(pat + 1, mesh.data() + 1, 5) == 0 || std::memcmp(pat + 1, tex.data() + 1, 5) == 0) {
                m.table[cat].pair = static_cast<std::int8_t>(v);
                break;
            }
        }
        m.table[cat].position = 0;
    }
    // +10f = 0x10: +10d / +10e / +100 / +104 then read past the table (D_00492a40, the category names)
    // and are rewritten by the first 00341c50 before 00341330 (which returns while +10f >= 16) runs.
    m.category = 16;
    agent_modify_refresh(c, m);
    return m;
}

void agent_modify_refresh(const AgentPartCache& c, AgentModify& m) {
    std::int32_t pos = -1;
    if (m.category < 0) fail(0x343940, "negative category");
    if (m.category >= 16) {
        m.position = 0;
        return;
    }
    auto& entry = m.table[static_cast<std::size_t>(m.category)];
    if (m.pair == -1) {
        m.position = 0;
        m.count = 0;
        return;
    }
    const Key mesh = get(m.packing, mesh_at(m.pair)), tex = get(m.packing, tex_at(m.pair));
    switch (m.kind) {
    case 3: m.count = agent_count_pairs(c, mesh, tex, pos, m.sex, m.tone, 3); break;
    case 0: m.count = agent_count_meshes(c, mesh, pos, m.sex, false); break;
    case 4: m.count = agent_count_meshes(c, mesh, pos, m.sex, true); break;
    case 1:
        if (mesh[6] == 'B' && mesh[7] == 'L' && mesh[8] == 'A') {
            m.count = 0;
            pos = 0;
        } else {
            m.count = agent_count_textures(c, merge(tex, mesh, M(AgentMask::m60)), pos, m.sex, m.tone, 2);
        }
        break;
    case 2: m.count = agent_count_textures(c, tex, pos, m.sex, m.tone, 0); break;
    case 5: m.count = agent_count_textures(c, tex, pos, m.sex, m.tone, 1); break;
    default: fail(0x343940, "category kind " + std::to_string(m.kind));
    }
    if (pos != -1) entry.position = static_cast<std::uint16_t>(pos);
    m.position = entry.position;
}

void agent_modify_select(const AgentPartCache& c, AgentModify& m, std::string_view name) {
    std::int8_t cat = 0;
    while (cat < 16 && name != agent_modify_categories[static_cast<std::size_t>(cat)]) ++cat;
    m.previous_category = m.category;  // 00341cb4: the branch's delay slot (also when unchanged)
    if (cat == m.category) return;
    m.category = cat;
    m.refresh_e0 = 1;
    if (cat >= 16) {
        m.count = 0;
        m.position = 0;
        m.slider_shown = false;
        m.slot_text.clear();
        return;
    }
    auto& entry = m.table[static_cast<std::size_t>(cat)];
    m.pair = entry.pair;
    if (m.pair == -1) {
        m.slider_shown = false;
        m.kind = 6;
        return;
    }
    m.kind = entry.kind;
    const Key mesh = get(m.packing, mesh_at(m.pair)), tex = get(m.packing, tex_at(m.pair));
    const auto km = std::find(c.meshes.begin(), c.meshes.end(), mesh);
    m.mesh_index = km == c.meshes.end() ? 0 : static_cast<std::int32_t>(km - c.meshes.begin());     // 00404690
    const auto kt = std::find(c.textures.begin(), c.textures.end(), tex);
    m.texture_index = kt == c.textures.end() ? 0 : static_cast<std::int32_t>(kt - c.textures.begin()); // 00401de0
    agent_modify_refresh(c, m);
    m.position = m.count ? m.position % m.count : 0;
    entry.position = static_cast<std::uint16_t>(m.position);
    m.slider_shown = true;
    show_position(m);
}

void agent_modify_step(const AgentPartCache& c, AgentModify& m, bool back, bool forward) {
    if (!back && !forward) return;
    if (m.category >= 16 || m.count == 0) return;
    m.dirty = true;   // 00341378: sb 1, +158
    const std::int32_t dir = back ? -1 : 1;
    const auto pair = m.pair;
    const auto mesh = [&] { return get(m.packing, mesh_at(pair)); };
    const auto tex = [&] { return get(m.packing, tex_at(pair)); };
    const std::int8_t model_tone = m.tone;   // model +2c6d (= +fc, both set by the ctor)
    switch (m.kind) {
    case 3: {
        const auto v = agent_texture_step(c, dir, mesh(), tex(), m.texture_index, pair, m.sex, m.tone, m.packing);
        if ((dir == 1 && m.texture_index < v) || (dir == -1 && v < m.texture_index)) {
            m.texture_index = v;
            break;
        }
        if (dir != 1 && dir != -1) break;
        const auto s = agent_mesh_step(c, dir, mesh(), m.mesh_index, pair, m.sex, model_tone, m.packing, m.pair_count);
        if (s == -1) break;
        m.mesh_index = s;
        m.texture_index = -1;
        m.texture_index = agent_texture_step(c, dir, mesh(), tex(), -1, pair, m.sex, m.tone, m.packing);
        break;
    }
    case 0:
    case 4: {
        const auto s = m.kind == 0
            ? agent_mesh_step(c, dir, mesh(), m.mesh_index, pair, m.sex, model_tone, m.packing, m.pair_count)
            : agent_mesh_step4(c, dir, mesh(), m.mesh_index, pair, m.sex, model_tone, m.packing);
        if (s == -1) break;
        m.mesh_index = s;
        const auto t = tex();
        const auto kt = std::find(c.textures.begin(), c.textures.end(), t);
        m.texture_index = kt == c.textures.end() ? 0 : static_cast<std::int32_t>(kt - c.textures.begin());
        break;
    }
    case 1:
    case 2: {
        std::int32_t v;
        if (static_cast<std::size_t>(pair) < m.pair_count) {
            v = m.count < 2 ? -1
                : m.kind == 1 ? agent_colour_step(c, dir, mesh(), tex(), m.texture_index, pair, m.sex, m.packing, m.pair_count)
                              : agent_texture_step2(c, dir, mesh(), tex(), m.texture_index, pair, m.sex, model_tone,
                                                    m.packing, m.pair_count);
        } else {
            const auto make = agent_part_key("_MAKE_XXX_NONE0");   // *D_0048b460
            v = m.kind == 1 ? agent_colour_step(c, dir, make, tex(), m.texture_index, pair, m.sex, m.packing, m.pair_count)
                            : agent_texture_step2(c, dir, make, tex(), m.texture_index, pair, m.sex, model_tone,
                                                  m.packing, m.pair_count);
        }
        if (v != -1) m.texture_index = v;
        break;
    }
    case 5: {
        const auto v = agent_texture_step(c, dir, mesh(), tex(), m.texture_index, pair, m.sex, m.tone, m.packing);
        if (v != -1) m.texture_index = v;
        break;
    }
    default: break;
    }
    const auto old = m.position;
    m.position += dir;
    if (m.position < 0) m.position = m.count - 1;
    if (m.position < 0) m.position = 0;
    if (m.position >= m.count) m.position = 0;
    if (m.position != old) ++m.changes;
    m.table[static_cast<std::size_t>(m.category)].position = static_cast<std::uint16_t>(m.position);
    show_position(m);
}

void agent_modify_patch_advance(const AgentPartCache& c, AgentModify& m, std::int32_t pair, bool two_digits) {
    const std::uint32_t address = two_digits ? 0x343680 : 0x3437c0;
    if (pair < -2) fail(address, "pair " + std::to_string(pair) + " before the block");
    const auto at = static_cast<std::size_t>(0x50 + 0x20 * pair);
    auto key = get(m.packing, at);
    const auto held = [&](const Key& k) { return std::find(c.textures.begin(), c.textures.end(), k) != c.textures.end(); };  // 004027e0
    if (held(key)) return;
    const auto bump = [](std::uint8_t& b) { b = static_cast<std::uint8_t>(b + 1); };
    for (int tries = 0; tries < 0x20; ++tries) {
        if (two_digits) {           // bytes 11..12
            if (key[12] == '9') { bump(key[11]); key[12] = '0'; } else bump(key[12]);
        } else if (key[13] == '-') { // bytes 12..13, "9-" -> "10"
            if (key[12] == '9') { key[12] = '1'; key[13] = '0'; } else bump(key[12]);
        } else if (key[13] == '9') {
            bump(key[12]);
            key[13] = '0';
        } else {
            bump(key[13]);
        }
        if (!held(key)) continue;
        if (pair < 0) fail(address, "a held key for pair " + std::to_string(pair) + " would rewrite the packing header");
        agent_texture_find(c, key, pair, static_cast<std::int32_t>(static_cast<std::uint8_t>(m.sex)), m.tone, m.packing);
        put(m.packing, at, key);
        return;
    }
}

bool agent_modify_part_blank(const AgentPacking& p, std::int32_t pair) {
    if (pair < -1) fail(0x340850, "pair " + std::to_string(pair) + " before the block");
    return eq_under(get(p, static_cast<std::size_t>(0x40 + 0x20 * pair)), p0, M(AgentMask::m60));
}

void agent_modify_save_merge(AgentPacking& p) {
    const auto pair = agent_pair_of(p, "VEST_");
    if (pair == -1) return;
    put(p, 0x20, merge(get(p, 0x20), get(p, mesh_at(pair)), M(AgentMask::m70)));
}

}  // namespace omega
