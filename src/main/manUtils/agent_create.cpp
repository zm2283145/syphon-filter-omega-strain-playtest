#include "agent_create.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

using Key = AgentPartKey;

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char a[16];
    std::snprintf(a, sizeof a, "%08x", address);
    throw std::runtime_error(std::string(a) + ": " + what);
}

// Pair j's mesh at +40 + 0x20 j, its texture at +50 + 0x20 j. GuiAgentCreate loads the pair signed
// (lb +ed): pair -1 reads +20 / +30 (the hard mesh name and the skeleton name), still in the block.
std::size_t mesh_off(std::int32_t pair, std::uint32_t address) {
    const std::int64_t at = 0x40 + 0x20 * static_cast<std::int64_t>(pair);
    if (at < 0 || at + 0x20 > 0x440) fail(address, "pair " + std::to_string(pair) + " outside the 0x440-byte block");
    return static_cast<std::size_t>(at);
}
Key get(const AgentPacking& p, std::size_t at) {
    Key k;
    std::memcpy(k.data(), p.data() + at, 16);
    return k;
}
Key mesh_of(const AgentPacking& p, std::int32_t pair, std::uint32_t address) { return get(p, mesh_off(pair, address)); }
Key tex_of(const AgentPacking& p, std::int32_t pair, std::uint32_t address) { return get(p, mesh_off(pair, address) + 0x10); }

bool eq_under(const Key& a, const Key& b, const Key& m) {
    for (std::size_t i = 0; i < 16; ++i)
        if ((a[i] & m[i]) != (b[i] & m[i])) return false;
    return true;
}
Key merge(const Key& a, const Key& b, const Key& m) {   // (a & ~m) | (b & m)
    Key r{};
    for (std::size_t i = 0; i < 16; ++i) r[i] = static_cast<std::uint8_t>((a[i] & ~m[i]) | (b[i] & m[i]));
    return r;
}
std::int32_t find_mesh(const AgentPartCache& c, const Key& k) {   // 00404690: exact, miss 0
    const auto i = std::find(c.meshes.begin(), c.meshes.end(), k);
    return i == c.meshes.end() ? 0 : static_cast<std::int32_t>(i - c.meshes.begin());
}
std::int32_t find_tex(const AgentPartCache& c, const Key& k) {    // 00401de0: exact, miss 0
    const auto i = std::find(c.textures.begin(), c.textures.end(), k);
    return i == c.textures.end() ? 0 : static_cast<std::int32_t>(i - c.textures.begin());
}
std::int32_t find_mesh_m80(const AgentPartCache& c, const Key& k) {   // 004046f0: under mask +80, miss -1
    for (std::size_t i = 0; i < c.meshes.size(); ++i)
        if (eq_under(c.meshes[i], k, agent_mask(AgentMask::m80))) return static_cast<std::int32_t>(i);
    return -1;
}

// D_0048b470 (6 bytes each, bytes 1..4 compared with a pair's texture name) and D_0048b500's types.
constexpr std::array<const char*, 11> category_names{
    "_NECK", "_NECK", "_NECK", "_NECK", "_HEAD", "_HEAD", "_HAIR", "_HAIR", "_EYE-", "_MAKE", "_BEAR"};
constexpr std::array<std::uint8_t, 11> category_types{4, 3, 5, 1, 8, 6, 7, 1, 1, 2, 2};
const Key make_none = agent_part_key("_MAKE_XXX_NONE0");   // *D_0048b460

AgentCreateEntry& entry(AgentCreate& a, std::int32_t sex, std::int32_t category, std::uint32_t address) {
    const std::int64_t i = static_cast<std::int64_t>(sex) * 11 + category;
    if (i < 0 || i >= 24) fail(address, "D_0048b500 entry " + std::to_string(i) + " (sex " + std::to_string(sex) + ")");
    return a.table[static_cast<std::size_t>(i)];
}

void show_position(AgentCreate& a) {
    a.slider_value = agent_slider_value(a.count, a.position);
    a.slot_text = a.count ? std::to_string(a.position + 1) + "/" + std::to_string(a.count) : "0/0";
}

void read_sex_tone(AgentCreate& a) {   // the d8 / dc / +2c6c / +2c6d refresh of 0028c390 / 0028cb20
    a.sex = agent_packing_sex(a.model().packing);
    a.model().sex = a.sex;
    a.tone = agent_packing_tone(a.model().packing);
    a.model().tone = a.tone;
}

