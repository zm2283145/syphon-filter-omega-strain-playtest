#include "population.hpp"
#include "binary.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef __vita__
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#endif

namespace omega {
namespace {
struct Cursor {
    std::span<const std::uint8_t> bytes;
    std::size_t pos, end;
    std::uint32_t word() {
        if (pos > end || end - pos < 4) throw std::runtime_error("Truncated population record");
        const auto value = read_u32(bytes, pos); pos += 4; return value;
    }
    std::uint32_t key() {
        const auto category = word(), index = word();
        if (category > 65535 || index > 65535) throw std::runtime_error("Population ID exceeds packed range");
        return (category << 16) | index;
    }
    float scalar() {
        const auto value = std::bit_cast<float>(word());
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite population coordinate");
        return value;
    }
    Vec3 point() { return {scalar(), scalar(), scalar()}; }
    std::string name(bool asset = true) {
        std::string value;
        while (pos < end && value.size() < 256) {
            const auto c = bytes[pos++];
            if (!c) {
                pos = (pos + 3) & ~std::size_t(3);
                if (pos > end) throw std::runtime_error("Truncated population name alignment");
                if (asset && (value.empty() || value.find("..") != std::string::npos))
                    throw std::runtime_error("Invalid population asset name");
                return value;
            }
            if (c < 32 || c > 126 || (asset && (c == '/' || c == '\\' || c == ':')))
                throw std::runtime_error("Invalid population asset name");
            value += static_cast<char>(c);
        }
        throw std::runtime_error("Unterminated population name");
    }
    std::vector<std::array<std::uint32_t, 2>> pairs() {
        const auto count = word();
        if (count > (end - pos) / 8) throw std::runtime_error("Population reference count exceeds extent");
        std::vector<std::array<std::uint32_t, 2>> values;
        for (std::uint32_t i = 0; i < count; ++i) values.push_back({word(), word()});
        return values;
    }
    void finish() const {
        if (pos != end) throw std::runtime_error("Population section has unconsumed bytes");
    }
};
void quoted(std::ostream& out, const std::string& s) {
    out << '"';
    for (const char c : s) { if (c == '"' || c == '\\') out << '\\'; out << c; }
    out << '"';
}
void point_json(std::ostream& out, Vec3 v) { out << '[' << v.x << ',' << v.y << ',' << v.z << ']'; }
void pairs_json(std::ostream& out, const std::vector<std::array<std::uint32_t, 2>>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << '[' << values[i][0] << ',' << values[i][1] << ']';
    }
    out << ']';
}
}

