#pragma once
// Thrown and launched projectiles (COMBAT_ITEMS.md C6a): the object built by
// 0025d730 (0x130 bytes, weapon.cc, vtable 004dc6c0), launched by 0025d4b0
// (prepare_projectile_launch) and updated by 0025ad80 (vtable +48), and the
// inventory launch 00141e20. The movement prefix is prepare_projectile_integration;
// the rest of 0025ad80 is here: the -1000 cutoff, the LOS request (mask 0x10)
// and the actor scan 0022ba30 through the host, bounce, rest, stuck-on-actor,
// fuse, impact and the detonation dispatch (0021a200 / 0025a2d0). Sounds, AI
// sounds, effects, unregistering and the retire timer are returned as events
// in call order; the host runs the synchronous calls (LOS, scan, static
// damage, explosion, area object). Single player only. The transform node
// (+50, vtable 004df6a0) is kept whole: the detonation reads its translation
// and the draw 0025ac50 its matrix.
#include "bullet_hit.hpp"
#include "explosion.hpp"
#include "projectile_launch.hpp"
#include "projectile_simulation.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// Runtime values the projectile reads besides the record.
struct ThrownProjectileTuning {
    float hold1_spin_y{};          // *004f7fe8 (0 in the boot snapshot; no writer found)
    float hold1_minimum_angle{};   // *004f7fe0 (likewise)
};

struct ThrownProjectile {
    std::uint64_t object{};                       // own identity (LOS self, AI sound source)
    std::int32_t id_c{};                          // +c
    ProjectileIntegrationState motion;            // +70 position, +80 previous, +90 velocity, +a0 acceleration,
                                                  // +b0 force, +f0 launch, +64 inverse mass, +100 previous dt,
                                                  // +104 age, +e4 retired, +e5 at rest, +e6 first step
    float mass_60{};
    float fuse_e0{};
    bool stuck_e8{};                              // stopped by an actor until a later LOS hit
    RootVector spin_110{};                        // +110/+114/+118 rad/s
    std::array<RootVector,4> node{};              // transform node (+50) matrix +10..+4f (row 3 = +40, the translation)
    ObjectWeakHandle owner_124;                   // +124/+128
    std::int32_t record_120{};                    // the weapon record (item ID)
    bool operator==(const ThrownProjectile&) const=default;
};

// 0025d730(obj, owner, &frame, record): rows of the hand frame (row 3 the
// origin). The node gets the frame (hold class 1) or the identity rotation
// with the origin (0025d910..0025d940).
ThrownProjectile construct_thrown_projectile(std::uint64_t object,std::int32_t id_c,const ObjectWeakHandle& owner,
    const std::array<RootVector,4>& frame,const WeaponRecord& record,std::int32_t record_id,const ThrownProjectileTuning& tuning);
// 0025d4b0(obj, &target, arc): +f0 = the launch velocity from +70.
void launch_thrown_projectile(ThrownProjectile& projectile,const WeaponRecord& record,const RootVector& target,float arc,
    float gravity_z,const ThrownProjectileTuning& tuning);

// The LOS manager's reply to the projectile request 005435a0 (result +0 hit,
// +10 point, +50 normal, +84 the hit object reference).
struct ProjectileLosHit {
    RootVector point{},normal{};
    std::uint64_t object{};
    bool operator==(const ProjectileLosHit&) const=default;
};
// 0021a200 as the detonation calls it: (owner or null, record +64, kind 1,
// &node position, skip humans 0, local 1, strength 3.0, inner, outer).
struct ProjectileBlastCall {
    ObjectWeakHandle owner;                       // object 0 when the owner no longer resolves
    std::int32_t damage{};
    RootVector point{};
    float strength{},inner{},outer{};
    bool operator==(const ProjectileBlastCall&) const=default;
};
// 0025a2d0(area, owner or null, &point, record, flag) (grenade.cc, 0xe0 bytes).
struct ProjectileAreaCall {
    ObjectWeakHandle owner;
    RootVector point{};
    std::int32_t record{};
    bool flag{};
    bool operator==(const ProjectileAreaCall&) const=default;
};

