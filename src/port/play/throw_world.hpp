#pragma once
// The thrown items between their launch and their effect (research/
// COMBAT_ITEMS.md, C6b): the projectiles that the release of a throw creates
// through the inventory launch 00141e20 (launch_inventory_projectile, C6a),
// their per-frame update 0025ad80 and the detonation dispatch to an explosion
// (0021a200, C7a, through the host) or an area object 0025a2d0 whose update
// 00259880 (C7b) this world also runs. The world, its LOS service, the actor
// scan and the area targets are the host's.
//
// Order (a host decision, not established for the original): the host calls
// update once per frame after the actor traversal; it updates the projectiles
// alive at the start of the call in creation order, then the area objects
// alive at the start of the call. An area created by a detonation is first
// updated by the next call. The objects' activation and registration
// (0040c9d0) and the retire timers are recorded, not simulated: a retired
// projectile or a done area is dropped from the lists.
#include "actor_animation_receiver.hpp"
#include "area_effect.hpp"
#include "thrown_projectile.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

struct ThrowWorldConfig {
    ThrownProjectileTuning tuning;               // *004f7fe0 / *004f7fe8
    RootVector gravity{};                        // the shared vector 004f5430
    // Request +d of the projectile's 0022ba30 request (unwritten stack in
    // 0025ad80; read by 00188780 only for a hold-7 hit): required for hold 7.
    std::optional<bool> scan_aim_view_byte;
    std::uint64_t first_object{};                // identities given to new projectiles and areas
    std::int32_t first_id{};                     // their +c
};
struct ThrowWorldHost {
    std::function<std::optional<ProjectileLosHit>(const RootVector& origin,const RootVector& displacement)> los;   // 004ea0b0, mask 0x10
    std::function<void(BulletHitRequest& request)> scan;                                                             // 0022ba30
    std::function<bool(const ObjectWeakHandle&)> valid;
    std::function<void(std::uint64_t object,std::uint64_t owner,std::int8_t type,float amount)> static_damage;       // hold 7
    std::function<std::int32_t(std::int32_t index)> effect_id;                                                       // *00236f20
    std::function<void(const ProjectileBlastCall&)> explode;                                                         // 0021a200
};
struct ThrownItem {
    ThrownProjectile projectile;
    std::int32_t item{};                         // the weapon record's item ID
};
struct ThrowWorldFrame {
    std::vector<std::pair<std::uint64_t,ThrownProjectileEvent>> projectile_events;
    std::vector<std::pair<std::uint64_t,AreaEvent>> area_events;
    std::vector<std::uint64_t> detonated,areas_created,areas_done;
};

// The 00141e20 inputs from the actor's fields: +33cc resolved through
// 0040c540 (`find`: the object and its +e4 for a name word; nullopt for the
// "none" word or an unknown name), +33d0, and the arc the caller captured.
struct ThrowLaunchActor {
    std::array<RootVector,4> hand_frame{};       // node *0049d02c of the actor's model (00132a30 / 00133ed0)
    std::array<RootVector,4> actor_matrix{};     // virtual +f4
    bool ai_controller{};                        // +58 virtual +58
    ObjectWeakHandle owner;                      // the actor
};
struct ThrowLaunchTarget {std::uint64_t object{};RootVector position_e4{};};
InventoryLaunchInput throw_launch_input(const ActorAnimationReceiverState& actor,float arc,const ThrowLaunchActor& launcher,
    const std::function<std::optional<ThrowLaunchTarget>(std::uint32_t name_word)>& find);

// 0021a200 as a projectile detonation calls it, for ExplosionWorld::blast:
// the owner as the source (nullopt: null), record +64, kind 1, the point, no
// human skip, strength 3.0 and the radii.
BlastRequest projectile_blast_request(const ProjectileBlastCall& call,std::optional<BlastSource> owner);

class ThrowWorld {
public:
    ThrowWorld(const WeaponDatabase& database,AreaWorld& areas,ThrowWorldHost host,ThrowWorldConfig config);
    // 00141e20 for the held slot of `inventory`: the projectile gets the next
    // identity; the launch result's events are returned with it.
    InventoryLaunchResult launch(WeaponInventory& inventory,InventoryLaunchInput input);
    ThrowWorldFrame update(float dt);
    // 0025a120 (game::ResetLevel's third step) over the live areas in order
    // (finish_area_effect); the done ones are dropped as update drops them.
    ThrowWorldFrame finish_areas();
    // Checkpoint 132uk4: 0025a2d0 called outside update (the bullet update's
    // hold-8/9 block, which runs before update in the frame): the area gets
    // the next identity, its constructor's events go to `out`, and the next
    // update call updates it with the live areas (a host decision, as for a
    // detonation's area).
    std::uint64_t add_area(const ObjectWeakHandle& owner,const RootVector& point,std::int32_t record,bool flag,ThrowWorldFrame& out);
    // Checkpoint 132uk6: the discharge's launched projectile (0013ff40
    // 0x00140740..0x001409c8): 0025d730(new, owner, &frame, record) and
    // 0025d4b0(obj, &point, arc). The projectile gets the next identity and
    // is first updated by the next update call (as a throw's launch).
    std::uint64_t launch_discharge(const ObjectWeakHandle& owner,const std::array<RootVector,4>& frame,std::int32_t item,
        const RootVector& point,float arc);
    const std::vector<ThrownItem>& projectiles() const{return projectiles_;}
    const std::vector<AreaEffect>& areas() const{return areas_;}
    const RootVector& gravity() const{return config_.gravity;}
private:
    const WeaponDatabase& db_;
    AreaWorld& area_world_;
    ThrowWorldHost host_;
    ThrowWorldConfig config_;
    std::vector<ThrownItem> projectiles_;
    std::vector<AreaEffect> areas_;
    std::uint64_t next_object_{};
    std::int32_t next_id_{};
};

}