Population decode_population(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes.size() % 4 || read_u32(bytes, 0) != 70)
        throw std::runtime_error("Only aligned POP version 70 is supported");
    const std::set<std::string> tags{"TER", "GOB", "SND", "ACT", "ACL", "INL", "NPC", "WPN", "PLR",
        "SKY", "PNT", "DIR", "ENV", "NOD", "GEN", "GRP", "BOX", "FIR", "CAM", "NIS", "NIE", "NIL", "INV", "BUG"};
    Population result;
    std::set<std::string> seen;
    for (std::size_t pos = 4; pos + 8 <= bytes.size(); pos += 4) {
        if (bytes[pos + 3] != ':') continue;
        const std::string tag(reinterpret_cast<const char*>(bytes.data() + pos), 3);
        if (!tags.contains(tag)) continue;
        if (!seen.insert(tag).second) throw std::runtime_error("Ambiguous duplicate population section");
        if (tag == "ACT") throw std::runtime_error("Older ACT adjacency layout is unsupported");
        result.sections.push_back({tag, pos, bytes.size(), read_u32(bytes, pos + 4), false});
    }
    for (std::size_t i = 1; i < result.sections.size(); ++i)
        result.sections[i - 1].end = result.sections[i].offset;
    auto section = [&](const std::string& tag, std::size_t minimum) -> PopulationSection& {
        auto found = std::find_if(result.sections.begin(), result.sections.end(), [&](const auto& s) { return s.tag == tag; });
        if (found == result.sections.end()) throw std::runtime_error("Required population section missing: " + tag);
        if (found->count > (found->end - found->offset - 8) / minimum)
            throw std::runtime_error("Population section count exceeds extent");
        found->decoded = true;
        return *found;
    };
    auto& ter = section("TER", 12);
    Cursor t{bytes, ter.offset + 8, ter.end};
    std::set<std::uint32_t> keys;
    for (std::uint32_t i = 0; i < ter.count; ++i) {
        const auto key = t.key();
        if (!keys.insert(key).second) throw std::runtime_error("Duplicate terrain ID");
        auto name = t.name();
        auto extension = name.size() >= 4 ? name.substr(name.size() - 4) : "";
        for (char& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (extension != ".vum") throw std::runtime_error("Unsupported terrain model extension");
        result.terrain.push_back({key, std::move(name)});
    }
    t.finish();
    auto& acl = section("ACL", 4);
    Cursor a{bytes, acl.offset + 8, acl.end};
    for (std::uint32_t i = 0; i < acl.count; ++i) {
        const auto words = a.word();
        if (words % 2 || words > (a.end - a.pos) / 4) throw std::runtime_error("Invalid adjacency pair count");
        std::vector<std::uint32_t> group;
        for (std::uint32_t j = 0; j < words / 2; ++j) group.push_back(a.key());
        result.adjacency.push_back(std::move(group));
    }
    a.finish();
    auto& inl = section("INL", 36);
    Cursor l{bytes, inl.offset + 8, inl.end};
    std::set<std::uint32_t> linked;
    for (std::uint32_t i = 0; i < inl.count; ++i) {
        const auto key = l.key(), group = l.word();
        const Bounds bounds{l.point(), l.point()};
        if (!keys.contains(key) || !linked.insert(key).second || group >= result.adjacency.size())
            throw std::runtime_error("Invalid terrain adjacency link");
        if (bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y || bounds.minimum.z > bounds.maximum.z)
            throw std::runtime_error("Inverted population bounds");
        result.links.push_back({key, group, bounds});
    }
    l.finish();
    if (linked != keys) throw std::runtime_error("Terrain bounds do not cover definitions");
    auto& gob = section("GOB", 76);
    Cursor g{bytes, gob.offset + 8, gob.end};
    std::set<std::uint32_t> object_ids;
    for (std::uint32_t i = 0; i < gob.count; ++i) {
        PopulationObject item;
        item.kind = g.word(); item.id = g.key();
        if (item.kind < 1 || item.kind > 6 || !object_ids.insert(item.id).second)
            throw std::runtime_error("Unsupported object kind or duplicate object ID");
        item.asset = g.name(); item.script = g.name(false);
        auto extension = item.asset.size() >= 4 ? item.asset.substr(item.asset.size() - 4) : "";
        for (char& ch : extension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (item.asset != "no_vum" && extension != ".vum")
            throw std::runtime_error("Unsupported object model extension");
        item.position = g.point();
        for (auto& v : item.basis) v = g.scalar();
        item.associations = g.pairs(); item.references = g.pairs();
        if (item.kind == 2 || item.kind == 3) {
            item.motion.mode = g.word(); item.motion.speed = g.scalar(); item.motion.parameter = g.word();
            const auto count = g.word();
            if (count > (g.end - g.pos) / 28) throw std::runtime_error("Population motion count exceeds extent");
            for (std::uint32_t j = 0; j < count; ++j) item.motion.points.push_back(g.point());
            for (std::uint32_t j = 0; j < count; ++j)
                item.motion.rotations.push_back({g.scalar(), g.scalar(), g.scalar(), g.scalar()});
        }
        if (item.kind == 3) item.extra_references = g.pairs();
        result.objects.push_back(std::move(item));
    }
    g.finish();
    auto& plr = section("PLR", 68);
    Cursor p{bytes, plr.offset + 8, plr.end};
    std::set<std::uint32_t> placed;
    for (std::uint32_t i = 0; i < plr.count; ++i) {
        PopulationPlacement item;
        item.id = p.key(); item.parent = {p.word(), p.word()}; item.name = p.name(); item.position = p.point();
        if (!placed.insert(item.id).second) throw std::runtime_error("Duplicate placement ID");
        for (auto& v : item.basis) v = p.scalar();
        result.placements.push_back(std::move(item));
    }
    p.finish();
    auto& nod = section("NOD", 72);
    Cursor n{bytes, nod.offset + 8, nod.end};
    std::set<std::uint32_t> node_ids;
    for (std::uint32_t i = 0; i < nod.count; ++i) {
        PopulationNode item;
        item.id = n.key(); item.parent_index = std::bit_cast<std::int32_t>(n.word()); item.flags = n.word();
        item.position = n.point();
        for (auto& v : item.basis) v = n.scalar();
        item.parameter = n.word(); item.links = n.pairs();
        if (!node_ids.insert(item.id).second) throw std::runtime_error("Duplicate node ID");
        result.nodes.push_back(std::move(item));
    }
    n.finish();
    auto& npc = section("NPC", 84);
    Cursor actor{bytes, npc.offset + 8, npc.end};
    std::set<std::uint32_t> npc_ids;
    for (std::uint32_t i = 0; i < npc.count; ++i) {
        PopulationNpc item;
        item.id = actor.key(); item.model = actor.name(); item.extra_name = actor.name(false);
        item.parameters_offset = actor.pos;
        item.parameters = {actor.word(), actor.word()}; item.position = actor.point();
        for (auto& v : item.basis) v = actor.scalar();
        item.trailing_parameters = {actor.word(), actor.word()};
        item.node_index = std::bit_cast<std::int32_t>(actor.word());
        if (!npc_ids.insert(item.id).second) throw std::runtime_error("Duplicate NPC ID");
        if (item.node_index >= 0 && static_cast<std::size_t>(item.node_index) >= result.nodes.size())
            throw std::runtime_error("NPC node index exceeds node table");
        result.npcs.push_back(std::move(item));
    }
    actor.finish();
    // 001c0c00: optional BOX: section (the loader skips it without the tag).
    for (auto& s : result.sections) {
        const auto body = bytes.subspan(s.offset + 8, s.end - s.offset - 8);
        if (s.tag == "BOX") result.hotboxes = decode_population_hotboxes(body, 70, s.count);
        // 001c1770 / 001c12a0: light and environment records (LEVEL_LIGHTS.md).
        else if (s.tag == "PNT") result.point_lights = decode_population_point_lights(body, 70, s.count);
        else if (s.tag == "DIR") result.directional_lights = decode_population_directional_lights(body, 70, s.count);
        else if (s.tag == "ENV") result.environments = decode_population_environments(body, 70, s.count);
        // 001c2ae0 / 001c2a10 (00257480 per record): generators and backpacks.
        else if (s.tag == "GEN") result.generators = decode_population_generators(body, 70, s.count);
        else if (s.tag == "INV") result.backpacks = decode_population_backpacks(body, 70, s.count);
        // 001c0b00 / 002625b0: fire zones.
        else if (s.tag == "FIR") result.fires = decode_population_fires(body, s.count);
        // 001c0990 / 0017e3f0: the level cameras (GameCamera, CAMERA.md).
        else if (s.tag == "CAM") result.cameras = decode_population_cameras(body, s.count);
        // 001c30a4 / 00237f80 and 001c323c / 0023c610: event lights and objects.
        else if (s.tag == "NIL") result.event_lights = decode_population_event_lights(body, 70, s.count);
        else if (s.tag == "NIE") result.event_objects = decode_population_event_objects(body, 70, s.count);
        else if (s.tag == "BUG") result.debug_names = decode_population_debug_names(body, s.count);
        // 002390a0: the event objects' sound names.
        else if (s.tag == "NIS") result.sound_names = decode_population_sound_names(body, 70, s.count);
        // 001c3390 / 00212ee0: the sound objects.
        else if (s.tag == "SND") result.sound_objects = decode_population_sound_objects(body, 70, s.count);
        else continue;
        s.decoded = true;
    }
    return result;
}

namespace {
// The light readers' records consist of whole words and byte fields padded to
// 4 bytes (the stream cursor is realigned after each byte; sections start on
// 4-byte boundaries).
std::size_t record_count(std::span<const std::uint8_t> bytes, std::uint32_t count, std::size_t size, const char* what) {
    // 001c1a20 passes the count to the pool allocator 00374270 unchecked; a
    // negative count is not supported.
    if (static_cast<std::int32_t>(count) < 0) throw std::runtime_error(std::string(what) + " negative record count is unsupported");
    if (count == 0) {
        if (!bytes.empty()) throw std::runtime_error(std::string(what) + " section has bytes after an empty record count");
        return 0;
    }
    if (count > bytes.size() / size) throw std::runtime_error(std::string(what) + " record count exceeds the section");
    if (bytes.size() != std::size_t(count) * size) throw std::runtime_error(std::string(what) + " section has unconsumed bytes");
    return count;
}
// Variable-size records: `minimum` bounds the count; the cursor checks the end.
std::size_t variable_record_count(std::span<const std::uint8_t> bytes, std::uint32_t count, std::size_t minimum,
                                  const char* what) {
    if (static_cast<std::int32_t>(count) < 0) throw std::runtime_error(std::string(what) + " negative record count is unsupported");
    if (count > bytes.size() / minimum) throw std::runtime_error(std::string(what) + " record count exceeds the section");
    return count;
}
std::uint8_t padded_byte(Cursor& c) { return static_cast<std::uint8_t>(c.word() & 0xffU); }
}

std::vector<PopulationPointLight> decode_population_point_lights(std::span<const std::uint8_t> bytes,
                                                                 std::uint32_t version, std::uint32_t count) {
    const std::size_t size = version >= 0x3b ? 88 : 80;                 // 001797f8: aux bytes from 0x3b
    const auto n = record_count(bytes, count, size, "PNT:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationPointLight> out(n);
    for (auto& l : out) {
        l.id_category = c.word(); l.id_index = c.word();
        l.skipped = {c.word(), c.word()}; l.unused = c.word();          // 001796cc..001796e8, 0x24-byte copy from +16
        for (auto& v : l.position) v = c.scalar();
        for (auto& v : l.colour) v = c.scalar();
        l.inner = c.scalar(); l.outer = c.scalar();
        for (auto& v : l.direction) v = c.scalar();
        l.cone_angle_outer = c.scalar(); l.cone_angle_inner = c.scalar();
        l.enable = padded_byte(c);
        if (version >= 0x3b) { l.aux_08 = padded_byte(c); l.aux_10 = padded_byte(c); }
        l.intensity = c.scalar();
    }
    c.finish();
    return out;
}

std::vector<PopulationDirectionalLight> decode_population_directional_lights(std::span<const std::uint8_t> bytes,
                                                                             std::uint32_t, std::uint32_t count) {
    const auto n = record_count(bytes, count, 44, "DIR:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationDirectionalLight> out(n);
    for (auto& l : out) {
        l.id_category = c.word(); l.id_index = c.word(); l.unused = c.word();
        for (auto& v : l.direction) v = c.scalar();
        for (auto& v : l.colour) v = c.scalar();
        l.enable = padded_byte(c);
        l.intensity = c.scalar();
    }
    c.finish();
    return out;
}

std::vector<PopulationEnvironment> decode_population_environments(std::span<const std::uint8_t> bytes,
                                                                  std::uint32_t version, std::uint32_t count) {
    const std::size_t size = 68 + (version >= 0x37 ? 4 : 0) + (version >= 0x3a ? 4 : 0);
    const auto n = record_count(bytes, count, size, "ENV:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationEnvironment> out(n);
    for (auto& e : out) {
        e.id_category = c.word(); e.id_index = c.word();
        for (auto& v : e.values) v = c.scalar();
        for (auto& r : e.references) r = c.word();
        e.fog_mode = padded_byte(c);
        e.value_80 = version >= 0x37 ? c.scalar() : std::bit_cast<float>(0x3ee66666U);   // 001788d0..00178908
        e.search = version >= 0x3a ? padded_byte(c) : std::uint8_t{1};                     // 00178944..00178998
    }
    c.finish();
    return out;
}

std::vector<PopulationGenerator> decode_population_generators(std::span<const std::uint8_t> bytes,
                                                              std::uint32_t version, std::uint32_t count) {
    if (static_cast<std::int32_t>(count) < 0) throw std::runtime_error("GEN: negative record count is unsupported");
    if (count > bytes.size() / 100) throw std::runtime_error("GEN: record count exceeds the section");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationGenerator> out(count);
    for (auto& g : out) {
        g.id = c.key(); g.manifest = c.name(false); g.health = c.word();
        g.position = c.point();
        for (auto& v : g.basis) v = c.scalar();
        g.items = {c.word(), c.word()};
        const auto words = c.word();
        if (words % 2 || words > (c.end - c.pos) / 4) throw std::runtime_error("GEN: invalid spawn node word count");
        for (std::uint32_t i = 0; i < words / 2; ++i) g.spawn_nodes.push_back(c.key());
        g.alive_cap = c.word(); g.total = c.word(); g.interval = c.scalar();
        if (version >= 0x3e) g.flag_d2 = padded_byte(c);
        g.enabled = padded_byte(c);
        g.parent_region = std::bit_cast<std::int32_t>(c.word());
        g.name = c.name(false);
    }
    c.finish();
    return out;
}

std::vector<PopulationBackpack> decode_population_backpacks(std::span<const std::uint8_t> bytes,
                                                            std::uint32_t version, std::uint32_t count) {
    if (version < 0x39) throw std::runtime_error("INV: records below version 0x39 are unsupported");
    const auto n = record_count(bytes, count, 84, "INV:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationBackpack> out(n);
    for (auto& b : out) {
        b.id = c.key(); b.position = c.point();
        for (auto& slot : b.slots) slot = {c.word(), c.word(), c.word(), padded_byte(c)};
        b.pair_2e4 = {c.word(), c.word()};
        b.owner = {c.word(), c.word()};
    }
    c.finish();
    return out;
}

std::vector<PopulationFire> decode_population_fires(std::span<const std::uint8_t> bytes, std::uint32_t count) {
    const auto n = variable_record_count(bytes, count, 40, "FIR:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationFire> out(n);
    for (auto& f : out) {
        f.id = c.key(); f.first = c.point(); f.second = c.point(); f.value = c.scalar();
        f.links = c.pairs();
    }
    c.finish();
    return out;
}

std::vector<PopulationCamera> decode_population_cameras(std::span<const std::uint8_t> bytes, std::uint32_t count) {
    const auto n = record_count(bytes, count, 56, "CAM:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationCamera> out(n);
    for (auto& camera : out) {
        camera.id = c.key();                                 // 0017e990..0017e9d0: (w0 << 16) | w1 -> +c
        camera.position = c.point();                         // sp+230 (w = 1.0)
        for (auto& row : camera.rows) row = c.point();       // sp+260 / +250 / +240 (w = 0)
    }
    c.finish();
    return out;
}

std::vector<PopulationSoundObject> decode_population_sound_objects(std::span<const std::uint8_t> bytes,
                                                                   std::uint32_t version, std::uint32_t count) {
    // 001c33d0: a zero count reads nothing more (the section's realignment follows the loop).
    const auto n = variable_record_count(bytes, count, 76, "SND:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationSoundObject> out(n);
    for (auto& s : out) {
        s.flag = c.word();                                                   // 001c3410
        s.id = c.key();                                                      // 00212f98..00212fb4, +c at 00213240
        s.name = c.name(false);                                              // 00212fc0..00213004
        s.script = c.name(false);                                            // 00213010..00213108
        s.position = c.point();                                              // 00213128..00213160
        for (auto& v : s.basis) v = c.scalar();                              // 00213164..00213224
        // 00213244..00213460: a signed count; none for count <= 0; the words go to two 0x400-byte stack arrays.
        const auto pairs = static_cast<std::int32_t>(c.word());
        if (pairs > 256) throw std::runtime_error("SND: more than 256 association pairs overflow 00212ee0's stack arrays");
        if (pairs > 0 && std::size_t(pairs) > (c.end - c.pos) / 8) throw std::runtime_error("SND: association count exceeds extent");
        for (std::int32_t i = 0; i < pairs; ++i) s.associations.push_back({c.word(), c.word()});
        if (c.pos >= c.end) throw std::runtime_error("Truncated population record");
        s.owned_1f2 = bytes[c.pos++];                                        // 00213470..00213480, then realigned
        c.pos = (c.pos + 3) & ~std::size_t(3);
        if (c.pos > c.end) throw std::runtime_error("Truncated population record");
        if (version >= 0x35) s.type = std::bit_cast<std::int32_t>(c.word()); // 002134b0..002134cc
    }
    c.finish();
    return out;
}

std::vector<PopulationDebugName> decode_population_debug_names(std::span<const std::uint8_t> bytes, std::uint32_t count) {
    const auto n = variable_record_count(bytes, count, 12, "BUG:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationDebugName> out(n);
    for (auto& d : out) { d.id = c.key(); d.name = c.name(false); }
    c.finish();
    return out;
}

PopulationSoundNames decode_population_sound_names(std::span<const std::uint8_t> bytes, std::uint32_t version,
                                                   std::uint32_t count) {
    const auto n = variable_record_count(bytes, count, version >= 0x43 ? 8 : 4, "NIS:");
    Cursor c{bytes, 0, bytes.size()};
    PopulationSoundNames out;
    for (std::size_t i = 0; i < n; ++i) {
        out.names.push_back(c.name(false));
        if (version < 0x43) { out.categories.push_back(0); continue; }
        const std::uint32_t category = c.word();
        out.categories.push_back(category);
        if (out.first_category == -1) out.first_category = static_cast<std::int32_t>(category);
        if (static_cast<std::int32_t>(category) == out.first_category) ++out.base;
    }
    c.finish();
    return out;
}

namespace {
// 00239df0 and the inline 10-/2-float readers: a key count word, then (when
// nonzero) the length float, then `width` floats per key.
PopulationTrack read_track(Cursor& c, std::size_t width) {
    PopulationTrack t{width};
    const auto keys = c.word();
    if (!keys) return t;
    // The length float, then `width` floats per key (the earlier bound also
    // charged one extra word per key and rejected valid tracks near a record's end).
    const std::size_t words = (c.end - c.pos) / 4;
    if (words < 1 || keys > (words - 1) / width) throw std::runtime_error("Population track key count exceeds extent");
    t.length = c.scalar();
    t.values.reserve(std::size_t(keys) * width);
    for (std::size_t i = 0; i < std::size_t(keys) * width; ++i) t.values.push_back(c.scalar());
    return t;
}
// 00238a50 reads counts and types as floats cast to int.
std::int32_t float_count(Cursor& c, const char* what) {
    const auto v = c.scalar();
    if (v < 0.0f || v > 1.0e6f) throw std::runtime_error(std::string(what) + " invalid float count");
    return static_cast<std::int32_t>(v);
}
}

std::vector<PopulationEventLight> decode_population_event_lights(std::span<const std::uint8_t> bytes,
                                                                 std::uint32_t version, std::uint32_t count) {
    // 00237f80 reads one float per light track via 00239cf0 below 0x46.
    if (version < 0x46) throw std::runtime_error("NIL: records below version 0x46 are unsupported");
    const auto n = variable_record_count(bytes, count, 68, "NIL:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationEventLight> out(n);
    for (auto& l : out) {
        l.id = c.key();
        for (auto& t : l.tracks) t = read_track(c, 4);
        for (auto& v : l.values_144) v = c.scalar();
        if (version >= 0x42) { l.byte_151 = padded_byte(c); l.byte_150 = padded_byte(c); }
        if (version >= 0x45) for (auto& v : l.values_160) v = c.scalar();
        l.keyframes = read_track(c, 10);
        l.switches = read_track(c, 2);
        l.links = c.pairs();
    }
    c.finish();
    return out;
}

std::vector<PopulationEventObject> decode_population_event_objects(std::span<const std::uint8_t> bytes,
                                                                   std::uint32_t version, std::uint32_t count) {
    const auto n = variable_record_count(bytes, count, 40, "NIE:");
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationEventObject> out(n);
    for (auto& o : out) {
        if (version >= 0x3f) o.group = c.name(false);
        o.id = c.key();
        o.kind = c.word();
        if (o.kind > 6) throw std::runtime_error("NIE: unsupported object kind");   // 004a6fc0 has 7 entries
        if (o.kind) o.asset = c.name(false);
        o.keyframes = read_track(c, 10);
        for (auto& t : o.tracks) t = read_track(c, 4);
        o.switches = read_track(c, 2);
        if (version >= 0x38) {
            const auto events = float_count(c, "NIE: event");
            if (std::size_t(events) > (c.end - c.pos) / 4) throw std::runtime_error("NIE: event count exceeds extent");
            for (std::int32_t i = 0; i < events; ++i) {
                PopulationEvent e{float_count(c, "NIE: event type")};
                std::size_t floats = 0;
                switch (e.type) {
                case 0: case 3: floats = 1; break;
                case 1: floats = 5; break;
                case 2: case 4: floats = 3; break;                              // 002397f0 / 00239610
                default: throw std::runtime_error("NIE: unsupported event type");
                }
                for (std::size_t k = 0; k < floats; ++k) e.values.push_back(c.scalar());
                o.events.push_back(std::move(e));
            }
        }
        o.links = c.pairs();
        o.children = c.pairs();
        if (version >= 0x44) o.byte_130 = padded_byte(c);
    }
    c.finish();
    return out;
}

bool PopulationHotbox::starts_disabled() const {
    for (const auto& a : associations) if (a[0] == 0xffffffffU && a[1] == 0xffffffffU) return true;
    return false;
}

std::vector<PopulationHotbox> decode_population_hotboxes(std::span<const std::uint8_t> bytes,
                                                         std::uint32_t version, std::uint32_t count) {
    // 001c0c00 reads the count as a signed word and loops while i < count.
    if (static_cast<std::int32_t>(count) <= 0) {
        if (!bytes.empty()) throw std::runtime_error("Hotbox section has bytes after an empty record count");
        return {};
    }
    if (count > bytes.size() / 48) throw std::runtime_error("Hotbox record count exceeds the section"); // smallest record: 12 words
    Cursor c{bytes, 0, bytes.size()};
    std::vector<PopulationHotbox> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PopulationHotbox h;
        if (version >= 0x36) h.type = c.word();                         // 001c0cc0
        h.id_category = c.word(); h.id_index = c.word();
        h.id = (h.id_category << 16) | h.id_index;                      // 00173214
        const auto pairs = c.word();
        // 00172bd0 stores the pairs in two 256-word stack arrays (sp+0x550, sp+0x150).
        if (pairs > 256) throw std::runtime_error("Hotbox association count exceeds the reader's 256-entry buffers");
        if (pairs > (c.end - c.pos) / 8) throw std::runtime_error("Hotbox association count exceeds the section");
        for (std::uint32_t k = 0; k < pairs; ++k) h.associations.push_back({c.word(), c.word()});
        if (version < 0x36) h.type = c.word();                          // 00172c2c
        h.parameter = c.word();
        for (auto& v : h.minimum) v = c.scalar();
        for (auto& v : h.maximum) v = c.scalar();
        if (version >= 0x3d) {                                          // 00172cf0
            for (auto& v : h.basis) v = c.scalar();
            const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
            for (std::size_t k = 0; k < 9; ++k) if (!(h.basis[k] == identity[k])) h.transformed = true;
        }
        const auto words = c.word();
        if (words % 2) throw std::runtime_error("Hotbox link word count is odd");
        if (words / 2 > (c.end - c.pos) / 8) throw std::runtime_error("Hotbox link count exceeds the section");
        for (std::uint32_t k = 0; k < words / 2; ++k) h.links.push_back({c.word(), c.word()});
        out.push_back(std::move(h));
    }
    c.finish();
    return out;
}

float population_node_yaw(const PopulationNode& node) {
    const auto ftz = [](float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; };
    const float x = node.basis[3], y = node.basis[4];
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Node yaw: nonfinite basis row");
    const float sum = ftz(ftz(ftz(x * x) + ftz(y * y)) + 0.0F);
    const float q = sum == 0.0F ? std::numeric_limits<float>::max() : ftz(vu::rsqrt(sum));
    const float nx = ftz(x * q), ny = ftz(y * q);
    float a = game_acosf(ftz(ftz(nx * 0.0F) + ny) + 0.0F);
    const float pi = std::bit_cast<float>(0x40490fdbU);
    if (a != 0.0F && a != pi && !(nx < 0.0F)) a = ftz(pi + ftz(pi - a));
    if (static_cast<double>(a) >= 6.2) a = 0.0F;
    return a;
}

SpawnPose npc_spawn_pose(const Population& population, std::size_t index) {
    if (index >= population.npcs.size()) throw std::runtime_error("NPC spawn index is outside population");
    const auto& npc = population.npcs[index];
    SpawnPose pose{npc.id,npc.position,npc.basis,npc.node_index>=0};
    if (pose.node_override) {
        if (static_cast<std::size_t>(npc.node_index) >= population.nodes.size()) throw std::runtime_error("NPC spawn node is outside population");
        pose.position = population.nodes[npc.node_index].position;
    }
    if (!std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) || !std::isfinite(pose.position.z))
        throw std::runtime_error("Nonfinite NPC spawn position");
    for (std::size_t column = 0; column < 3; ++column) {
        const auto at = column * 3;
        const float x=pose.basis[at], y=pose.basis[at+1], z=pose.basis[at+2];
        const float length = std::sqrt((x*x+y*y)+z*z);
        if (!std::isfinite(length) || length <= 0) throw std::runtime_error("Invalid NPC spawn basis column");
        const float inverse = 1.F/length;
        for (std::size_t row = 0; row < 3; ++row) pose.basis[at+row] *= inverse;
    }
    return pose;
}

void write_population(std::ostream& out, const Population& p) {
    out << std::setprecision(9) << "{\"version\":70,\"terrain\":[";
    for (std::size_t i = 0; i < p.terrain.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << p.terrain[i].id << ",\"name\":"; quoted(out, p.terrain[i].name); out << '}';
    }
    out << "],\"adjacency\":[";
    for (std::size_t i = 0; i < p.adjacency.size(); ++i) {
        if (i) out << ',';
        out << '[';
        for (std::size_t j = 0; j < p.adjacency[i].size(); ++j) { if (j) out << ','; out << p.adjacency[i][j]; }
        out << ']';
    }
    out << "],\"links\":[";
    for (std::size_t i = 0; i < p.links.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.links[i];
        out << "{\"id\":" << v.id << ",\"adjacency\":" << v.adjacency << ",\"bounds\":[";
        point_json(out, v.bounds.minimum); out << ','; point_json(out, v.bounds.maximum); out << "]}";
    }
    out << "],\"objects\":[";
    for (std::size_t i = 0; i < p.objects.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.objects[i];
        out << "{\"kind\":" << v.kind << ",\"id\":" << v.id << ",\"asset\":"; quoted(out, v.asset);
        out << ",\"script\":"; quoted(out, v.script);
        out << ",\"position\":"; point_json(out, v.position); out << ",\"basis\":[";
        for (std::size_t j = 0; j < 9; ++j) { if (j) out << ','; out << v.basis[j]; }
        out << "],\"associations\":"; pairs_json(out, v.associations);
        out << ",\"references\":"; pairs_json(out, v.references);
        out << ",\"extra_references\":"; pairs_json(out, v.extra_references);
        out << ",\"motion\":";
        if (v.kind == 2 || v.kind == 3) {
            out << "{\"mode\":" << v.motion.mode << ",\"speed\":" << v.motion.speed
                << ",\"parameter\":" << v.motion.parameter << ",\"points\":[";
            for (std::size_t j = 0; j < v.motion.points.size(); ++j) {
                if (j) out << ',';
                point_json(out, v.motion.points[j]);
            }
            out << "],\"rotations\":[";
            for (std::size_t j = 0; j < v.motion.rotations.size(); ++j) {
                if (j) out << ',';
                out << '[';
                for (std::size_t k = 0; k < 4; ++k) { if (k) out << ','; out << v.motion.rotations[j][k]; }
                out << ']';
            }
            out << "]}";
        } else out << "null";
        out << '}';
    }
    out << "],\"placements\":[";
    for (std::size_t i = 0; i < p.placements.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.placements[i];
        out << "{\"id\":" << v.id << ",\"parent\":[" << v.parent[0] << ',' << v.parent[1] << "],\"name\":";
        quoted(out, v.name); out << ",\"position\":"; point_json(out, v.position); out << ",\"basis\":[";
        for (std::size_t j = 0; j < 9; ++j) { if (j) out << ','; out << v.basis[j]; } out << "]}";
    }
    out << "],\"npcs\":[";
    for (std::size_t i = 0; i < p.npcs.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.npcs[i];
        out << "{\"id\":" << v.id << ",\"model\":"; quoted(out,v.model);
        out << ",\"extra_name\":"; quoted(out,v.extra_name);
        out << ",\"parameters\":[" << v.parameters[0] << ',' << v.parameters[1] << "],\"position\":";
        point_json(out,v.position); out << ",\"basis\":[";
        for (std::size_t j = 0; j < 9; ++j) { if (j) out << ','; out << v.basis[j]; }
        out << "],\"trailing_parameters\":[" << v.trailing_parameters[0] << ',' << v.trailing_parameters[1]
            << "],\"node_index\":" << v.node_index << '}';
    }
    out << "],\"nodes\":[";
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.nodes[i];
        out << "{\"id\":" << v.id << ",\"parent_index\":" << v.parent_index << ",\"flags\":" << v.flags << ",\"position\":";
        point_json(out,v.position); out << ",\"basis\":[";
        for (std::size_t j = 0; j < 9; ++j) { if (j) out << ','; out << v.basis[j]; }
        out << "],\"parameter\":" << v.parameter << ",\"links\":"; pairs_json(out,v.links); out << '}';
    }
    out << "],\"sections\":[";
    for (std::size_t i = 0; i < p.sections.size(); ++i) {
        if (i) out << ',';
        const auto& v = p.sections[i];
        out << "{\"tag\":"; quoted(out, v.tag);
        out << ",\"offset\":" << v.offset << ",\"end\":" << v.end << ",\"count\":" << v.count
            << ",\"decoded\":" << (v.decoded ? "true" : "false") << '}';
    }
    out << "]}\n";
}

void apply_stored_pose(Mesh& mesh, CollisionMesh& collision, const PopulationObject& object) {
    const auto& b = object.basis;
    for (const auto value : b)
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite object basis");
    const auto t = object.position;
    if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z))
        throw std::runtime_error("Nonfinite object position");
    bool orthogonal = true;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const float dot = b[i*3]*b[j*3] + b[i*3+1]*b[j*3+1] + b[i*3+2]*b[j*3+2];
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.F : 0.F)) > 0.0001F)
                orthogonal = false;
        }
    }
    const float determinant = b[0]*(b[4]*b[8]-b[5]*b[7]) - b[3]*(b[1]*b[8]-b[2]*b[7]) + b[6]*(b[1]*b[5]-b[2]*b[4]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 0.00000001F)
        throw std::runtime_error("Singular object basis is unsupported");
    auto normal_basis = b;
    if (!orthogonal) {
        // Inverse transpose is a diagnostic lighting rule, not recovered original VU shading.
        normal_basis = {(b[4]*b[8]-b[5]*b[7])/determinant, (b[5]*b[6]-b[3]*b[8])/determinant, (b[3]*b[7]-b[4]*b[6])/determinant,
                        (b[7]*b[2]-b[8]*b[1])/determinant, (b[8]*b[0]-b[6]*b[2])/determinant, (b[6]*b[1]-b[7]*b[0])/determinant,
                        (b[1]*b[5]-b[2]*b[4])/determinant, (b[2]*b[3]-b[0]*b[5])/determinant, (b[0]*b[4]-b[1]*b[3])/determinant};
    }
    const auto transform = [&](Vec3 v, bool point) {
        // Three stored basis vectors are matrix columns; the fourth is translation.
        const auto& matrix = point ? b : normal_basis;
        Vec3 result{(matrix[0]*v.x + matrix[3]*v.y) + matrix[6]*v.z,
                    (matrix[1]*v.x + matrix[4]*v.y) + matrix[7]*v.z,
                    (matrix[2]*v.x + matrix[5]*v.y) + matrix[8]*v.z};
        if (point) { result.x += t.x; result.y += t.y; result.z += t.z; }
        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
            throw std::runtime_error("Nonfinite transformed object geometry");
        return result;
    };
    for (auto& vertex : mesh.vertices) {
        vertex.position = transform(vertex.position, true);
        vertex.normal = transform(vertex.normal, false);
    }
    for (auto& vertex : collision.vertices) vertex = transform(vertex, true);
    for (auto& material : mesh.materials) {
        material.model_up = transform(material.model_up, false);
        material.rigid_pose = material.rigid_pose && orthogonal && determinant > 0;
    }
    mesh.bounds = {};
    if (!mesh.indices.empty()) {
        const auto inf = std::numeric_limits<float>::infinity();
        mesh.bounds = {{inf, inf, inf}, {-inf, -inf, -inf}};
        for (const auto index : mesh.indices) {
            if (index >= mesh.vertices.size()) throw std::runtime_error("Object mesh index out of range");
            const auto v = mesh.vertices[index].position;
            mesh.bounds.minimum = {std::min(mesh.bounds.minimum.x, v.x), std::min(mesh.bounds.minimum.y, v.y), std::min(mesh.bounds.minimum.z, v.z)};
            mesh.bounds.maximum = {std::max(mesh.bounds.maximum.x, v.x), std::max(mesh.bounds.maximum.y, v.y), std::max(mesh.bounds.maximum.z, v.z)};
        }
    }
    // The source's local bounds are no longer world bounds. Use the transformed indexed geometry.
    mesh.declared_bounds = mesh.bounds;
}

