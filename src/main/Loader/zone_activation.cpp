#include "zone_activation.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// Binary32 as the EE interpreter models the VU: each operation rounded, a
// denormal result flushed to a zero of the same sign.
float flush(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float vsub(float a, float b) { return flush(a - b); }
float vmul(float a, float b) { return flush(a * b); }
float vadd(float a, float b) { return flush(a + b); }
// vmul.xyz then the ACC sum (x + y) + z (vmulax / vmadday / vmaddz with 1.0).
float squared_length(float x, float y, float z) {
    return vadd(vadd(vmul(x, x), vmul(y, y)), vmul(z, z));
}
// cvt.w.s: truncation, saturating at the int32 range.
std::int32_t truncate(float v) {
    if (std::isnan(v)) throw std::runtime_error("Zone activation: NaN distance");
    if (v >= 2147483647.0F) return std::numeric_limits<std::int32_t>::max();
    if (v <= -2147483648.0F) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(v);
}
std::uint32_t mask_key(std::uint32_t key) { return (key & 0x00ff0000U) | (key & 0xffffU); }   // 001741b0
bool listed(const std::vector<std::uint32_t>& list, std::uint32_t key) {
    return std::find(list.begin(), list.end(), key) != list.end();
}
}

bool zone_contains(const ActivationZone& zone, const RootVector& p) {
    for (std::size_t c = 0; c < 3; ++c) if (std::signbit(vsub(p[c], zone.min[c]))) return false;
    for (std::size_t c = 0; c < 3; ++c) if (std::signbit(vsub(zone.max[c], p[c]))) return false;
    return true;
}

bool ZoneActivationMap::KeyTable::insert(std::uint64_t key, std::uint32_t value) {
    if ((size_ + 1) * 2 > keys_.size()) {   // at most half full
        std::vector<std::uint64_t> keys(std::max<std::size_t>(64, keys_.size() * 2), ~0ULL);
        std::vector<std::uint32_t> values(keys.size());
        keys.swap(keys_);
        values.swap(values_);
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] != ~0ULL) {
                const auto at = slot(keys[i]);
                keys_[at] = keys[i];
                values_[at] = values[i];
            }
    }
    const auto at = slot(key);
    if (keys_[at] == key) return false;
    keys_[at] = key;
    values_[at] = value;
    ++size_;
    return true;
}

std::optional<std::uint32_t> ZoneActivationMap::KeyTable::find(std::uint64_t key) const {
    if (keys_.empty()) return std::nullopt;
    const auto at = slot(key);
    if (keys_[at] != key) return std::nullopt;
    return values_[at];
}

void ZoneActivationMap::KeyTable::clear() {
    keys_ = {};
    values_ = {};
    size_ = 0;
}

// The key's slot or the empty one where it would go (linear probing from a Fibonacci hash; never full).
std::size_t ZoneActivationMap::KeyTable::slot(std::uint64_t key) const {
    const std::size_t mask = keys_.size() - 1;
    auto at = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
    while (keys_[at] != key && keys_[at] != ~0ULL) at = (at + 1) & mask;
    return at;
}

namespace {
std::uint64_t listed_key(std::size_t zone, std::uint32_t key) { return (static_cast<std::uint64_t>(zone) << 32) | key; }
}

ZoneActivationMap::ZoneActivationMap(const std::vector<std::uint32_t>& terrain_keys,
                                     const std::vector<std::vector<std::uint32_t>>& groups,
                                     const std::vector<TerrainLink>& links) {
    for (const auto key : terrain_keys) {
        if (!index_.insert(key, static_cast<std::uint32_t>(zones_.size())))
            throw std::runtime_error("Zone activation: duplicate terrain key " + std::to_string(key));
        ActivationZone zone;
        zone.key = key;
        // 00174500: min = FLT_MAX, max = -FLT_MAX (w = 1) until an INL record.
        const float big = std::numeric_limits<float>::max();
        zone.min = {big, big, big, 1.0F};
        zone.max = {-big, -big, -big, 1.0F};
        zones_.push_back(std::move(zone));
    }
    linked_.resize(zones_.size());
    for (const auto& link : links) {
        const auto zone = zone_of(link.id);
        if (!zone) throw std::runtime_error("Zone activation: INL record for a key that is not a terrain: " + std::to_string(link.id));
        if (link.adjacency >= groups.size()) throw std::runtime_error("Zone activation: INL group index out of range");
        auto& z = zones_[*zone];
        z.min = {link.bounds.minimum.x, link.bounds.minimum.y, link.bounds.minimum.z, 1.0F};
        z.max = {link.bounds.maximum.x, link.bounds.maximum.y, link.bounds.maximum.z, 1.0F};
        for (const auto key : groups[link.adjacency]) {
            list_key(*zone, mask_key(key));
            listed_.insert(listed_key(*zone, mask_key(key)), 1);
        }
    }
}

