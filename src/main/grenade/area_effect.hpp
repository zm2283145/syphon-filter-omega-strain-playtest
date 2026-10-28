#pragma once
// Lingering area effects (COMBAT_ITEMS.md C7b): the 0xe0-byte object
// (grenade.cc, vtable 004dc5b0) built by 0025a2d0 and updated by 00259880:
// flash (type 3), fire (4), lethal gas (5), tear gas (6), knockout gas (7),
// smoke (8, nothing) and 2 (nothing). The gas exposure test 002596e0, the
// flash/blind setters 001866e0 / 00186830 and their decay in the actor update
// (001b2328..001b237c) are here too. The world (object list 004f78dc, LOS
// 00148290 with mask 0x58, node positions, the damage virtual, ignition) is
// the host; every call goes through it and is also returned as an event in
// call order. The point light of type 3 (pool 00535d20) and the list join
// (004f8040) are presentation/bookkeeping events. Single player only.
#include "explosion.hpp"
#include "weapon_database.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

struct AreaEffect {
    std::uint64_t object{};
    RootVector position_70{};
    std::int32_t record_80{};
    ObjectWeakHandle owner_84;
    float age_60{};
    bool done_d4{},flag_d5{},particles_d6{};
    std::array<RootVector,4> extents_90{};       // types 5/6/7 only (others leave them unwritten)
    bool operator==(const AreaEffect&) const=default;
};

// An object of the list as 00259880 reads it when it reaches it.
struct AreaTarget {
    std::uint8_t life_108{};                     // virtual +108 (3 dead, 2 dying)
    std::optional<bool> controller_player_54;    // +58 controller's +54 (nullopt: none)
    bool byte_14{};
    bool human_4c{};
    RootVector position_e4{};
    std::array<RootVector,4> matrix_f4{};        // virtual +f4 (rows; row 3 the origin)
    bool fire_immune_33b8{},invulnerable_33a4{},burning_326a{},killed_33a5{},not_gasable_346b{},night_vision_3470{};
    std::int32_t stealth_44{};
};
// The owner fields the update reads (controller +54 and +14).
struct AreaOwner {
    std::optional<bool> controller_player_54;
    bool byte_14{};
};

struct AreaEvent {
    enum class Kind {
        light,          // type 3: point light from pool 00535d20 (radius 100/100, white), presentation
        los,            // 00148290(..., start, displacement, 0, 0, 0, mask 0x58, 0, 0, 0)
        node,           // 00132910(model, node *0049d024): the object's node position
        ignite,         // 00186360(object, 1, owner, 1, 0, -1.0)
        flash,          // 001866e0(object, 0, amount): +33ac
        blind,          // 00186830(object, 0, amount): +33a8
        statistic,      // 00333f10(stats, id, 1): 4 lethal gas, 6 knockout
        damage,         // object +100(object, owner, category, type, null, 0, null, amount)
        tear_gas,       // action 6 kind 0 (0055d480, vtable 004e0840) to the object, transport 0
        unregister,     // 0040c9a0
        retire,         // 003cb230(area, 0.1)
    } kind{};
    std::uint64_t object{};
    std::uint64_t owner{};
    std::int32_t value{};                        // node / category / statistic id
    std::int32_t type{};
    float amount{};
    RootVector position{},vector{};
    bool operator==(const AreaEvent&) const=default;
};

class AreaWorld {
public:
    virtual ~AreaWorld()=default;
    // The list 004f78dc in order: each node's weak handle (+8 object, +c id).
    virtual std::vector<ObjectWeakHandle> objects()=0;
    virtual bool valid(const ObjectWeakHandle& handle)=0;
    virtual AreaTarget target(std::uint64_t object)=0;
    virtual AreaOwner owner(std::uint64_t object)=0;
    // 00132910(00132a30(object), node): the node's world position.
    virtual RootVector node_position(std::uint64_t object,std::int32_t node)=0;
    // 00148290 with the LOS manager, mask 0x58: the hit point, or nullopt.
    virtual std::optional<RootVector> los(const RootVector& start,const RootVector& displacement)=0;
    virtual void ignite(std::uint64_t object,std::uint64_t owner)=0;
    virtual void flash(std::uint64_t object,float amount)=0;
    virtual void blind(std::uint64_t object,float amount)=0;
    virtual void damage(std::uint64_t object,std::uint64_t owner,std::uint8_t category,std::uint8_t type,float amount)=0;
    virtual void tear_gas(std::uint64_t object)=0;
};