#ifdef __vita__
namespace {
struct TerrainSizes {
    std::size_t vertices{}, indices{}, collision_vertices{}, collision_triangles{};
};
TerrainSizes terrain_reached;
std::string terrain_level;
bool terrain_complete = false;
constexpr const char* terrain_reserve_file = "local/terrain_reserve.txt";

// The built-in sizes (a level not yet in local/terrain_reserve.txt): the disc's own levels as measured (132vt2c: the
// play host's launcher arguments, --textured with the sky; TORONTO1 and TRAINING agree with the Vita3K runs' "Terrain
// size" lines). A level not measured (a mod's) reserves TORONTO1's sizes and gives back what it did not use at
// terrain_reserve_end; growing the aggregate from nothing re-allocated it ~20 times (the 132vt2b hardware run's
// std::bad_alloc at 29.9 MB, with the last 26.8 MB copy and the heap's holes alive).
constexpr TerrainSizes default_terrain_sizes{588765, 1257144, 66437, 94662};
const std::map<std::string, TerrainSizes>& builtin_terrain_sizes() {
    static const std::map<std::string, TerrainSizes> sizes{
        {"BELARUS1", {520152, 1147428, 60271, 77114}},  {"BELARUS2", {718475, 1521618, 126453, 183785}},
        {"CHECHNYA", {259443, 531933, 54873, 80208}},   {"ITALY", {189226, 383535, 39964, 51879}},
        {"KYRGSTAN", {215526, 444018, 35491, 49629}},   {"LORELEI", {427848, 897198, 68341, 85745}},
        {"MINSK", {449404, 890271, 73940, 98080}},      {"MNTNEGR1", {176513, 381327, 20600, 33085}},
        {"MYANMAR", {574800, 1258851, 93905, 151654}},  {"TOKYO", {566462, 1199028, 59985, 80087}},
        {"TORONTO1", default_terrain_sizes},            {"TORONTO2", {309625, 657699, 43941, 59319}},
        {"TORONTO3", {125361, 267894, 18818, 23441}},   {"TRAINING", {125565, 281148, 6098, 9072}},
        {"UKRAINE", {531003, 1110612, 49303, 68580}},   {"YEMEN1", {328289, 624333, 55537, 76471}},
        {"YEMEN2", {163648, 309687, 23006, 30249}},     {"ZURICH", {343058, 747024, 36377, 51082}},
    };
    return sizes;
}

std::map<std::string, std::pair<TerrainSizes, bool>> read_terrain_sizes() {
    std::map<std::string, std::pair<TerrainSizes, bool>> sizes;
    std::ifstream in(terrain_reserve_file);
    std::string level, state;
    TerrainSizes s;
    while (in >> level >> s.vertices >> s.indices >> s.collision_vertices >> s.collision_triangles >> state)
        sizes[level] = {s, state == "complete"};
    return sizes;
}
}  // namespace

