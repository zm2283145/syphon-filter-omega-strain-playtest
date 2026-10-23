#pragma once
#include "actor_ladder.hpp"
#include <istream>

namespace omega {
// Loaded from the user's verified extraction. Neither node identities nor
// tuning values are compiled into the native implementation.
class ActorLadderBindings {
public:
    ActorLadderTuning geometry() const { return geometry_; }
    float dismount_rungs() const { return dismount_rungs_; }
    MotionRequest resolve(ActorLadderMotion motion) const;
    std::size_t resolve_exit(ActorLadderExitMotion motion) const;
    float exit_crossfade() const;
private:
    ActorLadderBindings() = default;
    ActorLadderTuning geometry_;
    float dismount_rungs_{};
    std::array<MotionRequest, 7> motions_;
    std::optional<std::array<std::size_t,8>> exits_;
    std::optional<float> exit_crossfade_;
    friend ActorLadderBindings read_actor_ladder_bindings(std::istream&, const AnimationGraph&);
};
// Bounded, versioned configuration; all seven roles are required exactly once.
// The supplied graph must be the matching extraction. Bounds checks do not
// authenticate provenance: the owning loader still verifies the source ISO.
ActorLadderBindings read_actor_ladder_bindings(std::istream& input, const AnimationGraph& graph);
}
