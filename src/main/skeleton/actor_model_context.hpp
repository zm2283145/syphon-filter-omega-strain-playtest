#pragma once
#include "animation_root.hpp"
#include <cstddef>
#include <functional>

namespace omega {
struct ActorModelContextState {
    RootVector bounds_min{},bounds_max{},centroid{};
};
struct ActorModelContextNode {
    std::array<float,16> transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bool dirty{},bounds_enabled{},bounds_at_translation{};
    RootVector bounds_center{0,0,0,1};
    float radius{},center_weight{};
};
struct ActorModelContextHost {
    // The original obtains a fresh fixed begin/end range for each of the two
    // passes. Node identities/storage must remain stable within each pass.
    std::function<std::size_t()> count;
    std::function<ActorModelContextNode(std::size_t)> snapshot;
    // Original node virtual+58 when byte+50 is set. The next snapshot is fresh.
    // The view exposes in-progress fields; it must not outlive this callback.
    std::function<void(std::size_t,const ActorModelContextState&)> refresh;
};
// Original003a9220 then003a9090, under caller's freshly read context+14 gate.
// Owns only bounds/centroid: no model pose aggregate or root mutation occurs.
// Successful execution atomically publishes owned state. External refresh work
// belongs to the caller's transaction and is not rolled back by this function.
void update_actor_model_context(ActorModelContextState&,bool enabled,const ActorModelContextHost&);
}