// +ec: the first pair below the count whose mesh name bytes 1..4 are "HEAD" (D_004a9a28 "1HEAD_");
// none leaves it.
void find_head(AgentCreate& a) {
    const auto& m = a.model();
    for (std::size_t j = 0; j < m.pair_count; ++j)
        if (std::memcmp(m.packing.data() + 0x40 + 0x20 * j + 1, "HEAD", 4) == 0) {
            a.head_pair = static_cast<std::int8_t>(j);
            return;
        }
}

// strcmp(packing + at, key) with the key's 16 bytes (an unterminated key compares its 16 bytes).
int key_strcmp(const AgentPacking& p, std::size_t at, const Key& k) {
    for (std::size_t i = 0;; ++i) {
        const std::uint8_t a = p[at + i], b = i < 16 ? k[i] : 0;
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;
    }
}

}  // namespace

AgentCreateTable agent_create_table_initial() {
    AgentCreateTable t{};
    for (std::size_t s = 0; s < 2; ++s)
        for (std::size_t k = 0; k < 11; ++k) t[s * 11 + k].type = category_types[k];
    return t;   // 22, 23: type 0; every pair 0xff, every position 0
}

std::uint32_t agent_random_next(std::uint64_t& state) {
    state = state * 0x5851f42d4c957f2dULL + 1ULL;
    return static_cast<std::uint32_t>(state >> 32) & 0x7fffffffU;
}

bool agent_apply_scale_preset(const AgentPartCache& c, AgentCreateModel& m, std::int32_t preset) {
    const std::size_t n = c.bone_presets.size();
    std::size_t p = 0;
    if (preset < 0) {
        while (p < n && key_strcmp(m.packing, 0, c.bone_presets[p]) != 0) ++p;
        if (p >= n) return false;
    } else {
        p = static_cast<std::size_t>(preset);
        if (p >= n) return false;
    }
    std::memcpy(m.packing.data(), c.bone_presets[p].data(), 15);
    m.preset = static_cast<std::int8_t>(p);
    return true;
}

std::int32_t agent_morph_count(const AgentPartCache& c, std::int32_t sex) {
    if (sex != 0 && sex != 1) fail(0x38de20, "sex " + std::to_string(sex) + " indexes past D_005712f8");
    const auto i = static_cast<std::size_t>(sex);
    std::int32_t n = 0;
    for (std::size_t k = 0; k < c.morph_list_counts[i]; ++k)
        if (std::memcmp(c.morph_lists[i].data() + 16 * k, "SHAPE_", 6) == 0) ++n;
    return n;
}

std::int32_t agent_morph_step(const AgentPartCache& c, AgentCreateModel& m, std::int32_t dir, std::int32_t cur,
                              std::int32_t sex) {
    if (sex != 0 && sex != 1) fail(0x38df00, "sex " + std::to_string(sex) + " indexes past D_005712f8");
    const auto& list = c.morph_lists[static_cast<std::size_t>(sex)];
    const auto n = static_cast<std::uint32_t>(c.morph_list_counts[static_cast<std::size_t>(sex)]);
    if (agent_morph_count(c, sex) == 0)
        fail(0x38df00, "morph list " + std::to_string(sex) + " has no \"SHAPE_\" entry (the original loops forever)");
    std::int32_t i = cur;
    for (;;) {
        i += dir;
        if (i < 0) i = static_cast<std::int32_t>(n) - 1;
        if (static_cast<std::uint32_t>(i) >= n) i = 0;
        if (16 * static_cast<std::size_t>(i) + 16 > list.size()) fail(0x38df00, "morph entry past its list");
        if (std::memcmp(list.data() + 16 * static_cast<std::size_t>(i), "SHAPE_", 6) == 0) break;
    }
    std::memcpy(m.packing.data() + 0x10, list.data() + 16 * static_cast<std::size_t>(i), 16);
    m.morph_index = static_cast<std::uint8_t>(i);
    m.morph_on = std::strcmp(reinterpret_cast<const char*>(m.packing.data() + 0x10), "SHAPE_BASE") != 0;
    return i;
}

