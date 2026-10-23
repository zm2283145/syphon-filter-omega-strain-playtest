#pragma once

namespace omega {

// Scalar fields of the transition record: +18, +10, +08, +0c, +e4, +e0.
// Width and bias are neutral names; no gameplay interpretation is implied.
struct TransitionBlendState {
    float progress{}, rate{}, width{}, bias{}, elapsed{}, limit{-1.0F};
};

struct TransitionBlendResult {
    float sampled_weight{}, remaining_weight{};
    bool crossed{}, boundary{}, paused{}, submit_node{};
};

// Bounded arithmetic from 003ae350, without node evaluation or callback mutation.
// Uses individually rounded native float operations, not an EE accumulator model.
// Finite normal/zero inputs and intermediates only; dt and parent_weight >= 0.
// Failure leaves state unchanged. The caller supplies and retains the pause latch.
TransitionBlendResult step_transition_blend(TransitionBlendState& state,
    float parent_weight, float dt, bool paused);

}
