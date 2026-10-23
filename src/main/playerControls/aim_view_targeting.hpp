#pragma once
// The manual aim view's picks that the targeting function 00247d60 calls
// with selector +38 = 8 (COMBAT.md "Checkpoint 117 aim view entry and lock"):
// the lock 00249b10 (a 0022a8c0 probe along the aim-view shot frame with the
// LOS mask 0x10; its result is the aim-view range, inventory +f4, and the
// object the scan stops at), the class-1 (thrown item) aim point 002497c0
// (a LOS ray of the held record's +3c range from the camera point 1.5 ahead)
// and the aim-view shot frame of the player controller's virtual +64
// (00176400 with a3 != 0: the camera transform with row 3 one unit ahead).
// The HUD calls are returned in order; they are not drawn.
#include "flashlight.hpp"
#include "weapon_shot_frame.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// 00176400(controller, out, a2, a3 = 1): the camera's +50 transform (its +10
// matrix after the component's +58 refresh when +50 is set; the copy +da0
// after the camera update) with row 3 = M (0, 1, 0, 1) (vmulax / vmadday /
// vmaddaz / vmaddw).
ShotFrame aim_view_shot_frame(const ShotFrame& camera);

// The HUD calls of the two picks (on *004ffc2c).
struct AimViewHudEvent {
    enum class Kind : std::uint8_t {
        reticle_friend_9d0,   // 002449d0(hud): HUD +78 element 003e8de0(e, 1) (a probed object that is not an enemy)
        reticle_clear_9a0,    // 002449a0(hud): 003e8de0(e, 0)
        meter_960,            // 00244960(hud, value): HUD +68 (00260d90 / 003e7ee0)
        popup_2fa0,           // 00242fa0(hud, object +c, 003fdb50("Headshot"), 0, 0, 0, 0.1, 1.0)
        thrown_580,           // 00244580(hud, point, &direction)
    } kind{};
    float value{};
    std::int32_t object_id{-1};       // popup: the object's +c
    RootVector point{},direction{};   // thrown_580
    bool operator==(const AimViewHudEvent&) const=default;
};

struct AimViewLockHost {
    std::uint64_t actor{};
    // Controller +58 virtual +64(controller, frame, 0, 1): aim_view_shot_frame
    // of the followed camera for the player controller.
    std::function<ShotFrame()> shot_frame;
    // 0013faf0(actor +3524): the held record's +3c (cvt.s.w), 0.0 with
    // nothing held (+84 == 6) or an empty slot (item -1).
    std::function<float()> range;
    // 0022a8c0(actor, request, 0x10): flashlight_beam_probe_result over the
    // world LOS (mask 0x10) and the candidate scan 0022ba30.
    std::function<FlashlightProbeResult(const FlashlightProbeRequest&)> probe;
    std::function<bool(std::uint64_t object)> enemy;              // 003cc8e0(actor, object)
    std::function<std::uint8_t(std::uint64_t object)> life_108;    // object virtual +108
    std::function<std::int32_t(std::uint64_t object)> object_id;   // object +c
};
struct AimViewLockResult {
    std::uint64_t object{};           // the return value (request +38, also for a friend)
    float range_f4{};                 // stored at inventory +f4 (00249ccc)
    std::vector<AimViewHudEvent> hud;
};
// 00249b10(input, camera, actor). The request (stack +a0): +4 the shot
// frame, +8 0, +c 0, +10 -1, +14 the range, +1c 0, +20 0 (0022a8c0 sets 1),
// +24 0, +28 0.2 (0x3e4ccccd), +2c 1, +2d 1. A probed object that is not an
// enemy clears the category (+30 = 0) and is 002449d0, else 002449a0; a head
// hit (category 2) on an object whose +108 is not 2 or 3 adds the "Headshot"
// popup; 00244960 gets 1.0 for category 0, else 0.0. The probe's return is
// the inventory's aim-view range +f4 (its only writer).
AimViewLockResult aim_view_lock(const AimViewLockHost& host);

// 0022a8c0's probe request for the lock (exposed for tests).
FlashlightProbeRequest aim_view_lock_request(std::uint64_t actor,const ShotFrame& frame,float range);

struct AimViewLosHit {
    RootVector point{};               // result +10
    RootVector surface_50{};          // result +50 (the hit triangle record's fourth vector: its normal)
};
struct AimViewThrownHost {
    std::uint64_t actor{};
    std::function<ShotFrame()> camera;                             // camera +50 transform (+10 matrix)
    // 001437e0(actor +3524) +3c: the held record's word (nothing held reads
    // through a null record; the caller holds an item).
    std::function<std::int32_t()> record_3c;
    // 003caa50(004ffb50, 004ea0b0) virtual +c with the request (vtable
    // 004d9180, class 005435a0): origin +30, displacement +40, +50 the actor,
    // +54 0, +58 0x10, +5c 0, +3c 1.0, +4c 0, +24 / +28 0.
    std::function<std::optional<AimViewLosHit>(const FlashlightLosRequest&)> los;
};
struct AimViewThrownResult {
    RootVector point{};               // written to input +d0
    RootVector direction{};
    bool hit{};                       // the return value (result byte +0)
    std::vector<AimViewHudEvent> hud;
};
// 002497c0(input + d0, camera, actor): 00244960(hud, 0.0); origin = M (0,
// 1.5, 0, 1) (with M's row 3 w), which replaces row 3 of the stack copy
// (002498b8, sp+230); the request's displacement = row 1 x range (xyzw) with
// +3c = 1.0 and +4c = 0 stored after; far = M' (0, range, 0, 1) over the copy.
// A hit gives the hit point and result +50, else far and 0 - row 1 (each
// lane, vmulax ACC 0 / vmsubw); then 00244580(hud, point, &direction).
AimViewThrownResult aim_view_thrown_point(const AimViewThrownHost& host);

}