std::size_t agent_add_blank_pairs(const AgentPartCache& c, AgentPacking& p, std::size_t count) {
    const std::int32_t sex = agent_packing_sex(p);
    if (sex != 0 && sex != 1)
        fail(0x4061b0, "sex " + std::to_string(sex) + " leaves the excluded byte unset");
    const std::uint8_t ex = sex == 1 ? '2' : '1';
    const Key& m60 = agent_mask(AgentMask::m60);
    const Key& ma0 = agent_mask(AgentMask::ma0);
    const Key bla = agent_part_key("1belt_BLA_NONE_");   // the cache's +10 key
    const auto held = [&](const Key& k) {
        for (std::size_t j = 0; j < count; ++j)
            if (eq_under(k, get(p, 0x40 + 0x20 * j), ma0)) return true;
        return false;
    };
    const auto add = [&](const Key& mesh, const Key& tex) {
        if (count + 2 > 31) fail(0x4061b0, "a blank pair past the 32 pairs of the block");
        const std::size_t at = 0x40 + 0x20 * count;
        std::memmove(p.data() + at + 0x20, p.data() + at, 0x40);
        std::memcpy(p.data() + at, mesh.data(), 16);
        std::memcpy(p.data() + at + 0x10, tex.data(), 16);
        ++count;
    };
    for (const auto& k : c.meshes) {
        if (!eq_under(k, bla, m60) || k[0] == ex || held(k)) continue;
        add(k, k);
    }
    for (const auto& t : c.textures) {
        if (!eq_under(t, bla, m60) || t[0] == ex || held(t)) continue;
        const auto m = std::find_if(c.meshes.begin(), c.meshes.end(),
                                    [&](const Key& k) { return eq_under(t, k, ma0) && k[0] != ex; });
        if (m == c.meshes.end()) continue;
        add(*m, t);
    }
    return count;
}

void agent_create_copy(const AgentPartCache& c, AgentCreateModel& dst, const AgentCreateModel& src) {
    std::array<std::uint8_t, 16> skel;
    std::memcpy(skel.data(), dst.packing.data() + 0x30, 16);
    const AgentPartOrder order = dst.order;  // +2e10 / +2e30: past the 0x2df0 bytes copied
    dst = src;
    dst.order = order;
    agent_apply_scale_preset(c, dst, dst.preset);
    std::memcpy(dst.packing.data() + 0x30, skel.data(), 16);
}

AgentCreate agent_create_open(const AgentPartCache& c, const AgentPacking& male, const AgentPacking& female,
                              const AgentCreateTable& table) {
    AgentCreate a;
    a.table = table;
    for (std::size_t s = 0; s < 4; ++s) {
        auto& m = a.models[s];
        m.packing = (s & 1) ? female : male;
        m.pair_count = agent_add_blank_pairs(c, m.packing, agent_pair_count(m.packing));
        m.order = agent_reorder_parts(c, m.packing);  // 0028fb28
        m.pair_count = m.order.count;
        m.sex = agent_packing_sex(m.packing);
        m.tone = agent_packing_tone(m.packing);
        const char* skel = m.sex == 0 ? "FEMALE.SKEL" : "MALE.SKEL";
        std::memcpy(m.packing.data() + 0x30, skel, std::strlen(skel) + 1);
        agent_apply_scale_preset(c, m, -1);
    }
    a.cur = 0;
    a.sex = a.models[0].sex;
    a.tone = a.models[0].tone;
    // The pairs: [0..10] from the female model (+64), [11..21] from the male (+60).
    for (std::size_t half = 0; half < 2; ++half) {
        const auto& m = a.models[half == 0 ? 1 : 0];
        for (std::size_t k = 0; k < 11; ++k)
            for (std::size_t v = 0; v <= m.pair_count; ++v)
                if (std::memcmp(m.packing.data() + 0x50 + 0x20 * v + 1, category_names[k] + 1, 4) == 0) {
                    a.table[half * 11 + k].pair = static_cast<std::int8_t>(v);
                    break;
                }
    }
    for (std::size_t i = 0; i < 22; ++i) a.table[i].position = 0;
    find_head(a);
    const auto& e = entry(a, a.sex, a.category, 0x28f760);
    a.pair = e.pair;
    a.type = e.type;
    a.mesh_index = find_mesh(c, mesh_of(a.model().packing, a.pair, 0x28f760));
    a.texture_index = find_tex(c, tex_of(a.model().packing, a.pair, 0x28f760));
    agent_create_count(c, a);
    a.camera = agent_camera_initial(c);  // 002903fc
    a.transition = 0;
    return a;
}

