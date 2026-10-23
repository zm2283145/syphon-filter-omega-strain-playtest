#pragma once
// The HUD radar at hud +d0 (hud.cc), built by 002619b0 inside the HUD
// constructor 00245f00 over the IARCADEN screen's "radargroup" node, and its
// per-frame update 00261690 (vtable 004dc950 +8, called by the HUD update
// 00244fa0 between the player lookup and the targets 00273b40):
//   the north indicator turns to -heading (the camera's +64),
//   the waypoint arrow points from the player to the targets' selected
//   waypoint (00272cd0) and fades with the distance past RadarMaxDist,
//   the player marker turns to the player's forward in the camera frame.
// The danger / environmental meters 00261540 (Checkpoint 130c) are below;
// the blips (00261130 / 00261360 / 00274260, Checkpoint 130d) are in
// hud_markers.hpp.
#include "ie_screen.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace omega {

// The radar object (0x1c bytes; vtables 004dc930 then 004dc950 at +4). Its +0
// is the HUD's targets object (hud + 90), stored by the HUD constructor at
// 00246128 (then registered with 00246540). Elements and nodes are screen
// indices, -1 null.
struct HudRadar {
    int danger_8 = -1;          // "dangermeter"
    int environmental_c = -1;   // "environmentalmeter"
    int marker_10 = -1;         // "playermarker"
    int north_14 = -1;          // "northindicator"
    int waypoint_18 = -1;       // "selected_waypoint"
    // The hud.cc globals 004ffd58 / 004ffd50 (both "radartriangle") and
    // 004ffd60 ("radarsquare"), nodes left hidden.
    int triangle_4ffd58 = -1, triangle_4ffd50 = -1, square_4ffd60 = -1;
};

// The two constants of the waypoint fade: the ints *0048b0d8 (9) and
// *004f8298 (0); neither has a writer in the ELF.
inline constexpr std::int32_t hud_radar_waypoint_last = 9;   // 0048b0d8
inline constexpr std::int32_t hud_radar_waypoint_first = 0;  // 004f8298
// The float *0048b0c8 at boot, the engine variable game::RadarMaxDist.
inline constexpr float hud_radar_max_dist_boot = 150.0F;

// 002619b0(radar, group): the elements dangermeter / environmentalmeter /
// playermarker / northindicator (00260d50: each a child of group made an
// element, or null), group +60 = 1 (003eebe0 not recursive), its child
// "radar" visible with its subtree (a missing one is a null node: throws
// naming 00261a58), selected_waypoint (00260d50) evaluated at
// (float)*0048b0d8 (003f45a0; null throws naming 00261a80), and the globals
// 004ffd58 = 004ffd50 = child "radartriangle", 004ffd60 = child
// "radarsquare" (003eeb30, or null). A null group throws naming 002619e8.
HudRadar hud_radar_construct(IeScreen& screen, int group);

// What 00261690 reads outside the radar.
struct HudRadarFrame {
    // The camera 00159560(0x4ffc14) object's +64 (the heading
    // 0017e1b0 stores) and the rows 0..2 of its +50 transform's matrix
    // (00133330 -> 00133190 inverts it: the 3 x 3 transpose).
    float camera_heading_64 = 0.0F;
    std::array<std::array<float, 4>, 3> camera_rows{};
    // The player's +50 transform matrix row 1 (+20 of the component).
    std::array<float, 4> player_row1{};
    // vt +e4 (the position) of the player and of 00272cd0(*radar), the
    // selected waypoint (null: nullopt).
    std::array<float, 4> player_position{};
    std::optional<std::array<float, 4>> waypoint_position;
    // *0048b0c8 (game::RadarMaxDist).
    float max_dist = hud_radar_max_dist_boot;
};

// 00261690(radar, player):
//   north: 003e8070(+14, (0, -heading, 0));
//   with +18: no waypoint -> 003e8de0(+18, 0); else d = player - waypoint
//     (vsub), dist = vsqrt(ftz(ftz(dx^2 + dy^2) + dz^2)); dist <= max ->
//     003e8de0(+18, 0); else y = -heading + atan2f(-dx, -dy) on (0, y, 0)
//     (003e8070), f = min(dist - max, max), 003e8de0(+18, 1) and 003f44e0(+18,
//     last - f * (last - first) / max);
//   marker: v = R . row1 with R the camera rows (vmulax / vmadday / vmaddz,
//     v_k = ftz(ftz(ftz(R_k0 x) + R_k1 y) + R_k2 z)), 003e8070(+10,
//     (0, -atan2f(-v_x, v_y), 0)).
// A null north / marker element throws naming 002616f8 / 0026191c; a max of
// zero (div.s by zero at 0026182c) throws.
void hud_radar_update(IeScreen& screen, const HudRadar& radar, const HudRadarFrame& frame);

// The engine variables 00261540 reads (sinit/engine_variables.cpp), at
// their boot values: game::DangerMag (the float *0048b310, returned by
// 00272cc0), dangerFirstFrame (*004f8260), dangerLastFrame (*0048b0b0) and
// dangerDimFrame (*0048b0b8), the last three ints.
struct HudMeterVars {
    float danger_mag = 1.25F;
    std::int32_t first = 0;
    std::int32_t last = 22;
    std::int32_t dim = 28;
};

// 00261540(radar, environmental, value): the meter is +c (environmental) or
// +8 (danger); f = min.s(value x DangerMag, 1.0); the danger call first sets
// the targets' +30 (*radar + 30) to 1.0; a null meter returns there. f <=
// 0.98 (0x3f7ae148, c.ole.s): 003f44e0(meter, cvt.s.w(cvt.w.s(first +
// (last - first) x f))). Else 003f4510(meter, last, dim, 1) and, for the
// danger call, the targets' +30 = (dim - t) / (dim - last) with t the
// meter's animation time (+4) after the range. Callers: the targets update
// 00273b40 at 00273e30 (danger, value = the largest aim progress of an enemy
// aiming at the local player, update_hud_targets) and the player controller
// update 001770f0 at 00177344 (environmental, value = its +1ac, then +1a8 =
// +1ac = 0). Returns the targets' +30 the danger call leaves (nullopt for the
// environmental call). dim == last (div.s by zero at 00261614) throws.
std::optional<float> hud_radar_meter(IeScreen& screen, const HudRadar& radar, bool environmental, float value,
                                     const HudMeterVars& vars);

// The player controller's virtual +80 (00176590, vtable 004d9cf0): +1ac
// keeps the largest value (c.ole.s: stored unless value <= +1ac). The fire
// zone 00261df0 and the burning actor's own 001b2134 call it.
inline float hud_controller_heat(float heat_1ac, float value) { return value <= heat_1ac ? heat_1ac : value; }

} // namespace omega