// 0025a2d0(area, owner, &position, record, flag): the fields, and for types
// 5/6/7 the four horizontal extents (directions (0, 1), (0, -1), (1, 0),
// (-1, 0) times +c8; a blocked ray stops at 0.9 of the way to the hit).
AreaEffect construct_area_effect(std::uint64_t object,const ObjectWeakHandle& owner,const RootVector& position,
    const WeaponRecord& record,std::int32_t record_id,bool flag,AreaWorld& world,std::vector<AreaEvent>& events);
// 00259880(area, dt).
void update_area_effect(AreaEffect& area,float dt,const WeaponRecord& record,AreaWorld& world,std::vector<AreaEvent>& events);
// One area of the level reset 0025a120 (game::ResetLevel 0012f6e0): an area
// whose +d4 is 0 gets +d4 = 1, 0040c9a0 and 003cb230(area, 0.1) (the events
// of 00259880's done path); returns whether it did.
bool finish_area_effect(AreaEffect& area,std::vector<AreaEvent>& events);
// 002596e0(area, object): not gasable (+346b) -> no; else a horizontal ray at
// the height of the object's node 14 from the centre and from each extent to
// that node; any clear ray means exposed.
bool area_gas_exposure(const AreaEffect& area,std::uint64_t object,bool not_gasable,AreaWorld& world,std::vector<AreaEvent>& events);

// Checkpoint 132fx1: 002594a0 cAreaEffect::LOS_Obscured (SmokeVolumes_BlockSight, grenade.cc; matched C): the
// first area of the list 004f8040 (in order) with +d6 set whose volume the segment a-b meets: the ratio 1, or
// +60 / 5.0 while growing (+d5 clear, +60 < 5.0 and record +bc != 4); the radius record +c8 x ratio; the band
// [z, z + 8.0 (1 + ratio) / 2] of +70 clips the segment (0025a880), then 0025aa20: the clipped segment within the
// radius of (+70 with z + 4.0). Returns that area's object, 0 when none.
std::uint64_t area_smoke_blocker(const std::vector<AreaEffect>& areas,const std::function<const WeaponRecord&(std::int32_t)>& record,
    const RootVector& a,const RootVector& b);
// 0025a880(a, b, lo, hi): clips the segment to lo <= z <= hi in place; false when both ends are on one side.
bool area_clip_band(RootVector& a,RootVector& b,float lo,float hi);
// 0025aa20(a, b, c, r): the segment a-b passes within r of c.
bool area_segment_near(const RootVector& a,const RootVector& b,const RootVector& c,float r);

// The actor fields of a flash and a blindness, and their decay.
struct ActorSenses {
    float blind_33a8{};
    float flash_33ac{};
    bool operator==(const ActorSenses&) const=default;
};
// 001866e0(actor, 0, amount) in single player: +33ac = amount.
void apply_actor_flash(ActorSenses& senses,float amount);
// 00186830(actor, 0, amount) in single player: +33a8 = amount; returns true
// when the white-out 00260990(*004ffc30, 1, 0.0, 0.3, amount, amount) is
// requested (actor +14 and a player controller).
bool apply_actor_blind(ActorSenses& senses,float amount,bool byte_14,bool controller_player);
// 001b2328..001b237c in the actor update 001b1710 (static): each positive
// timer loses dt and stops at 0.
void decay_actor_senses(ActorSenses& senses,float dt);

}