void terrain_reserve_begin(const std::string& level, Mesh& dst, CollisionMesh& col) {
    terrain_level = level;
    terrain_reached = {};
    terrain_complete = false;
    TerrainSizes want;
    const char* source = "none";
    const auto stored = read_terrain_sizes();
    const auto found = stored.find(level);
    if (found != stored.end() && found->second.second) {
        want = found->second.first;
        source = terrain_reserve_file;
    } else {
        if (const auto builtin = builtin_terrain_sizes().find(level); builtin != builtin_terrain_sizes().end()) {
            want = builtin->second;
            source = "built in";
        } else {
            want = default_terrain_sizes;
            source = "not measured: TORONTO1's, the rest given back at the end";
        }
        if (found != stored.end()) {  // a failed load's sizes, +1/4, when past the built-in ones
            const auto& s = found->second.first;
            const auto more = [](std::size_t& w, std::size_t r) { w = std::max(w, r + r / 4); };
            more(want.vertices, s.vertices);
            more(want.indices, s.indices);
            more(want.collision_vertices, s.collision_vertices);
            more(want.collision_triangles, s.collision_triangles);
        }
    }
    dst.vertices.reserve(want.vertices);
    dst.indices.reserve(want.indices);
    col.vertices.reserve(want.collision_vertices);
    col.triangles.reserve(want.collision_triangles);
    std::cout << "Terrain reserve (port-only, PS Vita): " << level << " " << want.vertices << " vertices, " << want.indices
              << " indices, " << want.collision_vertices << " / " << want.collision_triangles
              << " collision vertices / triangles (" << source << ")\n";
}

