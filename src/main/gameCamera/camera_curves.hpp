#pragma once
// Retained curves owned by the camera object (base vtable 004da580). The camera
// steps every curve inline in its update 0017a640 with the same five-stage
// sequence: previous = current; preupdate (vtable +18); timer; sample or settle
// on the target; post (+1c); rate (+20). Vector curves (class 004da410) use
// animation_vector_curve; this module adds the quaternion class 004da440 and
// the scalar class 004da3e0.
#include "animation_vector_curve.hpp"

namespace omega {

// Class 004da440 (constructor 0017f6a0). Layout: +10 previous, +20 current,
// +30 rate, +40 target, +50 damping, +60 embedded Hermite segment
// (start, start tangent * T, end, end tangent * T, T, 1/T), +b0 segment pointer
// (always the embedded segment here), +b4 remaining time, +c0 hemisphere flag.
struct CameraQuaternionCurve {
    RootVector previous{0,0,0,1}, current{0,0,0,1}, rate{}, target{0,0,0,1};
    float damping{};
    AnimationVectorSegment segment;
    float remaining{};
    bool hemisphere{};
    bool operator==(const CameraQuaternionCurve&) const = default;
};
// 0017f6a0(start, rate): previous/current/target = start, rate = second
// argument, segment (start, rate, start, rate, 0, 0), remaining 0.
CameraQuaternionCurve make_camera_quaternion_curve(const RootVector& start,
    const RootVector& rate, bool hemisphere, float damping);
// 0017d840: a negative duration is a no-op; otherwise both tangents scale by
// duration * old inverse, duration and inverse are replaced and remaining
// restarts at the new duration.
void retime_camera_quaternion_curve(CameraQuaternionCurve&, float duration);
// The 0017a640 sequence with preupdate 00180310, timer 0017d540, sampler
// 0017d340, post 00180550 (4D normalise) and rate 00180580. inverse_elapsed is
// the caller's 1/elapsed (computed once per camera update).
void step_camera_quaternion_curve(CameraQuaternionCurve&, float elapsed, float inverse_elapsed);
// End of 0017a640 while the reset byte +201 is set: current = target,
// previous = current, rate = vtable +14 (001802f0, the zero vector).
void settle_camera_quaternion_curve(CameraQuaternionCurve&);

// Class 004da3e0 (base constructor 0017f480). Layout: +4 previous, +8 current,
// +c rate, +10 target, +14 damping, +18 segment (p0, m0*T, p1, m1*T, T, 1/T),
// +30 segment pointer, +34 remaining.
struct CameraScalarCurve {
    float previous{}, current{}, rate{}, target{}, damping{};
    float p0{}, m0{}, p1{}, m1{}, duration{}, inverse_duration{};
    float remaining{};
    bool operator==(const CameraScalarCurve&) const = default;
};
// Inline retime used by 0017a640 for the field-of-view curve (+a90): tangents
// scale by duration * old inverse; remaining restarts.
void retime_camera_scalar_curve(CameraScalarCurve&, float duration);
// The 0017a640 sequence with preupdate 0017c900, timer 0017c8a0, sampler
// 0017c7c0, empty post 0017c7b0 and rate 0017c760.
void step_camera_scalar_curve(CameraScalarCurve&, float elapsed, float inverse_elapsed);

// Quaternion helpers matching the VU0 macro sequences used by the camera.
// Hamilton product a*b in (x, y, z, w) order.
RootVector camera_quaternion_multiply(const RootVector& a, const RootVector& b);
float camera_dot4(const RootVector& a, const RootVector& b);

}
