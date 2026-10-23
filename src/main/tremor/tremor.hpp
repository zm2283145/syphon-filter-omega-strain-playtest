#pragma once
// The tremor manager (decomp src/main/tremor; *004ffbdc = G+8c): the camera
// shake and pad rumble of explosions and the script native AddTremor.
//   002727b0 add(list, &pos, f12 duration, f13 magnitude, f14 inner, f15 outer):
//            a 0x40 record (Mem_Alloc 0036b180) {+0 duration, +4 elapsed 0,
//            +8 inner, +c outer, +10 magnitude, +20 pos, +30 0, +34 -1}
//            pushed at the end of the list (002728f0 -> 00138ea0).
//   0012f910 AddTremor's body (native 0012f8a0(GOBJ, duration, magnitude,
//            radius)): the object's vt+e4, then 002727b0(pos, duration,
//            magnitude, radius / 3.0, radius).
//   0021a35c the blast 0021a200: 002727b0(P, 1.0, 1.0, inner, outer) (the
//            BlastFlash of explosion.hpp).
//   002722b0 update(list, f12 dt), called at 0013381c after the pad vibration
//            update 0026bc20: E = camera (*004ffc10) vt+e4; per record (list
//            order) elapsed += dt; a record with !(elapsed <= duration) is
//            erased (00138da0) and freed; otherwise
//              d = vsqrt((dx*dx + dy*dy) + dz*dz) of E - pos,
//              m = magnitude, times (duration - elapsed) / (duration / 3) in
//                  the last third,
//              d < inner: m; d < outer: m * (outer - d) / (outer - inner);
//                  else 0,
//            and sum += m. Then sum = min.s(sum, 1.0) and, every frame,
//            002726f0(camera +b20, sum * TremorCamFactor, 0.35). When sum > 0:
//            p = sum * TremorRandPct, u = cvt.s.w(00127dd0()) / (1 + 2^31),
//            sum += p * u - p and the rumble 0026bde0(0, 0, cvt.w.s(
//            float(TremorMinRumble) + sum * float(TremorMaxRumble -
//            TremorMinRumble)), 1, 0.01).
//   002726f0 the shake amplitude's target and duration on the camera's scalar
//            curve +b20 (class 004da3e0): +10 = x; with d >= 0 the tangents
//            +1c / +24 scale by d * +2c, +28 = d, +2c = d == 0 ? 0 : 1 / d and
//            the remaining time +34 = d (the segment's +10).
// The record's +30 handle (a followed object: 0014d350 refreshes +20) is never
// set by these callers; it is not modelled.
#include "animation_root.hpp"
#include "camera_curves.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

struct TremorRecord {
    float duration_00{};
    float elapsed_04{};
    float inner_08{};
    float outer_0c{};
    float magnitude_10{};
    RootVector position_20{};
    bool operator==(const TremorRecord&) const = default;
};
// *004ffbdc: the records in list order.
struct TremorList {
    std::vector<TremorRecord> records;
};
// The engine variables 0048b2d0 / 0048b2d8 / 0048b2e0 / 0048b2e8 (boot values;
// the scripts can write them as game::Tremor*).
struct TremorVariables {
    float cam_factor_d0{0.3F};       // TremorCamFactor 0x3e99999a
    std::int32_t min_rumble_d8{40};  // TremorMinRumble
    std::int32_t max_rumble_e0{255}; // TremorMaxRumble
    float rand_pct_e8{0.5F};         // TremorRandPct
};

void add_tremor(TremorList&, const RootVector& position, float duration, float magnitude, float inner,
    float outer);                                                                          // 002727b0
void add_script_tremor(TremorList&, const RootVector& position, float duration, float magnitude,
    float radius);                                                                         // 0012f910

struct TremorFrame {
    float sum{};                                   // after min.s (before the random spread)
    float shake_target{};                          // 002726f0 f12
    std::optional<std::int32_t> rumble_large;      // 0026bde0 a2, when sum > 0
};
// 002722b0 without its two calls: the caller applies shake_target with
// set_tremor_shake(camera +b20, shake_target, 0.35) and starts the rumble.
// `random` (00127dd0) is drawn once, only when sum > 0.
TremorFrame update_tremors(TremorList&, float dt, const RootVector& camera_e4, const TremorVariables&,
    const std::function<std::uint32_t()>& random);
inline constexpr float tremor_shake_duration = 0.35F;                                      // 0x3eb33333
inline constexpr float tremor_rumble_duration = 0.01F;                                     // 0x3c23d70a
void set_tremor_shake(CameraScalarCurve&, float target, float duration);                   // 002726f0

}