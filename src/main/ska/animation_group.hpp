#pragma once
#include <span>

namespace omega {
struct AnimationGroupPhase {
    float previous{}, current{}, advance{};
    bool independent_bounds{}, looping{}, finished{}, boundary{};
};
struct DurationContribution { float weight{}, duration{}; };
struct GroupDuration { float total_weight{}, weighted_duration{}; };

// Ordered scalar sums only: caller provides the recovered duration of each clip.
// Up to 10,000 contributions; finite normal/zero nonnegative inputs/arithmetic.
GroupDuration accumulate_group_duration(std::span<const DurationContribution> contributions);

// Scalar group update from 003ada60 with an explicitly supplied context time.
// Previous/current are in [0,1]; advance, sums and time are nonnegative.
// Finite normal/zero arithmetic only. Looping uses at most 4096 literal subtractions.
// Failure is atomic. Does not execute clips, modify contribution lists, or host time stacks.
void step_animation_group(AnimationGroupPhase& state, bool active,
    float total_weight, float weighted_duration, float effective_dt);
}
