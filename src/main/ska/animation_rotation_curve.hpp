#pragma once
#include "animation_channel.hpp"

namespace omega {
// Concrete scalar angle class004da7d0, using its owned Hermite segment.
// This is distinct from [0,1] model channels and quaternion interpolation.
struct AnimationRotationCurveState {
    float previous{},current{},rate{},target{},damping{};
    AnimationCurve curve;
    float remaining{};
};
AnimationRotationCurveState make_animation_rotation_curve(float damping=0);
// The original heading producer assigns target even for negative duration;
// only a nonnegative duration restarts the timer and rescales tangents.
void target_animation_rotation_curve(AnimationRotationCurveState&,float target,float duration);
bool step_animation_rotation_curve(AnimationRotationCurveState&,float elapsed);
}
