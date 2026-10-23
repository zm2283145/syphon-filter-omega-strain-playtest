#pragma once
// The attack and firing loop of the AI component (NPC_COMBAT.md sections 3,
// 5.2 and 5.3, milestone N5b): the aim update the per-frame update 00159590
// runs through component virtual +60 (0022f5b0 on the component's aim block,
// weapon_aim.hpp), the look-at selection 0014e0c0 (its look-around branch,
// +166 set, is npc_search.hpp's static npc_look_around_select), the weapon posture 00150ca0, the firing gate 00150340, the fire
// notice 0014f460 (virtual +cc, from the discharge), ForceWeaponFire 0014b470
// (virtual +198), the combat posture decision 001570b0, mode 4 Attack
// 00155350 and SetAggressiveness 0014b600. Interpreter goldens:
// tests/npc_attack_goldens*.hpp, tests/npc_fire_goldens*.hpp. Single player
// (the multiplayer tails reading 005721c8 are not modelled).
//
// Every outgoing message is an NpcAiSend through NpcAiHost::send in send
// order: actor actions (key 0055d480; the posture's 0x39 kind 4 / 0x3a / 0x3e
// kind 5 / 0x3f, the look-at selection's 0x3d kinds 1 / 4 / 5, the gate's 0x3b
// kind 0xb, Attack's reload 0x3c; all with +20 = 1 and transport 0) and the
// weapon-fired notice (key 004ff2a0) to the component.
#include "npc_ai.hpp"
#include "npc_perception.hpp"
#include "npc_perception_think.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

inline constexpr std::uint32_t npc_script_weapon_fired=0x004ff2a0;   // AIWeaponFiredMsg (0026f110)

// A call of the fire notice +cc made inside a discharge: the request's target
// and the continuation flag.
struct NpcFireNoticeCall {
    std::uint64_t target{};
    bool continuation{};
};

// What the N5b stages read beyond the component, its perception and the
// perception's object views (NpcAiHost::sense).
struct NpcAttackHost {
    std::uint32_t actor_id{};                                  // actor +c
    // Virtual +c0(object, subtarget) of an object the perception names.
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> eye_c0;
    // Virtual +f4 row 3 of an object (the throw target).
    std::function<RootVector(std::uint64_t object)> position_f4;
    // The actor's +3524 inventory: the held slot +84 (6: nothing) and its
    // item (+8 + 16 x +84, read for slot 6 too), +a0 (reloading), 00141240
    // (can attack), 001437a0 (hold class), 00143400(inv, 0) (ready),
    // 00143820 (full auto) and 00143940(inv, 6) (the held clip).
    std::function<std::uint8_t()> inventory_84;
    std::function<std::int32_t(std::uint8_t slot)> slot_item;
    std::function<bool()> reloading_a0;
    std::function<bool()> can_attack;
    // Port-only (checkpoint 132ob7, the --coop-partner's fire discipline): asked once the gate 00150340 decided to
    // fire and 00141240 / the cooldown allow it; true holds the shot (no throw, no 0x3b action, no discharge).
    // Unset (every original NPC): never held.
    std::function<bool()> port_hold_fire;
    std::function<std::int32_t()> held_class;
    std::function<bool()> ready;
    std::function<bool()> full_auto;
    std::function<std::int32_t()> clip;
    // The actor's +33e4 byte and its secondary component +3314 (nullopt:
    // none) with the states +310 / +311, and its aim slot 1 active byte +415.
    std::function<std::uint8_t()> byte_33e4;
    std::function<std::optional<std::array<std::uint8_t,2>>()> secondary;
    std::function<bool()> secondary_slot1_415;
    // The aim block: 0022f5b0(component, target, weapon item, subtarget)
    // (virtual +60) and its progress +20 / accuracy +24.
    std::function<void(std::uint64_t target,std::int32_t item,std::int32_t subtarget)> aim;
    std::function<std::array<float,2>()> aim_block;
    // 0013ff40(inv, 0, -1, 0, 0), the gate's direct discharge; returns the
    // fire notices the discharge sent (the gate applies them at once, as the
    // original's synchronous +cc call).
    std::function<std::vector<NpcFireNoticeCall>()> discharge;
    std::function<std::uint32_t()> random;                     // 00127dd0
    // Attack's relocation: 00163fd0(route manager 004ed9b0, actor) (the
    // queue head is this actor's and pending) and 0015fa70(+1b0, point,
    // region) (npc_route.hpp).
    std::function<bool()> route_head_pending;
    std::function<void(const RootVector& point,std::int32_t region)> route_to_point;
    // N5c, mode 3 AttackFromCover (npc_cover.hpp): 00143560(inv) (a reload
    // can start; not native) and 00142b50 of an actor-class object's
    // inventory +3524 (its weapon rating).
    std::function<bool()> reload_allowed;
    std::function<std::int32_t(std::uint64_t object)> weapon_rating_of;
    // N5c, mode 2 TakeCover (npc_cover.hpp): virtual +1ac = 0014bae0(c, item)
    // (npc_select_item with the actor's inventory, id and the game RNG).
    std::function<void(std::int32_t item)> select_item;
};