// Appends the key to the zone's list (the zones exist before any key is listed, so a key's zone never changes).
void ZoneActivationMap::list_key(std::size_t zone, std::uint32_t key) {
    zones_[zone].keys.push_back(key);
    if (const auto other = index_.find(key)) linked_[zone].push_back(*other);
}

std::optional<std::size_t> ZoneActivationMap::zone_of(std::uint32_t key) const {
    if (const auto zone = index_.find(key)) return *zone;
    return std::nullopt;
}

void ZoneActivationMap::associate(std::size_t zone, std::uint32_t member, bool propagate, std::optional<std::size_t> parent) {
    if (resolved_) throw std::runtime_error("Zone activation: association after 001740e0");
    if (zone >= zones_.size() || (parent && *parent >= zones_.size()))
        throw std::runtime_error("Zone activation: association with an unknown zone");
    const bool add = !parent || *parent == zone || listed_.find(listed_key(zone, zones_[*parent].key));
    if (add && listed_.insert(listed_key(zone, member), 1)) list_key(zone, member);
    if (!propagate) return;
    // The original loops over every key, re-reading the count; the recursion only changes other zones, so this zone's
    // keys (and the zones among them, linked_) are fixed during the loop.
    for (std::size_t i = 0; i < linked_[zone].size(); ++i) {
        const auto other = linked_[zone][i];
        if (other != zone) associate(other, member, false, parent);
    }
}

void ZoneActivationMap::resolve(const std::function<bool(std::uint32_t)>& exists) {
    if (resolved_) throw std::runtime_error("Zone activation: 001740e0 run twice");
    for (auto& zone : zones_) {
        zone.members.clear();
        for (const auto key : zone.keys) zone.members.push_back(zone_of(key) || exists(key) ? key : 0U);
        zone.keys.clear();
    }
    listed_.clear();
    linked_ = {};
    resolved_ = true;
}

