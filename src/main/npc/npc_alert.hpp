#pragma once
// Alert behaviour of the AI component beyond Patrol (NPC_COMBAT.md sections
// 2.4, 3 and 4, milestone N4b): mode 1 (00156d00) with its decision, the
// travel speed by purpose (component +d4 = 0014d730), the hearing reaction
// of the dispatcher (00152c50) with the look requests it makes (+128 =
// 0014dfb0, the active-entry query +a8 = 0014d390), the dispatcher's AI
// event 4 (look entry release), hearing (+d0 = 00152fd0) and the AI Sound
// System's handler 0016adb0, and the AI script natives AddAwareness /
// AddAwarenessOfPlayers (+e4 / +ec), SetTarget, ResumeAITargeting,
// ResumeAIMode (+e0 = 0014b570), SetMode (+dc = 0014bcf0) and LookAround
// (+134 = 0014dd50). Single player. What needs node searches, routes
// (N5a/N5c), projectiles (N5d) or the look-around presentation throws.
#include "npc_ai.hpp"
#include "npc_perception_think.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace omega {

// 00142b50(inventory): 0 while the weapon mode +84 is 6 (nothing held),
// else the held item's record +64 (WeaponRecord::damage_mid_64) through
// cvt.s.w, x 1.0 (00489c10) and cvt.w.s.
std::int32_t npc_weapon_rating(const WeaponInventory& inventory,const WeaponDatabase& database);
// The EE cvt.w.s (truncation; saturating outside the int range). NaN throws.
std::int32_t npc_cvt_w_s(float value);

