#pragma once

namespace omega {

struct AnimationCurve {
    float start{}, start_tangent{}, end{}, end_tangent{};
    float duration{}, inverse_duration{};
};

struct AnimationChannelState {
    float previous{}, current{}, rate{}, target{}, damping{};
    AnimationCurve curve;
    float remaining{};
    bool wrapped{};
};

// Concrete default scalar channel used by the actor's model parameter array.
// This is channel state, not a clip sampler or a player displacement controller.
class AnimationChannel {
public:
    AnimationChannel() = default;
    AnimationChannel(const AnimationChannel&) = delete;
    AnimationChannel& operator=(const AnimationChannel&) = delete;
    AnimationChannel(AnimationChannel&&) = default;
    AnimationChannel& operator=(AnimationChannel&&) = default;

    const AnimationChannelState& state() const { return state_; }
    void set_value(float value);
    void approach(float target, float maximum_step, bool wrapped = false);
    // A negative duration preserves the existing curve and remaining time.
    void set_target(float target, float duration, float damping, bool wrapped = false);
    // Returns whether positive remaining time reached zero during this step.
    bool step(float elapsed);
    // The original copy constructor retains scalar/curve values but clears time.
    AnimationChannel original_copy() const;
    // Authored transaction snapshot; unlike the game's copy operation, this
    // preserves the timer so a failed model frame can roll back exactly.
    AnimationChannel clone_for_transaction() const;

private:
    AnimationChannelState state_;
};

}
