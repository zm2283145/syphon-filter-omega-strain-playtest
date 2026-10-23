#include "animation_channel.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
void finite(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Animation channel requires finite values");
}
float wrapped_difference(float difference) {
    finite(difference);
    difference = std::fmod(difference, 1.0F);
    // The original timed-channel hooks compare BOTH branches with +0.5.
    // This is intentionally different from the indexed approach operation.
    if (difference < 0.5F) difference += 1.0F;
    else if (difference > 0.5F) difference -= 1.0F;
    return difference;
}
void validate(const AnimationChannelState& s) {
    for (const auto value : {s.previous, s.current, s.rate, s.target, s.damping,
             s.curve.start, s.curve.start_tangent, s.curve.end, s.curve.end_tangent,
             s.curve.duration, s.curve.inverse_duration, s.remaining}) finite(value);
}
float evaluate(const AnimationCurve& curve, float remaining) {
    const float u = curve.duration == 0.0F ? 1.0F :
        (curve.duration - remaining) * curve.inverse_duration;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float a = 3.0F * u2;
    const float b = 2.0F * u3;
    const float h00 = 1.0F + (b - a);
    const float h01 = (-2.0F * u3) + a;
    const float first = h00 * curve.start;
    const float second = first + h01 * curve.end;
    const float h10 = u + (u3 - 2.0F * u2);
    const float third = h10 * curve.start_tangent;
    const float accumulated = third + second;
    const float result = accumulated + (u3 - u2) * curve.end_tangent;
    finite(result);
    return result;
}
}

void AnimationChannel::set_value(float value) {
    finite(value);
    state_.previous = value;
    state_.current = value;
    state_.target = value;
    state_.rate = 0.0F;
}

void AnimationChannel::approach(float target, float maximum_step, bool wrapped) {
    finite(target);
    finite(maximum_step);
    if (maximum_step < 0.0F) throw std::runtime_error("Animation channel maximum step is negative");
    float difference = target - state_.current;
    finite(difference);
    if (wrapped) {
        if (difference < -0.5F) difference += 1.0F;
        else if (difference > 0.5F) difference -= 1.0F;
    }
    const float increment = std::min(std::max(difference, -maximum_step), maximum_step);
    float value = state_.current + increment;
    if (wrapped) {
        if (value < 0.0F) value += 1.0F;
        else if (value > 1.0F) value -= 1.0F;
    }
    set_value(value);
}

void AnimationChannel::set_target(float target, float duration, float damping, bool wrapped) {
    finite(target);
    finite(duration);
    finite(damping);
    auto next = state_;
    next.target = target;
    next.damping = damping;
    next.wrapped = wrapped;
    if (duration >= 0.0F) {
        const float ratio = duration * next.curve.inverse_duration;
        next.curve.start_tangent *= ratio;
        next.curve.end_tangent *= ratio;
        next.curve.duration = duration;
        next.curve.inverse_duration = duration == 0.0F ? 0.0F : 1.0F / duration;
        next.remaining = duration;
    }
    validate(next);
    state_ = next;
}

bool AnimationChannel::step(float elapsed) {
    finite(elapsed);
    if (elapsed <= 0.0F) throw std::runtime_error("Animation channel elapsed time must be positive");
    const float inverse_elapsed = 1.0F / elapsed;
    finite(inverse_elapsed);
    auto next = state_;
    auto& curve = next.curve;
    next.previous = next.current;
    if (next.remaining == curve.duration && curve.duration > 0.0F) {
        if (next.wrapped) next.target = next.current + wrapped_difference(next.target - next.current);
        curve.start = next.current;
        curve.start_tangent = next.rate * curve.duration;
        curve.end = next.target;
        curve.end_tangent = 0.0F * curve.duration;
        curve.inverse_duration = 1.0F / curve.duration;
    } else if (next.remaining != 0.0F) {
        if (next.wrapped) next.target = curve.end + wrapped_difference(next.target - curve.end);
        curve.end = next.target;
        curve.end_tangent = 0.0F * curve.duration;
    }
    const float preceding_remaining = next.remaining;
    next.remaining = std::max(0.0F, next.remaining - elapsed);
    const bool completed = preceding_remaining > 0.0F && next.remaining == 0.0F;
    if (next.remaining == 0.0F) {
        next.current = next.target;
    } else {
        const float value = evaluate(curve, next.remaining);
        if (next.wrapped) {
            next.current = std::fmod(value, 1.0F);
            if (next.current < 0.0F) next.current += 1.0F;
        } else next.current = std::min(std::max(value, 0.0F), 1.0F);
    }
    const float difference = next.current - next.previous;
    next.rate = (next.wrapped ? wrapped_difference(difference) : difference) * inverse_elapsed;
    const float damping_product = next.damping * elapsed;
    finite(damping_product);
    const float damping_factor = std::max(0.0F, 1.0F - damping_product);
    next.rate *= damping_factor;
    validate(next);
    state_ = next;
    return completed;
}

AnimationChannel AnimationChannel::original_copy() const {
    AnimationChannel result;
    result.state_ = state_;
    result.state_.remaining = 0.0F;
    return result;
}
AnimationChannel AnimationChannel::clone_for_transaction() const {
    AnimationChannel result;
    result.state_ = state_;
    return result;
}
}
