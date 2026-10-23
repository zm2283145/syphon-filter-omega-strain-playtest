#pragma once
#include "animation_root.hpp"

namespace omega {

struct AnimationVectorSegment {
    RootVector start{0,0,0,1}, start_tangent{}, end{0,0,0,1}, end_tangent{};
    float duration{}, inverse_duration{};
    bool operator==(const AnimationVectorSegment&) const = default;
};

// Concrete default vector curve; its selected segment is always the embedded
// segment. External segment pointers and derived virtual hooks are excluded.
struct AnimationVectorCurveState {
    RootVector previous{0,0,0,1}, current{0,0,0,1}, rate{}, target{0,0,0,1};
    float damping{};
    AnimationVectorSegment segment;
    float remaining{};
    bool operator==(const AnimationVectorCurveState&) const = default;
};

AnimationVectorSegment make_animation_vector_segment(float duration,
    const RootVector& start, const RootVector& start_rate,
    const RootVector& end, const RootVector& end_rate);
RootVector sample_animation_vector_segment(const AnimationVectorSegment&, float time);
// A finite negative duration is a no-op, including for unused retained fields.
void retime_animation_vector_curve(AnimationVectorCurveState&, float duration);
void set_animation_vector_target(AnimationVectorCurveState&, const RootVector&);
// Both preserve previous, target and remaining time. Reset retains the rate;
// rebase clears it, matching the concrete class's zero-vector callback.
void reset_animation_vector_current(AnimationVectorCurveState&, const RootVector&);
void rebase_animation_vector_current(AnimationVectorCurveState&, const RootVector&);
// Positive elapsed time; returns whether a positive timer reached zero.
// Mutations commit only after all finite-arithmetic checks succeed.
bool step_animation_vector_curve(AnimationVectorCurveState&, float elapsed);

}
