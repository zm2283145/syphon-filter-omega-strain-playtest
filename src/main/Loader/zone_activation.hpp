#pragma once
// The zone activation map (ZONE_ACTIVATION.md). 001c24d0 ("Initializing
// Activation Map...") gives every terrain object (TER:, class 00174500, type
// word *004ee780) the bounds of its INL: record (+230 min, +240 max, w = 1)
// and the keys of its ACL: group (+204). Loaders then add objects to those
// lists through 00173d60, 001740e0 resolves the keys (+1f4, a null for a key
// nothing answers), and the per-player zone tracker (0013ee70 / 0013db30,
// run from the render pass 00132e50 and once at level start from 001350e0)
// activates (00224330) the members of the terrain the player is in and
// deactivates (00224180) those of the terrain left.
#include "population.hpp"
#include "animation_root.hpp"
#include <functional>
#include <optional>

namespace omega {
struct ActivationZone {
    std::uint32_t key{};
    RootVector min{}, max{};                // +230 / +240 as read (w = 1.0)
    std::vector<std::uint32_t> keys;        // +204 (before 001740e0)
    std::vector<std::uint32_t> members;     // +1f4 (after 001740e0); 0 = a null entry
};
class ZoneActivationMap {
public:
    // 001c24d0, ACL:/INL: path: zones in TER order (the global terrain list
    // 004ffd48), each INL record (terrain, group, six floats; 001741b0) in
    // file order appending its group's keys, each masked to
    // (category & 0xff) << 16 | index. An INL terrain that is not a zone or
    // a group index out of range throws (the original would desynchronise).
    ZoneActivationMap(const std::vector<std::uint32_t>& terrain_keys,
                      const std::vector<std::vector<std::uint32_t>>& groups,
                      const std::vector<TerrainLink>& links);
    // 00173d60(zone, member, propagate, parent): appends the member's key to
    // the zone when it is missing and parent is absent, the zone itself, or
    // listed in the zone; with propagate, repeats for every other zone listed
    // in the zone (propagate off, the same parent). Members added to objects
    // that are not terrain never reach a zone (only terrain keys are listed
    // in terrain lists), so only zone targets are modelled.
    void associate(std::size_t zone, std::uint32_t member, bool propagate, std::optional<std::size_t> parent);
    // 001740e0 on every zone: +1f4 = the keys in order, 0 where `exists`
    // answers false (0040c540 returns null); throws when called twice.
    void resolve(const std::function<bool(std::uint32_t)>& exists);
    const std::vector<ActivationZone>& zones() const { return zones_; }
    std::optional<std::size_t> zone_of(std::uint32_t key) const;
    bool resolved() const { return resolved_; }
private:
    // Port-only (132vl2, load time): an open-addressing table of 64-bit keys (never ~0) to 32-bit values, without
    // a node allocation per entry.
    class KeyTable {
    public:
        // Stores key -> value when the key is absent; answers whether it was stored.
        bool insert(std::uint64_t key, std::uint32_t value);
        std::optional<std::uint32_t> find(std::uint64_t key) const;
        void clear();
    private:
        std::vector<std::uint64_t> keys_;
        std::vector<std::uint32_t> values_;
        std::size_t size_{};
        std::size_t slot(std::uint64_t key) const;
    };
    std::vector<ActivationZone> zones_;
    bool resolved_{};
    // Port-only (132vl2, load time): lookups that answer as the scans did. index_ maps each zone key to its zone (keys
    // are unique: the constructor throws on a duplicate); listed_ holds (zone << 32 | key) for each key a zone lists,
    // until 001740e0.
    KeyTable index_;
    KeyTable listed_;
    // Port-only (132vl2): the zones among each zone's keys, in key order (the keys the propagation acts on).
    std::vector<std::vector<std::uint32_t>> linked_;
    void list_key(std::size_t zone, std::uint32_t key);
};
// The population's map: TER:/ACL:/INL: and the associations of the hosted
// classes in load order: SND: (00212ee0, before GOB: pairs with neither word
// -1 resolving to a zone, (zone, object, 1, zone)), GOB: (0021add0: each pair (a, b) that 0040c480
// resolves to a zone, 00173d60(zone, object, 1, zone)), PLR: (001c59d0:
// parent words neither -1 resolving to a zone, (zone, placement, 1, zone)),
// NIL: then NIE:
// (00237f80 / 0023c610: each link resolving to a zone, (zone, member, 1,
// zone)), PNT: (001c1470, after NIE, before GEN: link words neither -1, the
// light and the link both found, (zone, light, 1, zone)), GEN: (00221d20:
// (zone, generator, 1, none) for the generator's region), INV: (00257f30,
// after GEN: owner words neither -1 resolving to a zone, (zone, crate, 1,
// none)), BOX: (00172ae0:
// each pair with a > 0 resolving to a zone, (zone, box, 0, none)), FIR:
// (002625b0, after BOX: each link with neither word -1 resolving to a zone,
// (zone, fire, 1, none)). `exists` answers 0040c540 for the
// resolution.
ZoneActivationMap population_zone_activation_map(const Population&, const std::function<bool(std::uint32_t)>& exists);

struct ZoneRequest {
    std::uint32_t key{};
    bool activate{};   // 00224330 (else 00224180)
    bool forced{};     // their third argument: the tracker's +4 byte
    bool operator==(const ZoneRequest&) const = default;
};
// The per-player tracker (+0 the actor, +4 forced byte, +8 the zone). Member
// presence is the members' +2d: the tracker's requests set it (00224330 sets
// +2d after its activation, 00224180 clears it), clear at construction
// except the keys given to mark_present.
class ZoneTracker {
public:
    // 0013ee70: zone = the first terrain (00224d80), forced. Needs a resolved
    // map with at least one zone.
    explicit ZoneTracker(const ZoneActivationMap& map);
    // 0013db30. `position` is the owner's virtual +e0 (the actor: 00133e00,
    // physical +2f0 or the model translation, plus +380), `camera` the owner
    // +50 component's translation +40 (00173980's distance origin).
    std::vector<ZoneRequest> update(const RootVector& position, const RootVector& camera);
    bool present(std::uint32_t key) const;
    // A member whose own constructor set +2d before the first update (the
    // INV: crate: 00257f30's box block calls virtual +5c 003ce980).
    void mark_present(std::uint32_t key) { set_present(key, true); }
    std::size_t zone() const { return zone_; }
    bool forced() const { return forced_; }
    // 00173980: the zone's members ordered for activation (the first n
    // entries, n = its terrain members, by truncated squared distance of their
    // bounds centre from `camera`, then the rest in list order).
    std::vector<std::uint32_t> sorted_members(std::size_t zone, const RootVector& camera) const;
    // 00224ac0: the listed terrain containing p with the largest squared
    // extent (strictly greater, from 0); 002248a0: the terrain nearest to p
    // (first at distance 0, else the first strict minimum from FLT_MAX).
    std::optional<std::size_t> containing_member(std::size_t zone, const RootVector& p) const;
    std::optional<std::size_t> nearest(const RootVector& p) const;
private:
    const ZoneActivationMap* map_;
    std::size_t zone_{};
    bool forced_{true};
    std::vector<std::uint32_t> present_;   // sorted keys with +2d set
    void set_present(std::uint32_t key, bool value);
};
// The +88 bounds test of 0013db30 / 00224ac0: p - min and max - p without a
// MAC sign flag (x, y, z; a -0 difference counts as outside).
bool zone_contains(const ActivationZone& zone, const RootVector& p);
}
