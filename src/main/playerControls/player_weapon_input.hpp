#pragma once
// The weapon block of the pad producer 0024b5d0 (0x0024f1c8..0x0024f810;
// COMBAT.md §3): the fire decision by hold class (001437a0), the hold timer
// +100, the fire buffer +120 (0.75 s when the upper body is not ready to fire),
// the selector-1 release with 00143770, and the fire request (actor action
// 0x3b kind 0xb, payload 0024fc30: the lock target's +c or the "none" word
// -1, the subtarget +e4, the aim-view flag, the aim point +d0 and the throw
// power +e0) sent to the actor with transport 0 once both secondary states
// (+3314 +310 / +311, 0024fcd0) allow firing. Interpreter-verified for every
// hold class; hold class 1 (thrown: the charge +100, the power 0025d430 into
// +e0 while Square is held, the request on release with the lock target's +e4
// or the actor-local point (0, 0025d470(+100), 0, 0)) outside the aim view
// (c6b/gen_pad_throw.py); in the aim view the release clears the lock
// (0024fc90(+c0, 0)) and keeps +d0, while Square held in the aim view runs
// the arc preview 0025cdc0 (Checkpoint 132wf1: PadWeaponHost::arc_preview,
// thrown_projectile.hpp throw_arc_preview). Hold class 7 (Checkpoint 110): in the aim
// view the lock is cleared and +d0 = 0024fc60(controller +64 frame (aim 1),
// (0, inventory +f4, 0, 1)); outside it +d0 = 0024fc60(actor +f4 matrix,
// (0, 200, 4, 1)) unless the lock target is valid (gen_aim_view.py).
// The lock target +c0 and the aim point +d0 / power +e0 / subtarget +e4 are
// written by the targeting function 00247d60 earlier in the same producer.
#include "actor_fire.hpp"
#include "hud_crosshair.hpp"
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

// The input state fields (component +50) the block reads and writes.
struct PadWeaponState {
    std::uint32_t selector_mask_3c{};        // selector group +38: pending mask (bit 1 read)
    std::int32_t group_40{};                 // selector group +40: selection
    std::uint32_t group_mask_44{};           // and its mask
    std::uint8_t square_ab{};                // +ab: Square held on the previous frame
    float hold_100{};                        // +100: time Square has been held
    float timer_11c{};                       // +11c: 3.0 (004a7588) after a fire decision
    float buffer_120{};                      // +120: fire buffer, 0.75 (004a7598) while the upper body is not ready
    std::uint64_t lock_c0{};                 // +c0 / +c4: the lock target's weak handle
    std::int32_t lock_id_c4{-1};
    RootVector point_d0{};                   // +d0: aim point
    float power_e0{};                        // +e0: throw power
    std::int32_t subtarget_e4{-1};           // +e4
};
// The frame values of 0024b5d0 the block reads.
struct PadWeaponFrame {
    float square{};                          // action 7 (sp+f9c)
    bool aim_selector_1e0{};                 // sp+1e0: selector +38 is 8
    bool near_camera_1f0{};                  // sp+1f0: camera distance < 4.0 and 0017a610 clear
    float dt{};                              // f27
};
struct PadWeaponHost {
    std::function<std::int32_t()> hold_class;                    // 001437a0(+3524)
    std::optional<std::array<std::uint8_t,2>> secondary;         // +3314 present: its +310 / +311
    std::function<std::int32_t()> spare_6,clip_6;                // 00143870 / 00143940(+3524, 6)
    std::function<bool()> full_auto;                             // 00143820(+3524)
    std::function<void()> item_notice;                           // 00143770(+3524)
    std::function<bool(std::uint64_t object,std::int32_t id)> lock_valid;   // 00150c60 / 0014d350: object +c == +c4
    std::function<RootVector(std::uint64_t object)> position_e4;             // hold class 1: the lock target's virtual +e4
    std::function<void(const ActorFireRequest&)> send;           // action 0x3b kind 0xb, transport 0
    // Checkpoint 132wf1: 0025cdc0(hand, &+d0, f12 = +e0, f13 = cvt.s.w(held record +104)) with the hand the
    // actor's skeleton node *(s8)0049d02c (00132a30, 00132910): the host reads the hand and the record.
    std::function<void(const RootVector& target,float power)> arc_preview;
    // 00249530(input, actor): input +13c caches actor +3584 + 0x1c (when +3584
    // is set; written by 001b2d60 from 003fbb10's player record) and its
    // halfword is incremented. Called before every send of a class other
    // than 5 (and 1/7) while the clip (00143940(+3524, 6)) is not empty.
    std::function<void()> shot_counter;
    // Hold class 7: the controller (actor +58) virtual +64 frame with a2 = 0,
    // a3 = 1 (aim view), the inventory's +f4 (aim-view range) and the actor's
    // virtual +f4 matrix.
    std::function<ShotFrame()> aim_shot_frame;
    std::function<float()> aim_range_f4;
    std::function<ShotFrame()> actor_matrix_f4;
};
// 0024fc90: the weak handle (+0 object, +4 the object's +c or -1).
void pad_weak_handle_assign(std::uint64_t& object,std::int32_t& id,std::uint64_t target,std::int32_t target_id_c);
// 0024fc60: ((M0*v.x + M1*v.y) + M2*v.z) + M3*v.w per lane.
RootVector pad_transform_point(const ShotFrame& m,const RootVector& v);
// The selector group helpers 0024ffe0 (clear bit) and 002500c0 (set bit):
// the group's selection becomes the highest set bit of 11..0, or -1.
void pad_selector_clear(std::int32_t& selection,std::uint32_t& mask,unsigned bit);
void pad_selector_set(std::int32_t& selection,std::uint32_t& mask,unsigned bit);
// 0x0024f1c8..0x0024f810 for the actor (s1 non-null). Returns the aim-view
// flag the block computed (s7).
bool run_pad_weapon_block(PadWeaponState& state,const PadWeaponFrame& frame,const PadWeaponHost& host);
// 00241ec0's return (stored to actor +346e at 0x0024f964) for a1 = sp+1e0,
// a2 = sp+200, a3 = the held record (001437e0, null with nothing held) and
// t0 = +346f ? 0013fb50(inventory) (the slot-6 goggles record) : 0, whose
// word +130 is `goggles_130` (Checkpoint 132cp4; nullopt for t0 = 0), with
// 004ffc14's object equal to *004ffc10 (the camera follows the local player).
// s2 = hud_crosshair_index; the return (00241fc8..00241fe4) is the held
// record's signed byte +100 when s2 >= 0 and a record is held, else 0. The
// HUD reticle switch (+8c, 00244a00 / 00244a10) is HudObject::crosshair
// (CombatPlayConfig::crosshair_hud in play).
std::int8_t pad_crosshair_346e(bool aim_1e0,bool far_200,const WeaponRecord* record,
    const std::optional<std::int32_t>& goggles_130=std::nullopt);
// The held record's +f0 / +130 / +100 as 00241ec0 reads them (nullopt: no record).
std::optional<HudCrosshairRecord> crosshair_held(const WeaponRecord* record);

}
