#include "animation_transition.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
        throw std::runtime_error("Transition blend requires finite normal or zero arithmetic");
    return value;
}
}

TransitionBlendResult step_transition_blend(TransitionBlendState& state,
    float parent_weight, float dt, bool paused) {
    for (float value : {state.progress, state.rate, state.width, state.bias,
             state.elapsed, state.limit, parent_weight, dt}) checked(value);
    if (parent_weight < 0.0F || dt < 0.0F)
        throw std::runtime_error("Transition blend weight and time must be nonnegative");

    auto next = state;
    TransitionBlendResult result;
    result.paused = paused;
    if (!paused) {
        const float increment = checked(dt * next.rate);
        const float advanced = checked(next.progress + increment);
        next.progress = std::min(1.0F, advanced);
        result.crossed = state.progress < 1.0F && next.progress >= 1.0F;
    }

    const float squared = checked(next.progress * next.progress);
    const float accumulated = checked(3.0F * squared);
    const float cubed = checked(next.progress * squared);
    const float doubled = checked(2.0F * cubed);
    const float polynomial = checked(accumulated - doubled);
    const float eased = std::min(std::max(polynomial, 0.0F), 1.0F);
    result.sampled_weight = checked(parent_weight * eased);
    result.submit_node = result.sampled_weight != 0.0F;
    result.remaining_weight = std::max(0.0F, checked(parent_weight - result.sampled_weight));

    if (!paused) {
        const float remaining = std::max(0.0F, checked(1.0F - next.progress));
        const float weighted_remaining = checked(remaining * next.width);
        const float difference = checked(next.width - next.bias);
        const float lower = checked(0.5F * difference);
        result.boundary = weighted_remaining < lower;
        const float sum = checked(next.width + next.bias);
        const float upper = checked(0.5F * sum);
        if (weighted_remaining > upper) result.paused = true;
    }

    next.elapsed = checked(next.elapsed + dt);
    if (next.limit != -1.0F) next.elapsed = std::min(next.limit, next.elapsed);
    state = next;
    return result;
}

}