void agent_create_count(const AgentPartCache& c, AgentCreate& a) {
    std::int32_t r = -1;
    if (a.category >= 11) {
        a.position = 0;
        return;
    }
    const auto& p = a.model().packing;
    a.sex = agent_packing_sex(p);
    a.tone = agent_packing_tone(p);
    auto& e = entry(a, a.sex, a.category, 0x28f220);
    a.pair = e.pair;
    a.type = e.type;
    const auto mesh = [&] { return mesh_of(p, a.pair, 0x28f220); };
    const auto tex = [&] { return tex_of(p, a.pair, 0x28f220); };
    switch (a.type) {
    case 0: a.count = agent_count_meshes(c, mesh(), r, a.sex, false); break;
    case 7: a.count = agent_count_meshes(c, mesh(), r, a.sex, true); break;
    case 1: a.count = agent_count_textures(c, merge(tex(), mesh(), agent_mask(AgentMask::m60)), r, a.sex, a.tone, 2); break;
    case 2: a.count = agent_count_textures(c, tex(), r, a.sex, a.tone, 0); break;
    case 8: a.count = agent_count_textures(c, tex(), r, a.sex, a.tone, 1); break;
    case 3: a.count = 100; break;
    case 4: a.count = 2; break;
    case 5: a.count = static_cast<std::int32_t>(c.bone_presets.size()); break;
    case 6: a.count = agent_morph_count(c, a.sex); break;
    default: break;
    }
    if (r != -1) e.position = static_cast<std::uint16_t>(r);
    a.position = static_cast<std::int16_t>(e.position);
}

void agent_create_select(const AgentPartCache& c, AgentCreate& a, std::string_view name) {
    std::int8_t cat = 0;
    while (cat < 11 && name != agent_create_categories[static_cast<std::size_t>(cat)]) ++cat;
    a.previous = a.category;
    if (cat == a.category) return;
    a.category = cat;
    a.transition = 1;
    if (cat >= 11) {
        a.count = 0;
        a.position = 0;
        a.slider_shown = false;
        a.slot_text.clear();
        return;
    }
    const auto& e = entry(a, a.sex, cat, 0x28d610);
    a.pair = e.pair;
    a.type = e.type;
    a.mesh_index = find_mesh(c, mesh_of(a.model().packing, a.pair, 0x28d610));
    a.texture_index = find_tex(c, tex_of(a.model().packing, a.pair, 0x28d610));
    agent_create_count(c, a);
    a.slider_shown = true;
    show_position(a);
}

void agent_create_step(const AgentPartCache& c, AgentCreate& a, bool back, bool forward) {
    read_sex_tone(a);
    if ((!back && !forward) || a.category >= 11 || a.count == 0) return;
    const std::int32_t dir = back ? -1 : 1;
    auto& m = a.model();
    const auto mesh = [&] { return mesh_of(a.model().packing, a.pair, 0x28c390); };
    const auto tex = [&] { return tex_of(a.model().packing, a.pair, 0x28c390); };
    switch (a.type) {
    case 0:
    case 7: {
        const auto r = a.type == 0
            ? agent_mesh_step(c, dir, mesh(), a.mesh_index, a.pair, a.sex, a.tone, m.packing, m.pair_count)
            : agent_mesh_step4(c, dir, mesh(), a.mesh_index, a.pair, a.sex, a.tone, m.packing);
        if (r != -1) {
            a.mesh_index = r;
            a.texture_index = find_tex(c, tex());
        }
        break;
    }
    case 1:
    case 2: {
        const auto r = a.pair < static_cast<std::int64_t>(m.pair_count)
            ? agent_colour_step(c, dir, mesh(), tex(), a.texture_index, a.pair, a.sex, m.packing, m.pair_count)
            : agent_colour_step(c, dir, make_none, tex(), a.texture_index, a.pair, a.sex, m.packing, m.pair_count);
        if (r != -1) a.texture_index = r;
        break;
    }
    case 3: agent_create_randomize(c, a, dir, true, a.timer); break;
    case 4: {
        a.gender = (a.gender + 1) & 1;
        a.cur = a.gender ? 1 : 0;
        if (!(a.pair < static_cast<std::int64_t>(a.model().pair_count))) a.pair = 0;
        find_head(a);
        agent_apply_scale_preset(c, a.model(), -1);
        a.mesh_index = find_mesh(c, mesh());
        a.texture_index = find_tex(c, tex());
        read_sex_tone(a);
        break;
    }
    case 5: {
        const auto n = static_cast<std::int32_t>(c.bone_presets.size());
        std::int32_t p = dir + m.preset;
        if (p < 0) p = n - 1;
        if (static_cast<std::uint32_t>(p) >= static_cast<std::uint32_t>(n)) p = 0;
        agent_apply_scale_preset(c, m, p);
        break;
    }
    case 6: a.morph = agent_morph_step(c, m, dir, a.morph, a.sex); break;
    case 8: {
        const auto r = agent_texture_step(c, dir, mesh(), tex(), a.texture_index, a.pair, a.sex, a.tone, m.packing);
        if (r != -1) a.texture_index = r;
        break;
    }
    default: break;
    }
    const auto old = a.position;
    a.position += dir;
    if (a.position < 0) a.position = a.count - 1;
    if (a.position >= a.count) a.position = 0;
    entry(a, a.sex, a.category, 0x28c390).position = static_cast<std::uint16_t>(a.position);
    if (a.position != old) ++a.changes;
    show_position(a);
    read_sex_tone(a);
}

