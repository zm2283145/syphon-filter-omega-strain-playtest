#pragma once
// The melee attack (research/COMBAT_ITEMS.md, C5; COMBAT.md, "Checkpoint 113
// NeckSnap takedown"): 001960c0's melee branch (hold classes 3, 4, 10 and 11)
// calls the target choice 001414d0 on the actor's inventory and then the
// attack start 00196480(actor, target, takedown). The unarmed takedown is the
// NeckSnap (perk 0, gated in 001960c0 by 00332f90, agent_perks.hpp); the knife
// takedown is the throat slit. Interpreter-verified (tools/ee/gen_melee_target.py,
// tools/ee/gen_melee_start.py).
//
// Every outside call goes through a host callback in the original's order;
// a callback the original would call that is empty throws.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include "player_targeting.hpp"
#include "secondary_component.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace omega {

// 0022ab20(a, b, out): 0022abc0 (target_angles) with a's virtual +f4 matrix
// whose row 3 is replaced by a's eye +c0(-1), at b's eye +c0(-1).
TargetAngles melee_relative_angles(const TargetingMatrix& matrix_f4,const RootVector& eye_c0,const RootVector& point_c0);

// The object virtuals and fields 001414d0 reads. Each is required only when
// the original would read it.
struct MeleeTargetHost {
    std::uint64_t attacker{};                                              // inventory +88
    // The target manager's list 004f78dc (004f77c0 +11c) in list order: node
    // +8 the object, +c the identity the object's +c must still match.
    std::vector<TargetHandle> candidates;
    std::function<std::int32_t(std::uint64_t object)> object_id;          // object +c
    std::function<bool(std::uint64_t object)> player;                     // controller +58 non-null and its virtual +54
    std::function<std::uint8_t(std::uint64_t object)> life_state;         // virtual +108
    std::function<bool(std::uint64_t object)> actor_class_4c;              // +4c == *0049d010
    std::function<RootVector(std::uint64_t object)> position_e4;          // virtual +e4
    std::function<TargetingMatrix(std::uint64_t object)> matrix_f4;       // virtual +f4
    std::function<RootVector(std::uint64_t object)> eye_c0;               // virtual +c0(-1)
    std::function<bool(std::uint64_t object)> invulnerable_33a4;          // 001b8740: +33a4
    // Physical +3310's +310 (velocity); nullopt without the component (the
    // original then reads the static zero vector 004e1d90).
    std::function<std::optional<RootVector>(std::uint64_t object)> velocity_310;
};
struct MeleeTarget {
    std::uint64_t target{};                                                // 0: none
    bool takedown{};                                                       // the out byte
    bool operator==(const MeleeTarget&) const=default;
};
// 001414d0(inventory, &takedown): the nearest candidate (planar distance
// squared below 5.5^2, strict, so the earlier of equals stays) that is not
// the attacker, not a player, not dying/dead (2/3), of the actor class (+4c),
// within 2.0 vertically and whose yaw from the attacker (0022ab20) is below
// 1.1 in magnitude. The out byte is cleared; it is set when the chosen target
// is not invulnerable, the attacker is behind it (|yaw from the target| above
// 2.0) and the target's speed (the VU length of +310 xyz) is below 1.0.
MeleeTarget choose_melee_target(const MeleeTargetHost& host);