// Mode 1 (00156d00) through the mode dispatch: AI events 0 (enter) and 3
// (timer): nothing unless the path follower's region +5c >= 0; then R =
// aggressiveness +6c x cvt.s.w(rating) and t = the target's threat
// (0016c730, 0 without a target): t > R -> the cover search 00150150 and the
// pair {1, 2}; else aggressiveness < 0.9 -> the firing-position search
// 0014fec0 and the pair {1, 3} (each pair set after its search, which can
// answer at once; npc_cover.hpp, N5c); else with floor contact +158 a
// request of mode 4 (priority 0). Returns "handled":
// 1 for events 0 and 3, 0 otherwise. Path events: 2 with tag +110 == 1 ->
// request mode +112; 1 / 3 -> mode 5 when 001552b0 (floor contact and the
// target, if any, is actor class), else +e8 = 60; 5 / 6 -> +e8 = 1.
bool npc_alert_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// 001552b0: floor contact +158, and the perception's target (when it
// resolves) is an actor-class object.
bool npc_search_allowed(const NpcAiComponent& c,NpcAiHost& host);
// Component +d4 = 0014d730(c, purpose): one draw r of the game RNG;
// i = cvt.w.s(((+6c + 3.07336e-10 (00489df0) x cvt.s.w(r)) - 0.33) x D / 5),
// clamped to [0, 3], D = 00160130 (the distance to the route end; 0 while
// the path follower's route buffer +74 is null, i.e. before Activate; with a
// buffer no route was written to it is heap content: |translation| for an
// actor loaded with the level (NpcAiHost::level_loaded, measured,
// npc_unwritten_route_distance), otherwise it throws); the speed enum = the purpose's table (00489df8..
// 00489e18 by the jump table 0049b680): 1 {Run, Jog, Run, Jog}, 2 and 3
// {Walk, Jog, Jog, Run}, 4 {Jog, Jog, Run, Run}, 5 and 6 {Jog, Run, Jog,
// Run}, 0 and others Walk. While +64 <= 0: +60 = its speed (virtual +148),
// +64 = 0, +141 bit 6 clear. Always the draw request +bc(1, 20).
void npc_travel_speed_for_purpose(NpcAiComponent& c,std::uint8_t purpose,NpcAiHost& host);
// Component +128 = 0014dfb0(c, object, priority, duration): nothing when
// the priority is below entry 0's (+178); +80 = 0 and nothing else when it
// is below the active entry's (+164); otherwise the active entry is
// released when active (+a8) and not above the priority, +164 = 0, entry 0 =
// {object, its ID or -1, duration, priority, active 1, flag 1}, +80 = 0.
void npc_request_look_at(NpcAiComponent& c,std::uint64_t object,std::int32_t object_id,std::int32_t priority,float duration);
// 0014dea0 (virtual +12c, RequestFacial's 00149e80 with priority 100): return
// when the priority is below entry 2's own (+1a0) or the active entry's
// (+178 + 20 * +164; +80 is not touched); otherwise the active entry is
// released when active (+a8 = 0014d390) and not above the priority, +164 = 2
// and entry 2 = {object, its ID +c or -1, duration, priority, active 1, flag 1}.
void npc_request_facial_look(NpcAiComponent& c,std::uint64_t object,std::int32_t object_id,std::int32_t priority,float duration);
// The dispatcher's AI event 4 (00151e74..00151ecc): the active look entry
// +164, when its priority is below 51, is released (+178/+17c of that
// entry), +166 = 0, +160 = 0, and with +168 ResumeMovement (+158).
void npc_release_look_entry(NpcAiComponent& c,NpcAiHost& host);
// The dispatcher's AI event 7 (00151df4..00151e5c): the object by its ID
// (0040c540; nothing when it is gone) and, for causes 2, 3, 4 and 7, the
// hearing reaction 00152c50(c, cause, object).
void npc_dispatch_awareness_event(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// 00152c50(c, cause, object): see NPC_COMBAT.md section 4. +80 is advanced
// by the look-at selection 0014e0c0 (npc_attack.hpp, run by the update with
// NpcAiHost::attack); reading +80 after a look request reset it while the
// update leaves that selection out throws.
void npc_hearing_reaction(NpcAiComponent& c,std::uint8_t cause,std::uint64_t object,NpcAiHost& host);
// Component +d0 = 00152fd0(c, cause, value, source): a null source does
// nothing; cause 5 (a landed projectile) does nothing in mode 10 and throws
// otherwise (the projectile's owner, 0025ac00 and mode 10 / action 8 are
// N5d); other causes: 0016c9d0(perception, source, cause, value).
void npc_hear(NpcAiComponent& c,std::uint8_t cause,float value,std::uint64_t source,NpcAiHost& host,std::vector<NpcPerceptionSend>& out);

// An AI sound message (key 004ee5c8, to the AI Sound System 004ee5c0):
// +24 kind (0: at the source object's position, 1: at +40, 2: directed at
// +34), +25 cause, +2c radius, +30 the source's ID, +34 the directed
// target's ID, +40 the point.
struct NpcSoundMessage {
    std::uint8_t kind_24{},cause_25{};
    float radius_2c{};
    std::int32_t source_id_30{},target_id_34{};
    RootVector point_40{};
};
// A thinking NPC as the handler sees it through the think list.
struct NpcSoundListener {
    std::uint64_t actor{};
    std::int32_t id{};                           // actor +c
    std::uint8_t life_108{};
    bool ai_58{};                                // its controller's virtual +58
    float deafness_33ac{};                       // 00158540(controller) +33ac
    RootVector position{};                       // virtual +e8 row 3
    // Its component's +d0 (00152fd0) with (cause, source, value).
    std::function<void(std::uint8_t cause,std::uint64_t source,float value)> hear;
};
struct NpcSoundWorld {
    std::function<std::uint64_t(std::int32_t id)> object_by_id;       // 0040c540
    std::function<RootVector(std::uint64_t object)> position;         // virtual +e8 row 3
    // The think list 004ea190 in order; entries that do not resolve are absent
    // (nullopt), as the handler skips them.
    std::function<std::vector<std::optional<NpcSoundListener>>()> listeners;
    // Kind 2: the directed target's controller (+58 / +5c) and its +d0.
    std::function<bool(std::uint64_t object)> ai_controlled;
    std::function<void(std::uint64_t object,std::uint8_t cause,std::uint64_t source,float value)> hear_directed;
};
// 0016adb0 for a message of key 004ee5c8: nothing when the source's ID is
// not found. Kind 2: cause 7 to a directed target with an AI controller
// (value 0.0); other causes nothing. Kinds 0/1: every listener other than
// the source (by ID), not dying or dead, with an AI controller, not deaf
// (+33ac <= 0) and closer than the radius (d = |position - (source position
// or +40)|, VU) hears (cause, source, d). Cause 5 also reaches listeners
// that see the projectile's owner, which needs projectiles (N5d): throws.
void npc_ai_sound_event(const NpcSoundMessage& m,const NpcSoundWorld& w);

// The perception's sends as the component sends them (transport 0): AI
// events to the component, NewAwareness (004ff298) to the actor as a script
// notification; the dead-object notice AIPickupableAwarenessMsg (004ff2a8)
// to the component as a script notification (checkpoint 132zf7).
void npc_forward_perception_sends(const std::vector<NpcPerceptionSend>& out,NpcAiHost& host);
// 00159a4c..00159c9c, after 0016b580 on the think frame: with a resolving
// target that is not visible and clear (virtual +b4), no ForceWeaponFire
// pending (+130 0) and perception +20 bit 3, the secondary component's two
// upper-body states both firing states (0049f790 +4c), the target's sensed
// record with a clear line, age below 1.0 and cause 3 or 4, and the last
// known position +00 in view (0016c260(p, +00, head, target +44 >> 1)): the
// snap burst ForceWeaponFire(+a4, item -1, delay 0.0) (+198 = 0014b470,
// npc_attack.hpp).
void npc_snap_burst_check(NpcAiComponent& c,NpcAiHost& host);

// Script natives (registrations listed in SCRIPT_HOSTING.md section 3).
// AddAwareness(NPC, GOBJ) 0014a410 -> +e4 = 0014a450: 0016c9d0(perception,
// object, 1, 0.0). AddAwarenessOfPlayers(NPC) 00149420 -> +ec = 0014aec0:
// +e4 for every player slot 003fb8d0(0..3) that is not empty.
void npc_add_awareness(NpcAiComponent& c,std::uint64_t object,NpcAiHost& host,std::vector<NpcPerceptionSend>& out);
void npc_add_awareness_of_players(NpcAiComponent& c,NpcAiHost& host,std::vector<NpcPerceptionSend>& out);
// SetTarget(NPC, GOBJ) 0014a210: 0016ddb0(perception, object, 100).
// ResumeAITargeting(NPC) 0014a1c0: perception +1c = 0.
void npc_script_set_target(NpcAiComponent& c,std::uint64_t object,NpcAiHost& host);
// RequestFacial(NPC, GOBJ, float) 00149e80: 003cc820 (identity) of the GOBJ,
// then virtual +12c = 0014dea0(c, object, 100, the float's bits as f12); the
// object's ID +c (-1 for a null object) from the perception's views.
void npc_script_request_facial(NpcAiComponent& c,std::uint64_t object,float duration,NpcAiHost& host);
// RequestLookAt(NPC, GOBJ, float) 00149ed0: 003cc820 (identity) of the GOBJ,
// then virtual +128 = 0014dfb0(c, object, 100, the float's bits as f12)
// (npc_request_look_at); the ID +c (-1 for a null object) from the views.
// ResumeAILookAt(NPC) 00149df0 -> +13c = 0014de30(c, 100): every look entry
// 0..2 that is active (+17c + 20 i) with exactly that priority (+178 + 20 i)
// is released (both cleared); +164 and +80 are untouched.
// DeactivateDead(NPC, int) 00149fe0 -> +160 = 0014b680(c, int != 0): +141
// bit 2 = the flag (read by the corpse removal, npc_corpse_removable_164);
// the multiplayer send 002d7290 (*005721c8) is skipped in solo.
// (tests/script_look_at_goldens.hpp, tools/ee/gen_script_look_at.py.)
void npc_script_request_look_at(NpcAiComponent& c,std::uint64_t object,float duration,NpcAiHost& host);
void npc_resume_look_at(NpcAiComponent& c,std::int32_t priority);
// Checkpoint 132sx10: the grenade target virtual +19c = 0014b410(c, object,
// f12 scatter, a2 flag): +1c8 = object, +1cc = object ? its +c : -1, +90 =
// scatter, +141 bit 4 = a2 & 1 (npc_attack.cpp reads all four at the throw).
void npc_set_grenade_target(NpcAiComponent& c,std::uint64_t object,std::int32_t id,float scatter,std::uint32_t flag);
// SetGrenadeTarget(NPC, GOBJ) 00149810: 003cc820 (identity), then +19c(c,
// object, 4.0f, 0).
void npc_script_set_grenade_target(NpcAiComponent& c,std::uint64_t object,NpcAiHost& host);
void npc_deactivate_dead(NpcAiComponent& c,bool deactivate);
void npc_resume_ai_targeting(NpcAiHost& host);
// ResumeAIMode(NPC) 0014a470 -> +e0 = 0014b570: when +4c > 20: +4c = 0, then
// a request of mode 1 (priority 0) with a resolving perception target, else
// mode 0.
void npc_resume_ai_mode(NpcAiComponent& c,NpcAiHost& host);
// ResumeAIMode through a kept AI pointer to a deleted NPC (a deferred call
// that fires after 0040bf50 -> 001b57c0 -> 003ccf30 freed the component;
// GENERATOR_RUNTIME.md "Checkpoint 132x"): the teardown's base chain
// 00158550 -> 0016f550 -> 002301e0 -> 003ea5b0 -> 003cb330 leaves the root
// vtable in +0, the free 0036b130 -> 0036bc10 writes only the block header
// and footer, so 0014a470 calls the root's +e0, which returns 0 and writes
// nothing (tests/freed_ai_resume_goldens.hpp). Holds while no later
// allocation has taken the freed block, which the port does not model.
inline constexpr std::uint32_t npc_freed_ai_component_vtable=0x4dfde0;  // 003cb330
inline constexpr std::uint32_t npc_freed_ai_resume_slot=0x15bff0;       // 004dfde0 +e0: jr ra; v0 = 0
// The same holds for SetMode 0014a4f0 (+dc = 00148870: jr ra; v0 = 0) and
// AddAwarenessOfPlayers 00149420 (+ec = 0015c000: jr ra), each writing
// nothing (Checkpoint 132zf5, freed_ai_natives goldens).
inline constexpr std::uint32_t npc_freed_ai_set_mode_slot=0x148870;     // 004dfde0 +dc
inline constexpr std::uint32_t npc_freed_ai_awareness_of_players_slot=0x15c000;   // 004dfde0 +ec
// SetMode(NPC, eMode) 0014a4f0 -> +dc = 0014bcf0(c, mode, 0): for mode & 0xff
// below 7 a request at priority 100; +13f and the route to a node are only
// for a non-null node argument, which SetMode does not pass.
void npc_set_mode(NpcAiComponent& c,std::uint32_t mode);
// LookAround(NPC) 00149e50 -> +134 = 0014dd50(c, 100): npc_look_around
// (npc_search.hpp).
}