void agent_create_random_look(const AgentPartCache& c, AgentCreate& a, std::size_t index, std::int32_t dir,
                              std::uint32_t seed) {
    if (index >= a.models.size()) fail(0x28cde0, "model " + std::to_string(index));
    a.random = seed;   // srand(T0_COUNT) 00127db8 (PORT-ONLY: the caller's seed)
    auto& model = a.models[index];
    agent_create_copy(c, model, a.model());
    const std::int32_t sex = static_cast<std::uint8_t>(model.sex);
    for (std::int32_t k = 2; k < 11; ++k) {
        const std::int32_t n = static_cast<std::int32_t>(agent_random_next(a.random) & 3) + 1;
        const auto& e = entry(a, sex, k, 0x28cde0);
        const std::int32_t pair = static_cast<std::uint8_t>(e.pair);   // lbu: 0xff is pair 255
        a.tone = agent_packing_tone(model.packing);
        model.tone = a.tone;
        auto& p = model.packing;
        switch (e.type) {
        case 0:
        case 7: {
            std::int32_t start = find_mesh_m80(c, mesh_of(p, pair, 0x28cde0));
            for (std::int32_t i = 0; i < n; ++i) {
                const auto r = e.type == 0
                    ? agent_mesh_step(c, dir, mesh_of(p, pair, 0x28cde0), start, pair, sex, model.tone, p, model.pair_count)
                    : agent_mesh_step4(c, dir, mesh_of(p, pair, 0x28cde0), start, pair, sex, model.tone, p);
                if (r != -1) start = r;
            }
            break;
        }
        case 1: {
            std::int32_t start = find_tex(c, tex_of(p, pair, 0x28cde0));
            for (std::int32_t i = 0; i < n; ++i) {
                const auto mk = pair < static_cast<std::int64_t>(model.pair_count) ? mesh_of(p, pair, 0x28cde0) : make_none;
                const auto r = agent_colour_step(c, dir, mk, tex_of(p, pair, 0x28cde0), start, pair, sex, p, model.pair_count);
                if (r != -1) start = r;
            }
            break;
        }
        case 5: {
            const auto N = static_cast<std::int32_t>(c.bone_presets.size());
            std::int32_t v = model.preset + n * dir;
            if (v < 0) v = N - v;
            if (v < 0) v = 0;
            if (static_cast<std::uint32_t>(v) >= static_cast<std::uint32_t>(N)) v -= N;
            if (static_cast<std::uint32_t>(v) >= static_cast<std::uint32_t>(N)) v = 0;
            model.preset = static_cast<std::int8_t>(v);
            agent_apply_scale_preset(c, model, v);
            break;
        }
        case 6:
            for (std::int32_t i = 0; i < n; ++i) a.morph = agent_morph_step(c, model, dir, a.morph, sex);
            break;
        case 8: {
            std::int32_t start = find_tex(c, tex_of(p, pair, 0x28cde0));
            for (std::int32_t i = 0; i < n; ++i) {
                const auto r = agent_texture_step(c, dir, mesh_of(p, pair, 0x28cde0), tex_of(p, pair, 0x28cde0), start,
                                                  pair, sex, model.tone, p);
                if (r != -1) start = r;
            }
            break;
        }
        default: break;   // type 2 (makeup, beard) and the rest: untouched
        }
    }
}

void agent_create_randomize(const AgentPartCache& c, AgentCreate& a, std::int32_t dir, bool step, std::uint32_t seed) {
    a.sex = agent_packing_sex(a.model().packing);
    a.tone = agent_packing_tone(a.model().packing);
    if (step) {
        if (dir == 1) {
            agent_create_copy(c, a.models[2], a.model());
            agent_create_copy(c, a.model(), a.models[3]);
            agent_create_random_look(c, a, 3, 1, seed);
        } else {
            agent_create_copy(c, a.models[3], a.model());
            agent_create_copy(c, a.model(), a.models[2]);
            agent_create_random_look(c, a, 2, dir, seed);
        }
    } else {
        agent_create_random_look(c, a, 2, -1, seed);
        agent_create_random_look(c, a, 3, 1, seed + 1);   // PORT-ONLY: T0_COUNT has moved on by the second call
    }
    a.sex = agent_packing_sex(a.model().packing);
    a.tone = agent_packing_tone(a.model().packing);
}

}  // namespace omega