// 001437a0 & 0xff and the fields 00196480 reads from its arguments.
struct MeleeAttackInput {
    std::uint64_t actor{};
    std::uint64_t target{};                    // 0: none
    std::int32_t target_id{-1};                // target +c
    bool takedown{};
    std::uint32_t hold_class{};                // 001437a0(+3524) & 0xff
    bool secondary{};                          // +3314 present
    bool attacker_physical{},target_physical{};   // +3310 present
    float guard_24{};                          // *(*004ffbd0) +24, read by a locally owned actor
    // The teleport offset (x, y) of the takedown: *004eeeb0 / *004eeeb8
    // (knife), *004eeec0 / *004eeec8 (unarmed); all four 0.0 in the boot
    // snapshot (no writer found).
    std::array<float,2> knife_offset{},unarmed_offset{};
};
struct MeleeAttackHost {
    std::function<std::uint8_t(std::uint64_t target)> life_state;                        // target virtual +108
    std::function<void(std::uint64_t object)> clear_physical_3d0;                        // physical +3d0 = 0
    std::function<void(std::uint64_t actor)> physical_reset_11;                          // physical +11 = 1
    // 0018c5a0 on *(object +2ec0): the node of that name (nullopt: none).
    std::function<ActorMotionRegistration(std::uint64_t object,std::string_view name)> resolve;
    // The retained motion *(actor +326c) (word +0, selector +4).
    std::function<ActorMotionRegistration(std::uint64_t actor)> retained;
    // 003ad500 on 0018c6a0(object +2ec0) = object +2ec0 +34.
    std::function<void(std::uint64_t object,const MotionRequest& request,MotionInstallMode mode)> install;
    std::function<TargetingMatrix(std::uint64_t actor)> matrix_f4;                       // actor virtual +f4
    // The teleport message (key 0055d4e0, vtable 004d9360, +24 = 2, +30..+6f
    // the matrix) sent to the target with transport 1.
    std::function<void(std::uint64_t target,const TargetingMatrix& matrix)> teleport;
    // 0018c9d0(target, name, code): target +34e4[code] = the node of that name.
    std::function<void(std::uint64_t target,std::string_view name,std::uint8_t code)> death_motion;
    // Target virtual +100(target, attacker, category, type, 0, 0, 0, amount).
    std::function<void(std::uint64_t target,std::uint64_t attacker,std::uint8_t category,std::uint8_t type,float amount)> damage;
    std::function<bool(std::uint64_t actor)> player;                                    // controller +58 non-null and its virtual +54
    // 00131540(004ffb50, actor, 0): the player's takedown camera set-up; it
    // ends with the world-stack push 00131320(G, G +d4) and G +d8 = 1.
    std::function<void(std::uint64_t actor)> push_world_object;
    // 001cae20(+3314, name, kind, &event).
    std::function<bool(const std::array<std::uint8_t,48>& name,std::uint8_t kind,const SecondaryOneShot& event)> one_shot;
    std::function<RootVector(std::uint64_t actor)> position_e4;                          // actor virtual +e4
    // 0020f860(request, descriptor, &position), byte +20 = 1, sent to 004ffb50 with transport 0.
    std::function<void(std::uint32_t descriptor,const RootVector& position)> sound;
    // The replication message (key 004eeee8, vtable 004da8f0, +24 the target,
    // +28 the takedown byte, +20 = 4, +1c = 0x40) to the actor itself with
    // transport 0 (its network receiver 00197380 replays 00196480).
    std::function<void(std::uint64_t actor,std::uint64_t target,bool takedown)> replicate;
};
inline constexpr std::uint32_t melee_baton_sound=0x004f2310;         // 0020f860 descriptor "G_STUNSTICK" (+10, static init 004c8994)
inline constexpr std::uint32_t melee_knife_miss_sound=0x004f2650;    // 0020f860 descriptor "G_KNIFE_MISS" (+10, static init 004c8ae0)
struct MeleeAttackResult {
    bool started{};                            // past the +33e8 / guard returns
    bool takedown{};                           // the takedown branch ran
    std::optional<bool> one_shot;              // 001cae20 was called: its answer
    bool replicated{};                         // the tail message was sent
};
// 00196480(actor, target, takedown) on the actor's +33e8 / +33cc
// (ActorAnimationReceiverState byte_33e8 / target_33cc) and +14 (byte_14).
MeleeAttackResult start_melee_attack(ActorAnimationReceiverState& actor,const MeleeAttackInput& input,const MeleeAttackHost& host);