// The update's aim stage (00159878..001599f4): the perception target when it
// is visible and clear (perception +20 bits 0 and 2), present and this NPC is
// not dying or dead, else none; the held item (+84 != 6) or -1; then virtual
// +60(c, target, item, +11c).
void npc_update_aim(NpcAiComponent& c,NpcAiHost& host);
// 0014e0c0(c, dt), the look-at selection (+167 = 0 first). A running
// look-around (+166, set by 0014f240) first runs npc_look_around_select
// (npc_search.hpp; static, no goldens). Each active
// look entry (0..2) whose duration +174 is positive and whose object resolves
// (and, for an actor-class object, is present) counts its duration down by
// dt; any other active entry is released (active and priority 0). Then with
// entry 0 active and resolving: when its flag +17d is set, a head look 0x3d
// (kind 4 at its eye +c0(-1) raised by 4.5 for an object of class *004ea770,
// else kind 5 with its ID), +1a8 = 1 and the flag cleared; otherwise with
// +1a8 set 0x3d kind 1 (+2c = byte 00489de0 = 0) and +1a8 = 0, or, with the
// secondary component's aim slot 1 active (+415), 0x3d kind 1 (+2c = byte
// 00489de8 = 0). Finally +80 += dt. The ageing of the entries is
// npc_age_look_entries (all three, entry 2 included).
void npc_look_at_selection(NpcAiComponent& c,float dt,NpcAiHost& host);
// 0014e5e8..0014e828: for each look entry 0..2 that is active, duration -= dt
// when the duration is above 0 (c.ole.s) and its weak handle resolves to an
// object that is not of the actor class (+4c != *0049d010) or is present
// (+2d, +2c, !+2e); otherwise active = 0 and priority = 0.
void npc_age_look_entries(NpcAiComponent& c,float dt,const NpcPerceptionHost& h);
// 00150ca0(c), the weapon posture; returns the raise decision (unused by the
// update). See NPC_COMBAT.md section 5.2.
bool npc_weapon_posture(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host);
// 00150340(c, dt), the firing gate. See NPC_COMBAT.md section 5.3.
void npc_firing_gate(NpcAiComponent& c,NpcAiSystem& system,float dt,NpcAiHost& host);
// 0014f460(c, target, continuation), the fire notice (component virtual +cc)
// the discharge 0013ff40 calls for an AI shooter. When the forced count runs
// out with a ForceWeaponFire item kept (+12c >= 0), it selects that item again
// (virtual +1ac = 0014bae0, NpcAttackHost::select_item) and sets +12c = -1
// (132zf5).
void npc_fire_notice(NpcAiComponent& c,NpcAiSystem& system,std::uint64_t target,bool continuation,NpcAiHost& host);
// 0014b470(c, shots, item, delay) (virtual +198): +12c = -1 for item < 0 or
// the held item (+1a4); a different item keeps the held item in +12c, then
// selects the item (+1ac = 0014bae0, NpcAttackHost::select_item; 132zf5);
// +130 = shots; +134 = cvt.w.s(delay), or cvt.w.s(+8c) for a delay of 0.0.
void npc_force_weapon_fire(NpcAiComponent& c,std::int32_t shots,std::int32_t item,float delay,NpcAiHost& host);
// 0014b470's item < 0 path (no host read): what the script native
// ForceWeaponFire (00149bf0: a1 = shots, a2 = -1, f12 = 0.0) reaches.
void npc_force_weapon_fire_stores(NpcAiComponent& c,std::int32_t shots,float delay);
// 001570b0(c): 2 (take cover), 3 (attack from cover) or 4 (attack).
std::uint8_t npc_combat_posture(NpcAiComponent& c,NpcAiHost& host);
// Mode 4 Attack (00155350) through the mode dispatch; returns "handled".
// AI event 8 with the posture 2: the cover search 00150150 and +110/+112 =
// 4/2 (not handled). Event 3 with the region not allowed: the approach search
// 0014fd30 (at 001555f0 instead of relocating, at 00155850 instead of the
// route to the last known position). Both through npc_cover.hpp.
bool npc_attack_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// SetAggressiveness (virtual +110 = 0014b600): +6c = value.
void set_npc_aggressiveness(NpcAiComponent& c,float value);
// Activate's look slot (00157ed0..00157f40): the first slot of 004ea1d0 not in
// use is taken (+14 = 1), else slot 0 is shared; +1b8 = that slot and
// 00158980 empties its list and clears +c and +10. Deactivate (00157bc0..
// 00157bf0): its list emptied and +14 = 0.
void npc_take_look_slot(NpcAiComponent& c,NpcAiSystem& system);
void npc_release_look_slot(NpcAiComponent& c,NpcAiSystem& system);
// The component's +84 (burst pause) and +88 (shot cooldown) floats.
float npc_burst_pause_84(const NpcAiComponent& c);
float npc_shot_cooldown_88(const NpcAiComponent& c);
}
