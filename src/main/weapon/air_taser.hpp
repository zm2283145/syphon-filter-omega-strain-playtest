#pragma once
// The air taser (taser.cc, 00262d90..00266d00; COMBAT_ITEMS.md §air taser):
// the 0xd0-byte object 00266630 creates at the inventory's +0 for the hold
// class 5 items (80, 92). Entry 00266200 (from the discharge 00277e20), fire
// 00265b90, the per-frame dispatcher 00262d90 (from the weapon update
// 00144080: 1 flight 00264e10, 2 shock 00263890, 3 reel 00262e00), release
// 00263080, reset 00266410 and their helpers (wobble 00262f30, physics
// 00264370, local -> world 00264530, world -> local 00264720, attach
// 00264bb0 / 00264980, smoothing 00265790, the wire LOS 00265910).
//
// Every call the original makes outside the module is a host function, in
// call order. Single player: the multiplayer sends (005721c8) are skipped.
// Stack words the original leaves uninitialised are 0 here: the local of a
// pushed wire segment (always rewritten before it is read), the hit point
// +40 of a boneless attach (only read with a bone) and the w of the flight
// angles +50 (22abc0 stores a whole quad; only x and z are read).
#include "air_taser_object.hpp"
#include "animation_root.hpp"
#include "bullet_hit.hpp"
#include "level_lights.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// The fields of an object the module reads directly.
struct AirTaserObjectFacts {
    std::int32_t id_c{-1};
    bool actor_4c{};                        // +4c == *0049d010 (an actor)
    std::int32_t state_320c{};
    bool invulnerable_33a4{};               // 001b8740
    bool byte_346a{};
    bool flak_33a0{};                       // a flak jacket
};

struct AirTaserContext {
    std::uint64_t owner{};
    std::int32_t owner_id{};                // owner +c
    bool owned_14{};                        // owner +14 (a local player's actor)
    const WeaponRecord* record{};           // the held weapon's record (the scan's +18)
    std::int32_t range_3c{};                // record +3c
    float noise_fc{};                       // record +fc
};

struct AirTaserHost {
    // The owner's controller +58 vcall +64(out, 0, aim view): its matrix.
    std::function<AirTaserMatrix(bool aim_view)> controller_matrix;
    std::function<float()> aim_range;                                         // *(owner +3524 inventory) +f4
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> target_point;   // vcall +c0
    std::function<AirTaserObjectFacts(std::uint64_t object)> facts;
    std::function<std::uint8_t(std::uint64_t object)> life;                   // vcall +108
    // The object's controller +58 vcall +54 (nullopt: no controller).
    std::function<std::optional<bool>(std::uint64_t object)> player;
    std::function<void(std::uint32_t desc,const RootVector& position)> positional_sound;   // 0020f860 -> 004ffb50
    std::function<void(std::uint32_t desc,std::uint64_t object)> sound;      // 0020f990 -> 004ffb50
    std::function<void(std::uint32_t desc,std::uint64_t object)> stop_sound; // 0020eba0 -> 004ffb50
    std::function<void(std::uint64_t object,std::int32_t kind,std::int32_t on,float amount)> effect;   // 00185d40
    std::function<void(std::uint64_t object,std::int32_t code)> stun_action;  // 0055d480 / 004e01e0 (+24 7, +28 2)
    std::function<void(std::uint64_t object,std::uint64_t source,std::int32_t a2,std::int32_t type,float amount)> damage;   // vcall +100
    std::function<void()> statistic;                                          // 00333f10(001314f0(G, -1), 6, 1)
    std::function<void(std::uint64_t victim,std::uint64_t owner)> camera_target;   // 00131540
    std::function<void()> camera_release;                                     // 00131510
    std::function<void(BulletHitRequest& request)> scan;                      // 0022ba30
    std::function<bool(const RootVector& start,const RootVector& displacement,std::uint64_t owner)> los;   // 004ea0b0 vcall +c
    // 00174080(0012f2b0(o), &id): the object's +c (0012f2b0 is o + 0xc) equals the id.
    std::function<bool(std::uint64_t object,std::int32_t id)> registered;
    std::function<AirTaserMatrix(std::uint64_t object,std::int32_t node)> node_matrix;    // 00133ed0(00132a30(o), node)
    std::function<RootVector(std::uint64_t object,std::int32_t node)> node_position;      // 00132910(00132a30(o), node)
    std::function<RootVector(std::uint64_t object)> position;                 // 00132020(001a91a0(001ae3a0(o)))
    std::function<std::uint32_t()> random;                                    // 00127dd0
    // 0036b180(0x70, taser.cc line) then 003a2dd0(.., 0.8, 003a35c0(0053b540,
    // "Tazer_Line_glo1"), matrix): the effect, 0 when the allocation failed.
    std::function<std::uint64_t(std::int32_t line,const AirTaserMatrix& matrix)> create_effect;
    std::function<void(std::uint64_t effect,const AirTaserMatrix& matrix,float dt)> update_effect;   // 003a2400(e, m, 0, dt)
    std::function<void(std::uint64_t effect,std::int32_t line)> destroy_effect;   // 003a2ba0(e, -1), 0036b130(line)
    std::function<void(const AirTaserMatrix& matrix)> burn;                   // 00232120(10, 004f8370)
    std::function<void(std::uint64_t object)> flak;                           // 00242fa0(.., "Flak Jacket", -1, 0, 0, 5.0, 0)
    std::function<void(std::int32_t owner_id,const RootVector& position,float radius)> noise;   // 0016b420(.., 4, 1.0, radius) -> 004ee5c0
    LevelLights* lights{};                                                    // 00535d20's point pool
};

inline constexpr std::uint32_t air_taser_fire_sound=0x004f1130;
inline constexpr std::uint32_t air_taser_buzz_sound=0x004f12d0;
inline constexpr std::uint32_t air_taser_hiss_sound=0x004f1470;
inline constexpr std::uint32_t air_taser_burn_sound=0x004f1610;

// 00266200(obj, target, aim view, subtarget): 0 when the object is busy, else
// fires (00265b90) and returns 1.
bool air_taser_entry(AirTaserObject& obj,std::uint64_t target,bool aim_view,std::int32_t subtarget,
    const AirTaserContext& ctx,const AirTaserHost& host);
void air_taser_fire(AirTaserObject& obj,std::uint64_t target,bool aim_view,std::int32_t subtarget,const RootVector& point,
    const AirTaserContext& ctx,const AirTaserHost& host);
// 00262d90(obj, dt).
void air_taser_update(AirTaserObject& obj,float dt,const AirTaserContext& ctx,const AirTaserHost& host);
void air_taser_release(AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& host);
void air_taser_reset(AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& host);
// The helpers, exposed for the goldens.
bool air_taser_los(const AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& host);
void air_taser_smooth(AirTaserObject& obj,float factor,bool half);
void air_taser_physics(AirTaserObject& obj,bool flag,float dt);
void air_taser_local_to_world(AirTaserObject& obj,const RootVector& origin);
void air_taser_world_to_local(AirTaserObject& obj,const RootVector& origin);

}
