#pragma once
#include "animation_vector_curve.hpp"
#include <optional>

namespace omega {
// The navigation producer's ordered duration/target update. Both changes are
// staged together so a rejected target cannot leave a partially retimed curve.
void retarget_actor_position_curve(AnimationVectorCurveState& state,
    float duration,const RootVector& target);

// Concrete vector-curve stage in001b1710. Root weight/position are resolved
// from the actor's actual root. Disabled updates preserve all curve state.
// A returned position must be published by the owning actor through its
// parent-aware root setter; low-weight updates deliberately return no write.
// This does not replace collision, the separate rotation curve, or the rest
// of the original actor frame. The owner stages this state with those systems.
std::optional<RootVector> update_actor_position_curve(AnimationVectorCurveState& state,
    bool enabled,float root_weight,const RootVector& root_position,float elapsed);
}