ZoneActivationMap population_zone_activation_map(const Population& population, const std::function<bool(std::uint32_t)>& exists) {
    std::vector<std::uint32_t> keys;
    for (const auto& terrain : population.terrain) keys.push_back(terrain.id);
    ZoneActivationMap map(keys, population.adjacency, population.links);
    // SND (001c3390 from 001c3940 at 001c39fc, before GOB 001c3510): the
    // constructor 00212ee0 after virtual +60(1) (00213518..00213580), each
    // pair with neither word -1 that 0040c480 resolves; 00173d60(found,
    // object, 1, found).
    for (const auto& sound : population.sound_objects)
        for (const auto& pair : sound.associations) {
            if (pair[0] == 0xffffffffU || pair[1] == 0xffffffffU) continue;
            if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;   // 0040c480 range checks
            if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, sound.id, true, zone);
        }
    // GOB (0021add0 after virtual +60(1)): each association pair that
    // resolves; 00173d60(found, object, 1, found).
    for (const auto& object : population.objects)
        for (const auto& pair : object.associations) {
            if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;   // 0040c480 range checks
            if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, object.id, true, zone);
        }
    // PLR (001c1ce0 from 001c3940 at 001c3a64, after GOB, before NIL / NIE):
    // the constructor 001c59d0 (0x1c5d14..0x1c5d44), with neither parent word
    // -1 and 0040c480 resolving, 00173d60(found, placement, 1, found).
    for (const auto& placement : population.placements) {
        const auto& pair = placement.parent;
        if (pair[0] == 0xffffffffU || pair[1] == 0xffffffffU) continue;
        if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;   // 0040c480 range checks
        if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, placement.id, true, zone);
    }
    // NIL then NIE (001c3050 from 001c3940 after GOB, before GEN 001c2ae0):
    // each link 0040c480 resolves, 00173d60(found, member, 1, found)
    // (00238674 in 00237f80, 0023cdcc in 0023c610).
    auto event_links = [&map](std::uint32_t member, const std::vector<std::array<std::uint32_t, 2>>& links) {
        for (const auto& pair : links) {
            if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;
            if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, member, true, zone);
        }
    };
    for (const auto& light : population.event_lights) event_links(light.id, light.links);
    for (const auto& object : population.event_objects) event_links(object.id, object.links);
    // PNT (001c1470 from 001c4190, after 001c3940, before ENV 001c12a0 and GEN
    // 001c2ae0): per record, link words (bytes 8..15) neither -1, the light
    // 0040c480(id words) and the link 0040c480(link words) both found;
    // 00173d60(link, light, 1, link).
    for (const auto& light : population.point_lights) {
        const auto& pair = light.skipped;
        if (pair[0] == 0xffffffffU || pair[1] == 0xffffffffU) continue;
        if (light.id_category >= 0x100 || light.id_index >= 0x10000) continue;   // 0040c480 range checks
        if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;
        if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, light.id(), true, zone);
    }
    // GEN (00221d20, version >= 0x32): the region (generator category, index)
    // when not -1, else 002248a0 from the position; 00173d60(region, generator,
    // 1, none). A region that resolves to no zone is handled by the caller.
    for (const auto& generator : population.generators) {
        std::optional<std::size_t> zone;
        if (generator.parent_region != -1) {
            zone = map.zone_of((generator.id & 0xffff0000U) | (static_cast<std::uint32_t>(generator.parent_region) & 0xffffU));
            if (static_cast<std::uint32_t>(generator.parent_region) >= 0x10000U) zone.reset();
        } else {
            ZoneTracker probe(map);
            zone = probe.nearest({generator.position.x, generator.position.y, generator.position.z, 1.0F});
        }
        if (zone) map.associate(*zone, generator.id, true, std::nullopt);
    }
    // INV (001c2a10, after GEN 001c2ae0, before BOX 001c0c00): cBackpack_ctor
    // 00257f30 with neither owner word -1, 0040c480 resolving: 00173d60(found,
    // crate, 1, 0).
    for (const auto& backpack : population.backpacks) {
        const auto& pair = backpack.owner;
        if (pair[0] == 0xffffffffU || pair[1] == 0xffffffffU) continue;
        if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;   // 0040c480 range checks
        if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, backpack.id, true, std::nullopt);
    }
    // BOX (00172ae0): pairs with a > 0 (signed) that resolve; (found, box, 0, none).
    for (const auto& box : population.hotboxes)
        for (const auto& pair : box.associations) {
            if (static_cast<std::int32_t>(pair[0]) <= 0 || pair[0] >= 0x100 || pair[1] >= 0x10000) continue;
            if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, box.id, false, std::nullopt);
        }
    // FIR (002625b0 from 001c0a90, which 001c4190 calls after BOX 001c0c00):
    // each link with neither word -1 that 0040c480 resolves; 00173d60(found,
    // fire, 1, none).
    for (const auto& fire : population.fires)
        for (const auto& pair : fire.links) {
            if (pair[0] >= 0x100 || pair[1] >= 0x10000) continue;
            if (const auto zone = map.zone_of((pair[0] << 16) | pair[1])) map.associate(*zone, fire.id, true, std::nullopt);
        }
    map.resolve(exists);
    return map;
}

ZoneTracker::ZoneTracker(const ZoneActivationMap& map) : map_(&map) {
    if (map.zones().empty()) throw std::runtime_error("Zone tracker without terrain (00224d80 returns null)");
}

bool ZoneTracker::own_present(std::uint32_t key) const { return std::binary_search(present_.begin(), present_.end(), key); }
bool ZoneTracker::present(std::uint32_t key) const {
    return own_present(key) || std::binary_search(held_.begin(), held_.end(), key);
}

std::vector<ZoneRequest> ZoneTracker::set_held(std::vector<std::uint32_t> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<ZoneRequest> requests;
    for (const auto key : held_)
        if (key && !std::binary_search(keys.begin(), keys.end(), key) && !own_present(key)) requests.push_back({key, false, false});
    for (const auto key : keys)
        if (key && !std::binary_search(held_.begin(), held_.end(), key) && !own_present(key)) requests.push_back({key, true, false});
    held_ = std::move(keys);
    return requests;
}
void ZoneTracker::set_present(std::uint32_t key, bool value) {
    const auto at = std::lower_bound(present_.begin(), present_.end(), key);
    const bool has = at != present_.end() && *at == key;
    if (value && !has) present_.insert(at, key);
    else if (!value && has) present_.erase(at);
}

std::optional<std::size_t> ZoneTracker::containing_member(std::size_t zone, const RootVector& p) const {
    const auto& zones = map_->zones();
    float best = 0;
    std::optional<std::size_t> found;
    for (const auto key : zones.at(zone).members) {
        const auto member = key ? map_->zone_of(key) : std::nullopt;   // +49 == 1 and terrain type
        if (!member) continue;
        const auto& z = zones[*member];
        if (!zone_contains(z, p)) continue;
        RootVector extent{};
        if (z.min[0] <= z.max[0]) for (std::size_t c = 0; c < 4; ++c) extent[c] = vsub(z.max[c], z.min[c]);
        const float value = squared_length(extent[0], extent[1], extent[2]);
        if (value <= best) continue;   // c.ole.s
        best = value;
        found = member;
    }
    return found;
}