struct ThrownProjectileEvent {
    enum class Kind {
        los,                // LOS request (origin, displacement)
        scan,               // 0022ba30 request (step, skip-damage byte)
        static_damage,      // hold 7: hit object +100(object, owner, 1, type, null, 0, null, amount)
        landed_ai_sound,    // 0016b4f0(msg, self +c, cause 5, 1.0, 15.0) to the AI Sound System, transport 0
        bounce_sound,       // 001ca210 record +20, 0020f990 at the projectile, to 004ffb50
        explosion,          // 0021a200
        screen_effect,      // 00231f00(identity at the point, 10.0, 0.5)
        effect,             // 002797c0(*004ffc3c, prev + 0.9 (impact - prev), effect *00236f20(0x25), 5.0)
        detonation_ai_sound,// 0016b420(msg, owner +c, &point, cause 4, 1.0, record +fc) (owner resolves)
        area,               // 0025a2d0
        impact_effect,      // 00232460(record +d8, identity at the point)
        detonation_sound,   // T_STAR_ROCK (hold 7), record +10 (weapon class 8) or G_GREN_EXP
        unregister,         // 0040c9a0
        retire,             // 003cb230(obj, 5.0)
    } kind{};
    RootVector position{},vector{};
    std::uint64_t object{};
    std::uint64_t owner{};                        // the owner passed on (0: null)
    std::int32_t value{};                         // type / cause / damage / effect id / flag
    float amount{};
    std::string name;
    bool operator==(const ThrownProjectileEvent&) const=default;
};

struct ThrownProjectileHost {
    // The LOS manager (004ea0b0 virtual +c): origin W = 1, displacement W = 0, self, mask 0x10.
    std::function<std::optional<ProjectileLosHit>(const RootVector& origin,const RootVector& displacement)> los;
    // 0022ba30 over the world objects (scan_bullet_candidates); fills +30 / +34.
    std::function<void(BulletHitRequest& request)> scan;
    std::function<bool(const ObjectWeakHandle&)> valid;
    // Hold 7: the hit object's damage virtual +100 (static geometry).
    std::function<void(std::uint64_t object,std::uint64_t owner,std::int8_t type,float amount)> static_damage;
    // *00236f20(index): the effect id (-1: none).
    std::function<std::int32_t(std::int32_t index)> effect_id;
    std::function<void(const ProjectileBlastCall&)> explode;
    std::function<void(const ProjectileAreaCall&)> area;
};

struct ThrownProjectileStep {
    std::vector<ThrownProjectileEvent> events;
    bool detonated{};
};
// 0025ad80(obj, dt). `gravity` is the shared vector 004f5430: hold class 7
// adds 10 to its Z for the step and subtracts it afterwards (the result can
// round differently, as in the original). `scan_aim_view_byte` is request +d
// of the 0022ba30 request, which 0025ad80 leaves unwritten on its stack: the
// caller supplies it (00188780 reads it for the aim-view hit statistic).
ThrownProjectileStep update_thrown_projectile(ThrownProjectile& projectile,float dt,RootVector& gravity,
    const WeaponRecord& record,const ThrownProjectileHost& host,bool scan_aim_view_byte);
// 0025bb50..0025bd3c (inside 0025ad80, every moving step without an impact
// detonation): the node matrix with row 3 = +70, premultiplied by the spin
// rotation 00140e20 of the angles +110/+114/+118 * dt.
void spin_thrown_projectile_node(ThrownProjectile& projectile,float dt);

// 0025ac50(obj) (vtable +58, called by the scene walk for an object filed in
// list 8150: +49 = 3, +2c / +2d / +2f set by 003cf0e0 / vt +5c / 0025d88c).
// Nothing when retired (+e4). Otherwise the light reference 00535d40 is the
// node's +40 (vt +e0 = 003cd420; bytes 00535d60 / 00535d61 set when it
// changes) and, with a model (record +11c, else +13c; 0: none), the node's
// dirty byte is cleared (vt +58 = 00132900) and 003836b0(model, +38, node +10, 0).
struct ThrownProjectileDraw {
    RootVector light_reference{};
    std::int32_t model{};                         // 0: no 003836b0 call
    std::array<RootVector,4> matrix{};            // the node's +10..+4f
};
std::optional<ThrownProjectileDraw> thrown_projectile_draw_0025ac50(const ThrownProjectile& projectile,const WeaponRecord& record);
// 0025ac20 (vtable +88, the scene walk's bounds): record +13c's model +3e0,
// else the box 005716c0 (returned as 0: the caller decides).
std::int32_t thrown_projectile_bounds_model_0025ac20(const WeaponRecord& record);

// The 0022ba30 request frame 0025ad80 builds from the step (0025b364..0025b438):
// rows side, forward, up and the origin (the previous position).
std::array<RootVector,4> projectile_scan_frame(const RootVector& displacement,const RootVector& origin);

