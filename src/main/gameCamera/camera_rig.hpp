#pragma once
// Base camera object (vtable 004da580, constructor 0017f9f0) and its update
// 0017a640. The camera is a chain of retained curves: a follow logic (for the
// player, 0027d620) writes targets and durations, and this update steps the
// curves and composes the view transform:
//   orientation = curve +210 toward (+ad0 * pitch curve +780),
//   pivot       = curve +2e0 toward +ae0,
//   boom        = curves +850 -> +910 -> +9d0 toward +af0 (pivot space),
//   position    = pivot + R(orientation) * boom + shake (+bc0),
//   half FOV    = atan(curve +a90 toward tan(+c64 / 2)).
// Camera collision (+c60) is delegated to a host: the original fills the body
// at +70 (vtable 004dad90), runs 001811c0, and limits each boom point with the
// sweep 0020d680. The shake amplitude +b20 (the tremor manager's 002726f0,
// tremor.hpp) draws its random offsets from the game random 00127dd0; the
// delayed shake curve +b58 is hosted only at rest.
// Below 1.1170107 rad the zoom body +170 (camera_zoom.hpp) supplies the field
// of view; its gather and sweep are host services.
#include "camera_curves.hpp"
#include "camera_zoom.hpp"
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

using CameraMatrix = std::array<RootVector,4>; // rows: X, Y (forward), Z (up), position

struct CameraRigState {
    CameraQuaternionCurve orientation_210;   // hemisphere, damping 0
    AnimationVectorCurveState pivot_2e0;     // damping 0
    AnimationVectorCurveState look_3a0;      // damping 0
    CameraQuaternionCurve heading_460;       // damping 15
    CameraQuaternionCurve heading_530;       // damping 15, target = current of +460
    AnimationVectorCurveState offset_600;    // damping 15, retimed to 0.15 each update
    AnimationVectorCurveState offset_6c0;    // damping 15, target = current of +600, 0.15
    CameraQuaternionCurve pitch_780;         // hemisphere, damping 15
    AnimationVectorCurveState boom_850;      // damping 15
    AnimationVectorCurveState boom_910;      // damping 15, target = current of +850
    AnimationVectorCurveState boom_9d0;      // damping 15
    CameraScalarCurve fov_a90;               // damping 15, retimed to 0.15 each update
    CameraScalarCurve shake_b20;             // amplitude (002726f0 target, 0.35 s)
    CameraScalarCurve shake_b58;             // delayed amplitude (0017dea0); only rest is hosted
    AnimationVectorCurveState shake_ba0;     // retimed to 0.01 each update
    RootVector target_orientation_ad0{0,0,0,1};
    RootVector target_pivot_ae0{0,0,0,1};
    RootVector boom_af0{0,0,0,1};
    RootVector position_b10{};
    float half_fov_60{};
    float fov_c64{}, fov_c68{};
    bool active_200{true};                   // +200: update runs only when set
    bool reset_201{true};                    // +201: snap every curve this update
    bool body_c60{};                         // +c60: collision body enabled
    bool look_c61{};                         // +c61: +3e0 supplied by the follow logic
    CameraMatrix matrix{};                   // last transform sent to +50 vtable +c
    CameraZoomBody zoom_170{};               // +170: zoom body (written below 1.1170107)
};

// 0017f9f0(fov): curves at identity / (0,0,0,1) with zero rates, damping 15
// on +460, +530, +600, +6c0, +780, +850, +910, +9d0 and +a90, hemisphere on
// +210 and +780, +c64 = +c68 = fov, +60 = fov / 2, reset byte set.
CameraRigState make_camera_rig(float fov);
// 00176ff0: translate the pivot and look vector curves without changing
// their rates, tangents, durations or remaining time.
void translate_camera_follow_curves(CameraRigState&,const RootVector& delta);

// Camera body at +70 as 0017a640 fills it before the collision step.
struct CameraBody {
    RootVector forward_10{};      // orientation row 1
    RootVector orientation_20{};  // +230
    RootVector look_40{};         // +3c0
    RootVector pivot_50{};        // +300
    std::array<RootVector,2> points{}; // +30 list: boom-target point, boom point
};
struct CameraCollisionResult {
    RootVector point{};
    bool hit{};                   // body +60
};
struct CameraCollisionHost {
    // +130 vtable +10 (+3c0), the body fields and 001811c0.
    std::function<void(const CameraBody&)> prepare;
    // 0020d680(body, point).
    std::function<CameraCollisionResult(const CameraBody&,const RootVector&)> limit;
};

// 0017a640. elapsed must be positive. The collision host is required while
// body_c60 is set; the zoom host while the field of view is below 1.1170107
// (0017c540: 0020d3e0 on the transform just sent, then 001811c0 on the zoom
// query, and the field of view +178 replaces +c64 below). A positive shake
// amplitude (+b20 + +b58) draws two game randoms (00127dd0, `random`, required
// then); a non-zero +b58 is not hosted.
void update_camera_rig(CameraRigState&, float elapsed, const CameraCollisionHost* host = nullptr,
    const CameraZoomHost* zoom = nullptr, const std::function<std::uint32_t()>* random = nullptr);
// 0017dd20: the rotation rows of the Euler angles (x, y, z) (+3c = 1 and the
// rest zero are implied).
std::array<RootVector,3> camera_shake_matrix(const RootVector& euler);
// 0017a6ec..0017a764: (0, amplitude, 0, 0) by the transpose of the Euler matrix
// of (cvt.s.w(random()), 0, cvt.s.w(random())), w = 1: the +ba0 target.
RootVector camera_shake_target(float amplitude, const std::function<std::uint32_t()>& random);

// Math shared with the follow logic (VU0 macro operation order).
RootVector camera_rotate(const RootVector& q, const RootVector& v);          // q (v,0) q*, W kept
RootVector camera_place(const RootParentTransform& frame, const RootVector& v); // rotate + W test
std::array<RootVector,3> camera_quaternion_rows(const RootVector& q);
}