std::optional<std::size_t> ZoneTracker::nearest(const RootVector& p) const {
    const auto& zones = map_->zones();
    float best = std::numeric_limits<float>::max();
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < zones.size(); ++i) {
        const auto& z = zones[i];
        std::array<float, 3> d{};
        for (std::size_t c = 0; c < 3; ++c) {
            const float below = vsub(z.min[c], p[c]), above = vsub(p[c], z.max[c]);
            d[c] = !std::signbit(below) ? vadd(below, 0.0F) : !std::signbit(above) ? vadd(above, 0.0F) : 0.0F;
        }
        const float distance = squared_length(d[0], d[1], d[2]);
        if (distance < best) {   // c.olt.s
            best = distance;
            found = i;
            if (distance == 0) break;
        }
    }
    return found;
}

std::vector<std::uint32_t> ZoneTracker::sorted_members(std::size_t zone, const RootVector& camera) const {
    const auto& members = map_->zones().at(zone).members;
    const auto n = members.size();
    constexpr std::int32_t limit = 100000000;   // 0x05f5e100
    std::vector<std::int32_t> distance(n, 0);    // 00173d20 with *0048a140 (0)
    std::size_t terrain = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto member = members[i] ? map_->zone_of(members[i]) : std::nullopt;
        if (!member) continue;
        const auto& z = map_->zones()[*member];
        std::array<float, 3> centre{};
        for (std::size_t c = 0; c < 3; ++c) centre[c] = vmul(vadd(z.min[c], z.max[c]), 0.5F);
        const float squared = squared_length(vsub(camera[0], centre[0]), vsub(camera[1], centre[1]), vsub(camera[2], centre[2]));
        distance[i] = std::min(truncate(squared), limit);
        ++terrain;
    }
    std::vector<std::uint32_t> sorted;
    // Repeatedly take the smallest value above the previous one (from -1.0)
    // and append, in list order, every entry of the first `terrain` with it.
    float previous = -1.0F, current = static_cast<float>(limit);
    std::size_t pushed = 0;
    while (pushed < terrain) {
        for (std::size_t i = 0; i < terrain; ++i) {
            const float v = static_cast<float>(distance[i]);
            if (v <= current && !(v <= previous)) current = v;
        }
        const auto before = pushed;
        for (std::size_t i = 0; i < terrain; ++i) {
            const float v = static_cast<float>(distance[i]);
            if (v <= current && !(v <= previous)) { sorted.push_back(members[i]); ++pushed; }
        }
        // A null or non-terrain entry among the first `terrain` (distance 0)
        // is never taken after the first round; the original would not end.
        if (pushed == before) throw std::runtime_error("Zone activation: 00173980 would not terminate for zone " + std::to_string(map_->zones()[zone].key));
        previous = current;
        current = static_cast<float>(limit);
    }
    for (std::size_t i = terrain; i < n; ++i) sorted.push_back(members[i]);
    return sorted;
}

std::vector<ZoneRequest> ZoneTracker::update(const RootVector& position, const RootVector& camera) {
    if (!map_->resolved()) throw std::runtime_error("Zone tracker before 001740e0");
    const auto& zones = map_->zones();
    std::vector<ZoneRequest> requests;
    if (!forced_ && zone_contains(zones[zone_], position)) return requests;
    auto next = containing_member(zone_, position);
    if (!next) next = nearest(position);
    if (!next) { forced_ = true; return requests; }
    if (!forced_ && *next == zone_) { forced_ = false; return requests; }
    const auto& old = zones[zone_].members;
    zone_ = *next;
    const auto sorted = sorted_members(zone_, camera);
    for (const auto key : old) {
        if (!forced_ && listed(sorted, key)) continue;
        if (!key || !own_present(key)) continue;
        // Port-only (132pt1): a member another agent's tracker holds stays present (no request).
        if (!std::binary_search(held_.begin(), held_.end(), key)) requests.push_back({key, false, forced_});
        set_present(key, false);
    }
    for (const auto key : sorted) {
        if (!forced_ && listed(old, key)) continue;
        if (!key || own_present(key)) continue;
        if (!std::binary_search(held_.begin(), held_.end(), key)) requests.push_back({key, true, forced_});
        set_present(key, true);
    }
    forced_ = false;
    return requests;
}
}
