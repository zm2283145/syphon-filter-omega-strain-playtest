#pragma once
#include "actor_physical_prediction.hpp"
#include <functional>

namespace omega {
struct ActorPhysicalInitialContact {
    RootVector position{},direction{};
    float scalar{}; // One float only: original +60 / +90, not a cleared vector.
    bool operator==(const ActorPhysicalInitialContact&) const = default;
};
struct ActorPhysicalInitializationState {
    ActorPhysicalPredictionState integrator;
    std::array<ActorPhysicalInitialContact,2> contacts;
    std::uint8_t flag_dc{},flag_dd{},primary_support_enabled{};
    float base_height{},contact_height{};
    RootVector plane{};
    float scalar_2c4{},scalar_394{};
    bool operator==(const ActorPhysicalInitializationState&) const = default;
};
struct ActorPhysicalInitializationHost {
    // Returns the actual registry result, including its established fallback.
    std::function<std::size_t()> root;
    std::function<RootParentTransform(std::size_t)> world_pose;
    // Original four-lane vector at physical pointer+8, member+50. No default.
    std::function<RootVector()> external_vector;
    std::function<RootParentTransform(std::size_t)> world_placement;
};
// Original001e9380. Captures one root identity, reads pose, then external vector,
// computes the complete matrix/vector product, and separately reads placement.
// Own state commits on success; host/cache side effects are not rolled back.
// This is requested reset, not a physical constructor or frame completion.
void initialize_actor_physical_state(ActorPhysicalInitializationState&,const ActorPhysicalInitializationHost&);
}