// 00141e20(inventory, arc, &target name, &point): the launch from the held
// slot. The model's hand node (*0049d02c, 00132a30/00133ed0) and the actor
// matrix (+f4) are host inputs; the target resolves through 0040c540.
struct InventoryLaunchInput {
    std::array<RootVector,4> hand_frame{};
    std::array<RootVector,4> actor_matrix{};      // virtual +f4 (read for an actor-local point, W = 0)
    std::optional<RootVector> target_position;    // resolved target +e4 (nullopt: none or the "none" word)
    std::uint64_t target_object{};
    RootVector point{};                           // the aim point (W = 0: actor-local)
    float arc{};
    bool ai_controller{};                         // actor +58 virtual +58
    std::uint64_t object{};                       // the new projectile's identity
    std::int32_t id_c{};
    ObjectWeakHandle owner;                       // the actor
};
struct InventoryLaunchEvent {
    enum class Kind {
        detach_slot_0,      // 0038c7c0(actor + 60, 0)
        fire_notice,        // AI controller +cc(target, 0)
        ai_sound,           // 0016b4f0(msg, actor +c, cause 4, 1.0, record +b8)
        spare_used,         // slot +c -= 1 (not with +99)
        star_statistics,    // hold 7: 0013fe80(inventory, point W == 1)
    } kind{};
    std::uint64_t object{};
    float amount{};
    bool flag{};
    bool operator==(const InventoryLaunchEvent&) const=default;
};
struct InventoryLaunchResult {
    ThrownProjectile projectile;
    RootVector target{};
    std::vector<InventoryLaunchEvent> events;
};
InventoryLaunchResult launch_inventory_projectile(WeaponInventory& inventory,const WeaponDatabase& database,
    const InventoryLaunchInput& input,float gravity_z,const ThrownProjectileTuning& tuning);

// Checkpoint 132wf1: the aim view's throw arc. The pad block (0024f350..0024f388, a hold-class-1 item with Square
// held in the aim view) calls 0025cdc0(hand, &+d0, f12 = the power +e0, f13 = cvt.s.w(record +104)) with the hand
// the actor's skeleton node *(s8)0049d02c = 20 (00132a30, 00132910) and +d0 the thrown point 002497c0 wrote. It
// fills the vector 004f8000 (+4 count, +8 data; 001810c0 clear, 00180bd0 push) that the render frame's 0025c650
// (00132e50 at 001330fc: after the scene, before the taser wires 002629c0 and the HUD 00244b90) draws and clears.
//   0025cdc0: d = target - hand, xyz normalised (vrsqrt / vmulq.xyz, w kept); side 004f8010 = d x (0, 0, 1, 1)
//   (vopmula / vopmsub, w 0); the start = hand + side * 1.0 (0048afd8); side *= 0.6 / 3.0 (0048afa8, div.s);
//   v = 003a8c80(start, target, 0, *004f5438, power, *004f7fe0) (solve_projectile_trajectory), clamped to the
//   speed when |v|^2 > speed * speed (normalised xyz, then all lanes times the speed); a Verlet particle on the
//   stack (001ec2a0(state, 1.0): mass 1, inverse mass 1) at the start with the previous position start - 0.1 v
//   and the force 0 + gravity 004f5430 * 1; the start is pushed, then each step x' = (2 x - x_prev) +
//   force * (1 * 0.01) (0017dc00, 00183380, 0017f670, 00134080; dt 1 / 10 from 0048af80, w = 1 by 00183000) is
//   pushed until the step x - x_prev no longer rises (z >= 0) and x.z <= target.z (c.olt.s / c.ole.s). The
//   velocity / acceleration the step also writes (+30 / +40) are not read.
struct ThrowArcPreview {
    RootVector side{};                    // 004f8010 after the 0.6 / 3 scale: the ribbon's half width
    std::vector<RootVector> points;       // 004f8000's elements
};
ThrowArcPreview throw_arc_preview(const RootVector& hand,const RootVector& target,float power,float speed_104,
    const RootVector& gravity,float gravity_z,const ThrownProjectileTuning& tuning);
// 0025c650: per point p (count n = *004f8004), two strip vertices (0025ca40) p - side and p + side, both coloured
// 0025c9d0 (cvt.w.s(128 c)) with c = (0.3, 0.6, 0.3, a) (0048afb0 / afb8 / afc0), a from 1.0 (0048afc8) down by
// (1.0 - 0.1) / n (0048afd0, cvt.s.w) after each point. One 00371200 flush: a triangle strip, untextured
// (00539228 = 0), ALPHA 004bc1b0[0] (0x8000000044), the mode 0 for the draw (005392d0 restored after); then
// 001810c0 clears the vector.
struct ThrowArcVertex {
    RootVector position{};
    std::array<std::int32_t,4> rgba{};
};
std::vector<ThrowArcVertex> throw_arc_ribbon(const ThrowArcPreview& arc);
inline constexpr std::uint64_t throw_arc_alpha=0x0000008000000044ULL;   // 004bc1b0[0]

}