void terrain_reserve_end(Mesh& dst, CollisionMesh& col) {
    terrain_reached = {dst.vertices.size(), dst.indices.size(), col.vertices.size(), col.triangles.size()};
    const auto fit = [](auto& v) { if (v.capacity() - v.size() > v.size() / 8) v.shrink_to_fit(); };
    fit(dst.vertices);
    fit(dst.indices);
    fit(col.vertices);
    fit(col.triangles);
    terrain_complete = true;
    std::cout << "Terrain size (port-only, PS Vita): " << terrain_level << " " << terrain_reached.vertices << " vertices ("
              << (dst.vertices.capacity() * sizeof(MeshVertex) >> 10) << " KB held), " << terrain_reached.indices
              << " indices, " << terrain_reached.collision_vertices << " / " << terrain_reached.collision_triangles
              << " collision vertices / triangles\n";
    terrain_reserve_flush();
}

void terrain_reserve_flush() {
    if (terrain_level.empty()) return;
    auto sizes = read_terrain_sizes();
    auto& entry = sizes[terrain_level];
    if (terrain_complete || !entry.second) {
        auto& s = entry.first;
        if (terrain_complete) s = terrain_reached;
        else {
            s.vertices = std::max(s.vertices, terrain_reached.vertices);
            s.indices = std::max(s.indices, terrain_reached.indices);
            s.collision_vertices = std::max(s.collision_vertices, terrain_reached.collision_vertices);
            s.collision_triangles = std::max(s.collision_triangles, terrain_reached.collision_triangles);
        }
        entry.second = terrain_complete;
        std::ofstream out(terrain_reserve_file, std::ios::trunc);
        for (const auto& [level, e] : sizes)
            out << level << ' ' << e.first.vertices << ' ' << e.first.indices << ' ' << e.first.collision_vertices << ' '
                << e.first.collision_triangles << ' ' << (e.second ? "complete" : "partial") << '\n';
    }
    terrain_level.clear();
}
#endif

