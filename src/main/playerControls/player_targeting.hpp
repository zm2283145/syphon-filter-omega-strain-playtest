#pragma once
// The player's targeting function 00247d60 (called by the pad producer
// 0024b5d0 every frame before its weapon block; COMBAT.md "Player targeting"):
// the R1 lock-on pick and its recheck, the subtarget cycle (L1 with agent
// unlock 2), the automatic pick when nothing is locked, the aim view (selector
// +38 = 8), and the aim and look actions it sends to the actor with transport
// 1: 0x3e kind 5 (look at an object), 0x3f (clear the look target), 0x39 kind
// 0xb (aim at an object's eye), 0x39 kind 4 (aim at a point) and 0x39 kind 1
// (the 001977f0 request with the byte 0048aee8, 0 in the boot image: lower).
// The small callees 00249330 (lock timer), 00249580 (look target) and
// 00249670 (aim-view point) are native; the target manager (0022ad30,
// 0022b660), the visibility cache 0022cb10, the aim-view picks 00249b10 /
// 002497c0, the aim update (controller +60) and the HUD are host functions.
// Interpreter-verified (tests/player_targeting_goldens.hpp). Single player
// only: with the multiplayer byte 005721c8 set the original also writes the
// per-player record (00249420, 00249490, 002494e0), which throws here.
#include "animation_root.hpp"
#include "player_weapon_input.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

using TargetingMatrix=std::array<RootVector,4>;   // rows 0..3 (row 3 the translation)

// A weak handle (object pointer and the object's +c at the time it was stored).
struct TargetHandle {
    std::uint64_t object{};
    std::int32_t id{-1};
    bool operator==(const TargetHandle&) const=default;
};

// The input component fields 00247d60 reads and writes besides those of
// PadWeaponState (+3c, +40, +44, +c0/+c4, +d0, +e4, +11c).
struct PlayerTargetingState {
    std::int32_t selector_38{};          // +38: selector of the first group (8: aim view, 5 read for +11c)
    std::uint8_t r1_a8{};                // +a8: R1 on the previous frame
    std::uint8_t recheck_failed_bc{};    // +bc: the locked pick was lost while R1 stayed held
    std::uint8_t forward_119{};          // +119: no target this frame (the forward aim / lower branch ran)
    TargetHandle pick_c8{};              // +c8 / +cc: the R1 pick
    float lock_time_e8{};                // +e8: 0.25 (004a7550) while R1 holds a pick, then counts down
};

struct TargetingSend {
    std::uint8_t action{};               // +24: 0x39, 0x3e or 0x3f (all with +20 = 1, transport 1)
    std::uint8_t kind{};                 // +28
    std::int32_t object_id{-1};          // 0x3e kind 5: +2c; 0x39 kind 0xb: +30
    std::int32_t subtarget{-1};          // 0x39 kind 0xb: +34 (+38 = 0, +40 = zero vector, +50 = 0.0)
    std::uint8_t byte_2c{};              // 0x39 kind 1: +2c
    RootVector point{};                  // 0x39 kind 4: +30
    bool operator==(const TargetingSend&) const=default;
};
struct TargetingEvent {
    enum class Type : std::uint8_t {
        send,               // 003c9420(message, actor, 1)
        hud_clear_960,      // 00244960(hud, value): aim view with nothing held
        hud_changed_240,    // 00244240(hud): the HUD's tracked target (004ffc10) differs from 004ffc14's
        hud_target_380,     // 00244380(hud, &id, subtarget)
        hud_none_280,       // 00244280(hud)
        hud_aim_2e0,        // 002442e0(hud, progress x accuracy)
        clear_recent,       // 00177f60 over the recent-pick list +ec (from 00249330)
        thrown_point,       // 002497c0(input + d0, camera, actor)
    };
    Type type{};
    TargetingSend send{};
    std::int32_t id{-1};
    std::int32_t subtarget{-1};
    float value{};
    bool operator==(const TargetingEvent&) const=default;
};

// Checkpoint 132ln5h: the multiplayer record writes of 00247d60 (byte 005721c8 set; DME/dme_game.hpp): a lock
// 002494e0(input, actor, the locked object's +0x10, &shown, the subtarget) then 00249420(input, actor, &shown, 1);
// the lowered aim 00249420(..., 0); an aim point 00249490(input, actor, &point) (also 00249670's).
struct TargetingRecord {
    enum class Kind : std::uint8_t {lock,lower,point} kind{};
    std::uint64_t object{};          // lock: the locked object
    std::int32_t shown{-1};          // lock: its +c
    std::int32_t subtarget{-1};      // lock: input +e4
    RootVector point{};              // point
};

