#pragma once
#include "animation_rotation_curve.hpp"
#include "animation_vector_curve.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace omega {
struct ActorCurveUpdateState {
    AnimationVectorCurveState position;
    // Original words3030/3034/3038, preserved without inventing flag meanings.
    std::array<std::uint32_t,3> control_flags3030{};
};
ActorCurveUpdateState make_actor_curve_update(float position_damping);
struct ActorCurveUpdateHost {
    // Actual byte31b0, read after the angle curve advances.
    std::function<bool()> position_enabled;
    // Actual selected root+d0 and resolved WORLD base placement. These must
    // come from the same root3290, not the published blended pose transform.
    std::function<float()> root_weight;
    std::function<RootVector()> root_world_position;
    // Original position-only setter, including parent conversion and descendant
    // invalidation when an attachment tree owns the root.
    std::function<void(const RootVector&)> publish_root_world_position;
};
// Exact local tail001b2218..2314 inside an already eligible NPC logic update.
// The caller applies outer3269 and world traversal gates. Run before model
// evaluation resets root weights. Heading is separately owned by its stack.
// Both curve/control states commit together. Host side effects belong to the
// caller's transaction; they cannot be undone if publication throws.
void update_actor_curves(AnimationRotationCurveState& heading,ActorCurveUpdateState& state,
    float elapsed,const ActorCurveUpdateHost& host);
}
