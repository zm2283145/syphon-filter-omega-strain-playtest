#pragma once
#include "animation_root.hpp"

namespace omega {
// Explicit retained input to001e9980's prediction region, after any requested
// initialization/history/parameter updates. This is not a physical constructor.
struct ActorPhysicalPredictionState {
    RootVector position{0,0,0,1},previous_position{0,0,0,1}; //2f0/300
    RootVector velocity{},acceleration{},force{},override_motion{}; //310/320/330/9d0
    float mass{},inverse_mass{},previous_dt{}; //2e0/2e4/2d0
    bool operator==(const ActorPhysicalPredictionState&) const = default;
};
struct ActorPhysicalPredictionDraft {
    ActorPhysicalPredictionState state;
    bool override_applied{},acceleration_limited{};
};
// Pure prediction through force clearing, immediately BEFORE001dfb80. Does not
// perform geometry gathering, contact response, root publication or gravity.
// previous_dt and override_motion intentionally remain unchanged: the original
// writes them only after the remaining physical stage has completed.
ActorPhysicalPredictionDraft prepare_actor_physical_prediction(
    const ActorPhysicalPredictionState&,float elapsed);
}
