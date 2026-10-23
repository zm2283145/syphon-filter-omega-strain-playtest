#pragma once
#include "animation_root.hpp"
#include <cstddef>
#include <functional>
#include <optional>

namespace omega {
using ContactIdentity=std::uint64_t;
struct ActorContactEventInput {
    bool successful_hit{};
    ContactIdentity destination{};
    std::array<std::uint8_t,4> root_key{}; // Verified runtime key at004a03c8.
};
struct ActorContactEvent {
    ContactIdentity destination{},object{};
    std::uint8_t subtype{},flags{5};
    std::uint32_t transport{1};
};
struct ActorContactEventHost {
    std::function<ContactIdentity()> contact_object; // First word at result+64.
    std::function<ContactIdentity(ContactIdentity)> component; // Object+54, zero absent.
    std::function<bool(ContactIdentity)> eligible; // Object+2d && object+2c && !object+2e.
    std::function<RootVector(ContactIdentity)> component_velocity; // Component+a0.
    // Resolve current model+named key and perform original world-root refresh.
    std::function<void(const std::array<std::uint8_t,4>&)> refresh_world_root;
    std::function<float()> normal_z; // Result+28, only read for subtype2 candidate.
    std::function<ContactIdentity(std::size_t)> comparison_object; // State+1f0+i*4.
    std::function<RootVector()> actor_velocity; // State+310, fresh after refresh.
    std::function<RootVector()> normal; // Result+20, fresh for relative-motion branch.
    std::function<void(const ActorContactEvent&)> send; // Required synchronous delivery.
};
// Original001e5a10 producer only, with retained/fresh reads at the original gates.
// No-hit skips the host. Successful-hit calls require a complete concrete host.
// External refresh/receiver effects are not rolled back on a later exception.
std::optional<ActorContactEvent> produce_actor_contact_event(
    const ActorContactEventInput&,const ActorContactEventHost&);
}
