#pragma once
// Jump to grab, the ledge catch, the hang and the pull-up / drop actions of
// the actor (see the hanging notes, HANGING.md):
//   001914c0  actor action 0x14 (self-sent by 001930a0 for mask bits 5/6);
//   001a1c80  ledge catch: DATA_ID_JumpGrabLedge -> sliderHang;
//   001a2470  jump launch toward the grab point (+3490 == 2), with 001a2b20
//             and the ballistic solver 003a8c80 (solve_projectile_trajectory);
//   0019f6c0  per-frame priority history (0019f7f0/0019f7c0, the 0019f890
//             reactions, the 00189590 cleanup loop) and 001a2c00 (launch
//             request, catch test, fall/land tests, airtime, 0019f450,
//             0019e670 and the slot-5 request);
//   001ab1d0 / 001ab610  actor actions 0x11 (pull up) and 0x13 (drop);
//   001a1590 / 001a0b30  the fall and the landing.
// Branches that reach code that is not native throw before any effect of
// that branch (the fall and landing hosts of the air state throw when empty).
#include "actor_animation_receiver.hpp"
#include "actor_edge_types.hpp"
#include "actor_motion.hpp"
#include "actor_physical_state.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
// Actor fields of the jump (all zero after construction in the solo host).
struct ActorJumpState {
    std::uint8_t kind_3490{};          // 2: jump to grab (001914c0), 1: running jump
    RootParentTransform grab_34a0;     // +34a0/+34b0: 'edge' root world placement at the request
    RootVector target_34c0{};          // +34c0: launch target (W written as 1)
    RootVector arrival_34d0{};         // +34d0: velocity at the target (z read by 001a2c00)
    float airtime_30a0{};              // +30a0: time unsupported
    float fall_start_30a4{};           // +30a4: flor z when the fall started (0019f450)
    float fall_height_30a8{};          // +30a8: last landing drop above 0 (0019f450)
};
// Physical fields read or written here (the caller copies them in and out).
struct ActorJumpPhysical {
    bool present{};                    // actor +3310
    ActorPhysicalMode pending;         // +28/+2c
    std::uint8_t keep_contact_9c0{};   // +9c0
    float radius_374{};                // +374
    RootVector override_9d0{};         // +9d0: velocity override consumed by the next prediction
    RootVector velocity_310{},force_330{};
    float inverse_mass_2e4{};
    std::uint8_t enabled_12{},keep_velocity_13{},unsupported_dc{},supported_dd{};
    float floor_2c4{};                 // +2c4: height above the floor
    // +1f4: the moving-support slot 1 object; with a +54 component, 001a2a94
    // (Checkpoint 132fx1) adds that component's +a0 velocity (xyzw) to the
    // launch velocity and to +34d0. nullopt: no slot-1 object or no +54.
    std::optional<RootVector> support_velocity_a0;
};
// Motion installs through the actor's controller (0018c5a0 name lookup and
// 003ad500 on +2ef4; the retained motion +326c through 001ae270/001ae260).
// resolve is the node-name map (model +0x24); resolve_root is the named-root
// registry (model +0x44, the 12-byte {node, group pair} records registered by
// 003b4640) that the fall 001a1590 and the landing 001a0b30 search through
// 001926d0/001926c0 (001badd0 map find, no end check): "blendFall",
// "blendLand" and the other blend* names are registered roots, not node names.
struct ActorHangMotionHost {
    std::function<ActorMotionRegistration(std::string_view)> resolve;
    std::function<ActorMotionRegistration(std::string_view)> resolve_root;
    std::function<ActorMotionRegistration()> retained;
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
struct ActorHangInstall {
    std::string name;                  // "" = the retained motion
    MotionInstallMode mode{};
    bool operator==(const ActorHangInstall&) const=default;
};

// 001914c0 for actions 0x14..0x16. Life state 2/3 or mode +320c 4 or 5
// leave everything unchanged; so do other actions and wrong message kinds.
// Action 0x14 (kind 14): h = edge z - flor z (00132400/00132370): below 10.0
// "jump_straight_up_for_grab_8_to_10_feet_longer", otherwise
// "..._10_to_12_feet_longer", replace, {0.2, 0.2, 5, 5}, tokens
// {9, 10, -, 13, -, 14, 15}, phase hooks [0.5, 0.5] -> 0x0b and
// [0.5, 0.7] -> 0x0c; then "jump_idle_in_air", queue, {0.2, 0.2, 5, 5},
// tokens {0x10, -, -, 0x11, -, 0x12, 0x13}. Priority action 4, +3248 |=
// 0x400 and bits 0x800/0x1000/0x2000 cleared; +3490 = 2 and +34a0/+34b0 =
// the edge placement are stored before the installs.
// Actions 0x15/0x16 (kind 4, the running jump of 00192800; single player, so
// the 0x15 multiplayer echo 00191b50 is not sent): 0x15 sets +3490 = 1 and
// +34b0 = the payload as sent; 0x16 sets +3490 = 2, +34a0 = (0, 0, sin(w/2),
// cos(w/2)) (00121928/00121688) and +34b0 = (payload.xyz, 1). Then priority
// action 4; +3248 |= 0x800 (0x15) or 0x1000 (0x16), clearing the other, and
// 0x400/0x2000 cleared. Lead foot +31dd == 1 selects the _Right motions,
// otherwise _Left, both through the named-root registry (001926d0/001926c0,
// no missing-name check in the original): "blendRunningJumpTakeOff_*"
// replace, {0.2, 0.2, 5, 5}, tokens {9, 10, -, 13, -, 14, 15}, phase hooks
// [0.8, 0.8] -> 0x0b and [0.8, 0.8] -> 0x0c; "blendRunningJumpInAir_*"
// queue, {0.2, 0.2, 5, 5}, tokens {0x10, -, -, 0x11, -, 0x12, 0x13}.
struct ActorJumpGrabRequest {
    std::uint8_t life_state{};
    std::int32_t action{0x14};
    std::uint32_t kind{14};
    float floor_z{};                   // 'flor' root world z (0x14)
    RootParentTransform edge;          // 'edge' root world placement (0x14)
    RootVector payload{};              // message +0x30 (0x15/0x16)
    std::uint8_t lead_foot_31dd{};     // actor +31dd (0x15/0x16): 1 right, otherwise left
    std::uint64_t receiver{};
};
struct ActorHangResult {
    bool handled{};
    std::vector<ActorHangInstall> motions;
};
ActorHangResult handle_actor_jump_to_grab(ActorAnimationReceiverState& actions,ActorJumpState& jump,
    const ActorJumpGrabRequest& request,const ActorHangMotionHost& host);

// 001a1c80. Returns without effect in actions 7/8. Action 5 is cleared
// through 00189590 (+9c0 = 0, +374 = 004a03e0). Action 7, then
// DATA_ID_JumpGrabLedge (replace, width 0.25 when +3248 bit 10 else 0.3,
// tokens {0x14, 0x15, -, 0x16, -, 0x17, 0x18}) and sliderHang (queue,
// {0.5, 0.5, 2, 2}, tokens {0x19, -, -, 0x1a, -, 0x1b, 0x1c}), physical
// bits 5 then 1, the sound M_SHIMMY_ON (bank 1, sent immediately to the
// sound manager 004ffb50) and a timed event (004da870, byte +10 = 1: the
// same sound again) after 20 + (int)(21 * rand() / 2^31) ticks (00127dd0 and
// the scheduler 003ea1d0; actor_timed_event below, drawn by the caller).
struct ActorLedgeCatchResult {
    bool caught{};
    std::vector<ActorHangInstall> motions;
    std::vector<std::string> sounds;   // immediate sound requests (bank 1)
    int delayed_sounds{};              // scheduled 004da870 events
};
ActorLedgeCatchResult catch_actor_ledge(ActorAnimationReceiverState& actions,ActorJumpPhysical& physical,
    std::uint64_t receiver,const ActorHangMotionHost& host);

// 001a2b20(point, orientation, position): VU rotation of point.xyz by the
// quaternion (point.w kept), then + position.xyz when point.w == 1, otherwise
// + (position * point.w).xyz with w = position.w * point.w.
RootVector place_actor_local_point(const RootVector& point,const RootVector& orientation,const RootVector& position);

// 001a2470. position: actor virtual +e4 (00132860, the model anchor);
// transform_position: actor +50 component +40; clip_start: the start
// placement of the kind's motion (00190f50 + 001901c0, see
// resolve_motion_start_placement), nullopt when the name is not a leaf
// (identity placement, 00190060).
// +3490 == 2 (DATA_ID_JumpGrabLedge): +374 = 0.5 |start.y|; target =
// 001a2b20(start.position + (0, -0.45, 0, 0), +34a0, +34b0) + (position -
// transform_position), W = 1; arc = 0.5 - (12 - (grab z - position z)) *
// 0.0225 clamped to [0.1, 1] with +3248 bit 10, else [0.5, 1].
// +3490 == 1 (animRunningJumpLand, 001a2778): +374 unchanged; target =
// ((+34b0 + rot(flor, start.position)) + (position - transform_position)) +
// 0 over xyzw, then W = 1 (+34cc); arc = 0.5 - (9 - (target z - position z))
// * 0.0225 clamped to [0.1, 1].
// Both: 003a8c80(position, target, g = 004f5438, arc, pi/4) gives +9d0 and
// +34d0; +9c0 = 1. Without a physical component nothing happens; other
// +3490 values throw (the original solves with arc 0 on a stale target).
struct ActorJumpLaunchInput {
    RootVector position{},transform_position{};
    std::optional<RootParentTransform> clip_start;
    std::uint32_t flags_3248{};
    RootVector flor_orientation{0,0,0,1};  // 'flor' world orientation (00132400 -> 001323f0), kind 1
};
void launch_actor_jump(ActorJumpState& jump,ActorJumpPhysical& physical,const ActorJumpLaunchInput& input);
inline constexpr std::uint32_t game_gravity_bits=0xc200bd7bU;    // 004f5438 after initialization (-32.185)

// 0019f6c0 history: +3234 <- +3220 <- {+320c top, +3210 bits, +3218 changes}
// (0019f7f0), then +3218 = 0 (0019f7e0); +3258 <- +3250 <- +3248/+324c
// (0019f7c0).
struct ActorPrioritySnapshot {
    std::int32_t top{-1};
    std::uint32_t bits{},changes{};
    bool operator==(const ActorPrioritySnapshot&) const=default;
};
struct ActorActionHistory {
    ActorPrioritySnapshot older_3234,newer_3220;
    std::array<std::uint32_t,2> flags_3250{},flags_3258{};
    bool operator==(const ActorActionHistory&) const=default;
};
// A 0019f450 call of actor virtual +100 (damage type and amount).
struct ActorFallDamage {
    std::int32_t type{};
    float amount{};
};
// One 0019f450 call: its inputs (fall start before, 'flor' z, the actor
// state it reads) and outputs (+30a4/+30a8 after, the damage call).
struct ActorFallStateTrace {
    bool physical_present{},unsupported_dc{},first{},byte_14{},byte_33a5{};
    float fall_start_before{},floor_z{},health_40{};
    std::int32_t health_3398{};
    std::uint32_t active_actions{};
    std::uint8_t life_state{};
    float fall_start_after{},fall_height_after{};
    std::optional<ActorFallDamage> damage;
};
struct ActorAirHost {
    std::function<void()> launch;      // 001a2470 (action 4 with +3248 bit 14)
    std::function<void()> catch_ledge; // 001a1c80 (action 5, +3248 bit 10)
    std::function<void()> fall;        // 001a1590 (fall_actor; empty throws)
    std::function<void()> land;        // 001a0b30(actor, 1) (land_actor; empty throws)
    std::function<float()> floor_z;    // 'flor' world z (0019f450)
    // Actor virtual +100 (damage) as 0019f450 calls it: (actor, actor, 1,
    // type, 0, 0, 0, amount); type 0xf in the air, 0xc on landing
    // (apply_actor_damage in the solo host); empty throws when reached.
    std::function<void(std::int32_t type,float amount)> damage;
    // Queued player-component message (class 0055a1d8, send mode 0): code 7
    // clears input pending priority bit 7 (00176660 case 7).
    std::function<void(std::int32_t)> component_message;
    // Leaving action 16 (the ladder, 0019fbd8..0019fc38): code 12 sent to the
    // player component at once (class 0055a1d8, transport 1; 00176c88 clears
    // input pending bit 6), then 00143cf0 on +3524 (the inventory restore).
    std::function<void(std::int32_t)> component_message_immediate;
    std::function<void()> restore_inventory;
    // Leaving action 21 (the body carry, 0019f8a4..0019f96c, before the action
    // 17/18 test): leave_actor_carry (actor_body_carry.hpp). Empty throws when reached.
    std::function<void()> leave_carry;
    // Leaving action 17 or 18 (the buddy-climb idles, 0019f970..0019fb48)
    // unless action 19 is now set: the CAS code (0xd with life state 2/3,
    // else 0xf; +28 = -1, +2c = the actor, event byte 1) sent at once
    // (transport 1) to the object +3474, which is then cleared. Empty throws
    // when reached.
    std::function<void(std::uint64_t object,std::int32_t code)> object_code_immediate;
    // 0019e670(actor, 0) after 0019f450 when supported with action 0x1d or
    // 0x1e (the dead landing, actor_death.hpp); empty throws when reached.
    std::function<void()> land_dead;
    // Optional: receives each 0019f450 call's inputs and outputs (diagnostic).
    std::function<void(const ActorFallStateTrace&)> trace_fall_state;
};
struct ActorAirInput {
    float elapsed{};
    std::uint8_t life_state{};         // actor virtual +108
    // Actor +14: set by the root object constructor (003cb0e0) for every
    // locally owned object, including the single-player player; cleared only
    // for network replicas (LADDERS.md, "Actor +14").
    bool byte_14{};
    bool component_is_player{true};
    // Read by 0019f450 only with +14.
    float health_40{};                 // +40 (truncated to an integer)
    std::int32_t health_3398{};        // +3398 (integer, scales the partial damage)
    bool byte_33a5{};                  // +33a5 (lower thresholds, like action 21)
};
struct ActorAirResult {
    std::optional<ActorFallDamage> damage; // 0019f450 (actor +14)
    bool component_code7{};            // 0019f890 queued code 7
    bool component_code12{};           // 0019f890 sent code 12 (left the ladder)
    std::optional<std::int32_t> object_code; // 0019f890 sent this code to +3474 (left action 17/18)
    bool action5_cleared{};            // 00189590(5) from the 0019f724 loop
    bool launched{},caught{},slot5_latched{};
};
// 0019f6c0 then 001a2c00. Throws for paths that reach code that is not
// native: the 0019f890 reaction to action 21, and empty
// fall/land/damage/land_dead hosts. 0019f450 (with actor +14) calls the
// damage host (virtual +100, actor_damage.hpp); 0019e670 is the land_dead host.
ActorAirResult update_actor_air_state(ActorAnimationReceiverState& actions,ActorActionHistory& history,
    ActorJumpState& jump,ActorJumpPhysical& physical,std::array<ActorEdgeSlotState,9>& slots,
    const ActorAirInput& input,const ActorAirHost& host);
// 0019f450(actor, first): with actor +14 the fall damage (drop = +30a4 -
// 'flor' z; virtual +100 type 0xf above 100 in the air, type 0xc above 16
// (12 with action 21 or +33a5) on landing, +30a8 = drop above 0), then +30a4
// ('flor' z on the first unsupported frame, -FLT_MAX when supported).
// Returns the damage call it made (through host.damage; empty throws).
std::optional<ActorFallDamage> update_actor_fall_state(const ActorAnimationReceiverState& actions,ActorJumpState& jump,
    bool physical_present,bool unsupported_dc,bool first,const ActorAirInput& input,const ActorAirHost& host);

// Actor actions 0x11 (001ab1d0) and 0x13 (001ab610) in a single-player game
// (005721c8 clear, so the multiplayer mirror message is not sent).
// 0x11: pulling_up_from_ledge (replace, {0.5, 0.5, 2, 2}, tokens
// {0x2e, 0x2f, -, 0x30, -, 0x31, 0x32}) and the retained motion (queue,
// {0.5, 0.5, 2, 2}, no tokens), priority action 1, physical bits 5 and 3.
// Accepted in modes 7/8 when 001e9290 holds, otherwise only for a network
// replica of a player (actor +14 clear, player component); rejected (also
// for life state 2/3 or without a physical component) with the component
// message 0055a1d8 code 9, sent immediately, which 00176660 accepts without
// effect. The single-player player has +14 set, so only the hang accepts.
// 0x13: accepted in actions 7/8 or for a player replica; clears +3248 bits
// 10..13 and calls 001a1590 (fall).
struct ActorHangActionInput {
    std::uint8_t life_state{};
    bool byte_14{};                   // actor +14 (see ActorAirInput)
    bool component_is_player{true};
    std::uint64_t receiver{};
};
struct ActorPullUpResult {
    bool handled{},rejected{};
    std::vector<ActorHangInstall> motions;
};
ActorPullUpResult handle_actor_pull_up(ActorAnimationReceiverState& actions,ActorJumpPhysical& physical,
    const std::array<ActorEdgeSlotState,9>& slots,const ActorHangActionInput& input,const ActorHangMotionHost& host);
// 001e9290: the first of slots 6, 5, 3 with its result byte set decides:
// its mask bit 4. False when none is set.
bool actor_hang_slot_climbable(const std::array<ActorEdgeSlotState,9>& slots);
// Returns whether the drop was accepted (and the fall host called).
bool handle_actor_hang_drop(ActorAnimationReceiverState& actions,const ActorHangActionInput& input,
    const std::function<void()>& fall);

// 001a1590, the fall. Mode +320c 0x1b or 0x1f: action 0x1d only. Action 10
// (already falling): nothing. Otherwise the horizontal velocity (physical
// +310 with z = 0, or the zero vector 004e1d90 without a physical component)
// is projected on the horizontal forward of the actor's +50 transform
// component (matrix row +20 with z = 0, normalized with VU RSQRT); above 10.0
// with +3248 bits 10..12 clear sets bit 13. The motion is "blendFall" except
// for +3248 bit 12 without bits 11/13 ("blendJumpUpInAir"), looked up through
// 001926d0/001926c0 (no missing-name check in the original: an unresolved
// name throws here) and installed replacing, {0.5, 0.5, 2, 2} from 0049d038,
// tokens {0x21, -, -, 0x22, -, 0x23, 0x24}, limit -1, phase 0.
struct ActorFallInput {
    bool physical_present{};
    RootVector velocity_310{};
    RootVector forward_row{};          // actor +50 component +20 (local Y image)
    std::uint64_t receiver{};
};
struct ActorFallResult {
    bool installed{};
    std::vector<ActorHangInstall> motions;
};
ActorFallResult fall_actor(ActorAnimationReceiverState& actions,const ActorFallInput& input,const ActorHangMotionHost& host);

// 001a0b30(actor, 1), the landing (001a2c00). Life state 2/3: nothing.
// Action 11 active: no installs. Otherwise, airtime +30a0 == 0 gives
// "blendLandFromShortFallFromHang" with 0.35 (0x3eb33333) for it and for the
// retained motion, otherwise "blendLand" with 0.2 (0x3e4ccccd) and the
// retained motion with 0049d038 (0.5); the first replaces (tokens {0x25, -,
// -, 0x26, -, 0x27, 0x28}), the retained motion +326c is queued without
// tokens. Then always the landing effect 00188fb0(actor, 2, 1) (surface
// sounds and particles; it writes no actor or physical field) and a timed
// event 004da870 (byte +10 = 0, handler 001b95c0 -> 00188fb0(actor, 1, 1))
// after 70 + (int)(31 * rand() / 2^31) ticks (00127dd0, 003ea1d0;
// actor_timed_event below, drawn by the caller). The solo host plays both. The second argument 0 (001a1258, from the
// teleport tail with action 10) resets (mode 3) to the retained motion +326c
// with 0018a380(the model's default duration 0018a410) and no tokens (the
// pending +2ef9 is neither read nor consumed), asks the caller for physical
// +9c0 = 0 and +11 = 1, then the same effect and timed event; the action
// bits are not read.
struct ActorLandInput {
    std::uint8_t life_state{};
    std::int32_t argument{1};
    float airtime_30a0{};
    std::uint64_t receiver{};
    std::optional<float> default_duration;    // 0018a410, read for argument 0 only
};
struct ActorLandResult {
    bool handled{};
    std::vector<ActorHangInstall> motions;
    int effects{},delayed_events{};    // 00188fb0(actor, 2, 1) calls and scheduled 004da870 events
    bool physical_reset{};             // argument 0: physical +9c0 = 0, +11 = 1 (with a physical component)
};
ActorLandResult land_actor(ActorAnimationReceiverState& actions,const ActorLandInput& input,const ActorHangMotionHost& host);

// The timed event 004da870 queued by the landing (001a12d0..001a13b0) and the
// ledge catch (001a236c..001a2448) after their immediate effect: one draw r
// of the game RNG (00127dd0), delay = base + cvt.w.s(span * (cvt.s.w(r) /
// 2^31)) with (70, 31) for the landing and (20, 21) for the catch; the record
// 0036b180(0, 0x14) is {+0 delay, +4 = 1, +5 = 0, +8 = 004da870, +c actor,
// +10 kind}, queued on the script callback list by 003ea1d0. Its handler
// 001b95c0: kind 0 calls 00188fb0(actor, 1, 1), kind 1 sends M_SHIMMY_ON
// (001ca210 bank 1, 0020f990 owned by the actor, +20 = 1, 003c9420 with
// transport 1); other kinds do nothing. The profiler calls (10dc40 / 10dca8
// with byte 00533880) and counter 00533888 are not hosted.
struct ActorTimedEvent {
    enum class Kind : std::uint8_t { landing = 0, shimmy = 1 };
    Kind kind{};
    std::int32_t delay{};
};
ActorTimedEvent actor_timed_event(ActorTimedEvent::Kind kind,std::uint32_t draw);
}