// The melee hit 00141880(inventory, target, forced) (COMBAT.md, "Checkpoint
// 115 melee hit and takedown execution"), called by 00197e00's melee trigger
// (actor_throw.hpp) with forced 0 once the swing passes its hit phase.
// Interpreter-verified (tools/ee/gen_melee_hit.py).
struct MeleeHitInput {
    std::uint8_t mode_84{};                    // inventory +84 (lbu): only 5 (the melee slot) hits
    std::uint32_t item_58{};                   // inventory +58: slot 5's item word (+84 * 16 + 8)
    std::uint64_t attacker{};                  // inventory +88
    std::uint64_t target{};
    bool forced{};                             // a2
    // The engine variable byte *00489da0 "doKnifeDmg" (boot value 1).
    std::uint8_t knife_damage{};
};
// The fields of the weapon record 00147840(*004ffd30, item) answers that
// 00141880 reads.
struct MeleeHitRecord {
    std::uint8_t hold_class_f0{};              // 3 knife, 4 stun baton, 10 torch
    std::int32_t damage_64{};                  // cvt.s.w for the knife's damage
    std::int32_t penetration_f8{};             // the +100 penetration word (in/out through t0)
};
struct MeleeHitHost {
    std::function<MeleeHitRecord(std::uint32_t item)> record;                          // 00147840(*004ffd30, item)
    std::function<bool(std::uint64_t object)> player;                                  // controller +58 non-null and its virtual +54
    std::function<RootVector(std::uint64_t object)> position_e4;                       // virtual +e4
    // 003caa50(004ffb50, 004ea0b0) -> 00148290(manager, out, query (+40, +42,
    // +60, +64 zero), &origin, &displacement, 0, 0, 0; stack 0x58, 0, 0, 0):
    // the static LOS with excluded mask 0x58. True (a nonzero result byte):
    // blocked.
    std::function<bool(const RootVector& origin,const RootVector& displacement,std::uint32_t excluded_mask)> blocked;
    std::function<std::uint8_t(std::uint64_t object)> life_state;                      // virtual +108
    std::function<bool(std::uint64_t object)> actor_class_4c;                          // +4c == *0049d010
    std::function<bool(std::uint64_t object)> stun_immune_346a;                        // +346a != 0
    std::function<bool(std::uint64_t object)> byte_14;                                 // +14 != 0
    std::function<bool(std::uint64_t object)> invulnerable_33a4;                       // 001b8740: +33a4
    // 00333f10(001314f0(004ffb50, -1), id, amount): the current player's statistic.
    std::function<void(std::uint32_t id,std::uint32_t amount)> statistic;
    // 00184ce0(target, a1, a2, f12, f13): the stun.
    std::function<void(std::uint64_t target,std::uint32_t a1,std::uint32_t a2,float f12,float f13)> stun;
    // 00186360(target, a1, a2 = attacker, a3, t0, f12): the ignition.
    std::function<void(std::uint64_t target,std::uint32_t a1,std::uint64_t attacker,std::uint32_t a3,std::uint32_t t0,float f12)> ignite;
    // 00188140(target, &origin, &direction, mode, &record): the hit volumes
    // (the record is a stack block of which only +4c is set; its result is
    // not read).
    std::function<void(std::uint64_t target,const RootVector& origin,const RootVector& direction,std::int32_t mode,float record_4c)> hit_volumes;
    // Target virtual +100(target, attacker, category, type, &penetration, 0, 0, amount).
    std::function<void(std::uint64_t target,std::uint64_t attacker,std::uint8_t category,std::uint8_t type,std::int32_t& penetration,float amount)> damage;
};
inline constexpr std::uint32_t melee_hit_los_mask=0x58;
inline constexpr std::uint32_t melee_knockout_statistic=6;
struct MeleeHitResult {
    enum class Outcome:std::uint8_t {
        none,           // a return before the class branch (mode, record, doKnifeDmg, player target, LOS)
        refused,        // the class branch returned (life 2 / 3, not human, +346a)
        stunned,        // 00184ce0
        ignited,        // 00186360
        damaged,        // virtual +100
    };
    Outcome outcome{};
    bool statistic{};                          // 00333f10 was called
    bool hit_volumes{};                        // 00188140 was called
};
MeleeHitResult apply_melee_hit(const MeleeHitInput& input,const MeleeHitHost& host);

}