struct PlayerTargetingHost {
    // The object's +c (a weak handle resolves when the stored id equals it).
    std::function<std::int32_t(std::uint64_t object)> object_id;
    // Root bytes +2c, +2d, +2e of a picked object (the R1 recheck needs +2d and
    // +2c set and +2e clear).
    std::function<std::array<std::uint8_t,3>(std::uint64_t object)> root_bytes;
    std::function<RootVector(std::uint64_t object)> position_e4;    // virtual +e4 (also of the actor)
    std::function<std::int32_t(std::uint64_t object)> subtarget_count_d0;   // virtual +d0
    std::uint64_t actor{};
    std::function<TargetingMatrix()> actor_matrix_f4;                // actor virtual +f4
    // The held item: inventory +84 (6: nothing) and the slot's item ID.
    std::function<std::int32_t()> current_item;                      // -1 when nothing is held
    std::function<std::uint8_t()> hold_class;                        // 001437a0(+3524)
    std::function<std::uint8_t()> item_kind;                         // 00143510(+3524)
    std::function<bool()> pointer_33ec;                              // 00199740(actor): *(+33ec) set
    std::function<bool()> agent_unlock_2;                            // 00332f90(001314f0(004ffb50, -1), 2)
    // Target manager: 0022ad30(actor, current pick, input + ec) and
    // 0022b660(actor, previous lock); 0 when nothing qualifies.
    std::function<std::uint64_t(std::uint64_t current)> lock_pick;
    std::function<std::uint64_t(std::uint64_t previous)> auto_pick;
    // 0022cb10(004f77c0, actor, target, max_age, force): the blocking object (0: visible).
    std::function<std::uint64_t(std::uint64_t target,std::int32_t max_age,bool force)> blocker;
    std::function<std::uint64_t()> aim_view_pick;                    // 00249b10(input, camera, actor)
    std::function<TargetingMatrix()> camera_da0;                     // camera +da0 (rows 1 and 3 read)
    // 004ffc14's current object compared with 004ffc10 (true: they differ).
    std::function<bool()> hud_target_changed;
    // Controller virtual +60 (the aim accumulator 0022f5b0 for the player)
    // with (target or 0, item, subtarget); returns +20 and +24 after the call.
    std::function<std::array<float,2>(std::uint64_t target,std::int32_t item,std::int32_t subtarget)> aim_update;
    bool multiplayer{};                                              // 005721c8
    std::function<void(const TargetingRecord&)> record;               // the record writes (multiplayer set)
};

// 0022abc0(frame, point): the point in the frame's rigid inverse (0013ee20
// transpose, then 0013edb0 / 0013ed90 / 0013ed60 / 00131e70 for a nonzero
// translation), then pitch = atan2f(z, |xy|) and yaw = atan2f(-x, y) with the
// game's atan2f. Returns {pitch, 0, yaw} and |xy| (the VU square root).
struct TargetAngles {
    RootVector angles{};
    float planar{};
};
TargetAngles target_angles(const TargetingMatrix& frame,const RootVector& point);

// The threat inputs 0022ad30 reads from a candidate's controller.
struct TargetPickThreat {
    std::uint64_t aim_target{};          // 003ea550(controller): the resolved +28 / +2c
    float progress{};                    // controller +20
};
struct TargetPickHost {
    std::uint64_t actor{};
    std::function<std::int32_t(std::uint64_t object)> object_id;                   // +c
    std::function<std::array<std::uint8_t,3>(std::uint64_t object)> root_bytes;    // +2c, +2d, +2e
    std::function<RootVector(std::uint64_t object)> position_e4;                   // virtual +e4 (also the actor)
    std::function<RootVector(std::uint64_t object)> eye_c0;                        // virtual +c0 with subtarget -1
    std::function<std::uint8_t(std::uint64_t object)> kind_c4;                     // virtual +c4 (2: a character)
    std::function<std::int32_t(std::uint64_t object)> stealth_44;                  // +44
    // +4c is the actor class word (*0049d010) and +346c is set (0022ad30 only).
    std::function<bool(std::uint64_t object)> actor_flag_346c;
    // The object's controller +58 (nullopt: none) for 0022ad30's threat test.
    std::function<std::optional<TargetPickThreat>(std::uint64_t object)> controller;
    std::function<bool(std::uint64_t object)> enemy;                               // 003cc8e0(actor, object)
    std::function<std::uint64_t(std::uint64_t target,std::int32_t max_age,bool force)> blocker;   // 0022cb10
    std::function<TargetingMatrix()> actor_matrix_f4;                              // 0022b660
    std::function<TargetingMatrix()> actor_frame_d8;                               // 0022ad30
    std::function<TargetingMatrix()> camera_frame_d8;                              // 0022ad30: 004ffc14's object, virtual +d8
    // 001437e0(+3524): the held record's hold class +f0 and range +3c (nullopt: nothing held).
    std::function<std::optional<std::array<std::int32_t,2>>()> held_record;
    float actor_33a8{};                                                            // > 0: every target counts as fully hidden
    std::uint8_t actor_3470{};                                                     // 1: stealth ignored
    std::vector<TargetHandle> candidates;                                          // *004ffc2c + a8 (the HUD target list), in order
};
// 0022b660(actor, previous): the automatic pick: an enemy character (+c4 = 2)
// that is visible (0022cb10 age 0), within 0.15 x pi of the actor's +f4 frame
// (1 - |(pitch, yaw)| / pi > 0.85, +0.05 for the previous lock) and nearest by
// the stealth-scaled squared distance below the held weapon's range squared.
std::uint64_t auto_target_pick(const TargetPickHost& host,std::uint64_t previous);
// 0022ad30(actor, current, recent): the R1 pick by score (the yaw in the
// camera's +d8 frame whose row 3 the original replaces by the actor's +d8
// frame point (0, 0, 5, 1), range,
// distance tier, threat, recency in the recent list +ec), the best visible
// candidate (0022cb10 forced); the chosen object moves to the back of the
// recent list (0022d740, 00222980). Returns current when it scored and
// nothing else was chosen.
std::uint64_t lock_target_pick(const TargetPickHost& host,std::uint64_t current,std::vector<TargetHandle>& recent);

// 00249330(input, dt): the lock timer +e8 counts down to 0; at 0 the recent
// list is cleared and the pick +c8 / subtarget +e4 are dropped.
void targeting_lock_timer(PadWeaponState& pad,PlayerTargetingState& state,float dt,std::vector<TargetingEvent>& events);

// 00247d60(input, camera, actor, r1, l1, dt); l1 is the caller's "L1 held and
// not +b8". Returns the events in call order.
std::vector<TargetingEvent> run_player_targeting(PadWeaponState& pad,PlayerTargetingState& state,bool r1,bool l1,float dt,
    const PlayerTargetingHost& host);

}