void append_terrain(Mesh& dst, CollisionMesh& col, const Mesh& src, const CollisionMesh& source_col) {
    if (dst.vertices.size() + src.vertices.size() > 10000000 || dst.indices.size() + src.indices.size() > 30000000
        || col.vertices.size() + source_col.vertices.size() > 10000000)
        throw std::runtime_error("Terrain aggregate exceeds supported limits");
    const auto vertices = static_cast<std::uint32_t>(dst.vertices.size());
    const auto indices = static_cast<std::uint32_t>(dst.indices.size());
    const auto textures = static_cast<std::uint32_t>(dst.textures.size());
    const auto materials = static_cast<std::uint32_t>(dst.materials.size());
    const bool had_geometry = !dst.indices.empty();
#ifdef __vita__
    // 132vt2 (research/VITA.md "Memory"): terrain_reserve_begin reserved the level's expected size; past it the
    // aggregate grows by an eighth rather than doubling, and the size reached is noted for the next load.
    const auto grow = [](auto& v, std::size_t more, std::size_t& reached) {
        const std::size_t need = v.size() + more;
        reached = std::max(reached, need);
        if (need > v.capacity()) v.reserve(std::max(need, v.size() + v.size() / 8));
    };
    grow(dst.vertices, src.vertices.size(), terrain_reached.vertices);
    grow(dst.indices, src.indices.size(), terrain_reached.indices);
    grow(col.vertices, source_col.vertices.size(), terrain_reached.collision_vertices);
    grow(col.triangles, source_col.triangles.size(), terrain_reached.collision_triangles);
#endif
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (const auto index : src.indices) dst.indices.push_back(vertices + index);
    dst.textures.insert(dst.textures.end(), src.textures.begin(), src.textures.end());
    for (auto material : src.materials) {
        material.texture += textures;
        for (auto& pass : material.passes) pass.texture += textures;
        dst.materials.push_back(std::move(material));
    }
    for (auto surface : src.surfaces) {
        surface.first_index += indices; surface.material += materials; dst.surfaces.push_back(surface);
    }
    dst.batches += src.batches;
    for (auto batch : src.batch_spans) {
        batch.first_vertex += vertices;
        dst.batch_spans.push_back(batch);
    }
    for (auto patch : src.color_patches) {
        for (auto& span : patch.spans) span.first_vertex += vertices;
        dst.color_patches.push_back(std::move(patch));
    }
    if (!src.indices.empty()) {
        if (!had_geometry) dst.bounds = src.bounds;
        else {
            dst.bounds.minimum = {std::min(dst.bounds.minimum.x, src.bounds.minimum.x), std::min(dst.bounds.minimum.y, src.bounds.minimum.y), std::min(dst.bounds.minimum.z, src.bounds.minimum.z)};
            dst.bounds.maximum = {std::max(dst.bounds.maximum.x, src.bounds.maximum.x), std::max(dst.bounds.maximum.y, src.bounds.maximum.y), std::max(dst.bounds.maximum.z, src.bounds.maximum.z)};
        }
        dst.declared_bounds = dst.bounds;
    }
    const auto base = static_cast<std::uint32_t>(col.vertices.size());
    col.vertices.insert(col.vertices.end(), source_col.vertices.begin(), source_col.vertices.end());
    for (auto triangle : source_col.triangles) {
        for (auto& index : triangle.indices) index += base;
        col.triangles.push_back(triangle);
    }
}
}
