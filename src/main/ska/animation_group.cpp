#include "animation_group.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
        throw std::runtime_error("Animation group requires finite normal or zero arithmetic");
    return value;
}
void nonnegative(float value) {
    checked(value);
    if (value < 0.0F) throw std::runtime_error("Animation group requires nonnegative values");
}
}

GroupDuration accumulate_group_duration(std::span<const DurationContribution> contributions) {
    if (contributions.size() > 10000)
        throw std::runtime_error("Animation group contribution count exceeds supported limit");
    GroupDuration result;
    for (const auto& contribution : contributions) {
        nonnegative(contribution.weight);
        nonnegative(contribution.duration);
        const float product = checked(contribution.duration * contribution.weight);
        result.total_weight = checked(result.total_weight + contribution.weight);
        result.weighted_duration = checked(result.weighted_duration + product);
    }
    return result;
}

void step_animation_group(AnimationGroupPhase& state, bool active,
    float total_weight, float weighted_duration, float effective_dt) {
    for (float value : {state.previous, state.current, state.advance,
             total_weight, weighted_duration, effective_dt}) nonnegative(value);
    if (state.previous > 1.0F || state.current > 1.0F)
        throw std::runtime_error("Animation group phase is outside supported bounds");
    auto next = state;
    if (!active) {
        next.previous = next.current = next.advance = 0.0F;
        next.finished = next.boundary = false;
    } else {
        const float preceding = next.current;
        next.previous = std::min(std::max(preceding, 0.0F),
            next.independent_bounds ? 1.0F : next.current);
        const float duration = total_weight == 0.0F ? 0.0F :
            checked(weighted_duration / total_weight);
        float advance = duration == 0.0F ? (next.looping ? 0.0F : 1.0F) :
            checked(effective_dt / duration);
        float proposed = checked(preceding + advance);
        if (proposed >= 1.0F) {
            if (next.looping) {
                unsigned wraps = 0;
                while (proposed >= 1.0F) {
                    if (wraps == 4096)
                        throw std::runtime_error("Animation group loop limit exceeded");
                    const float reduced = checked(proposed - 1.0F);
                    if (!(reduced < proposed))
                        throw std::runtime_error("Animation group loop subtraction made no progress");
                    proposed = reduced;
                    ++wraps;
                }
            } else {
                advance = checked(1.0F - preceding);
                next.finished = true;
                proposed = 1.0F;
            }
            next.boundary = true;
        }
        next.advance = advance;
        next.current = std::min(std::max(proposed,
            next.independent_bounds ? 0.0F : next.previous), 1.0F);
    }
    state = next;
}
}
