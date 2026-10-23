#pragma once
#include "root_attachments.hpp"

namespace omega {
struct ActorResponsePlacementState {
    float support_height{},child_offset{}; // physical +e0,+394
    bool operator==(const ActorResponsePlacementState&) const = default;
};
struct ActorResponsePlacementInput {
    RootAttachmentId primary{};
    std::optional<RootAttachmentId> secondary;
    RootVector resolved_position{};
    bool floor_hit{},flag_dc{};
    float floor_height{};
    std::uint32_t flags34{};
};
enum class ActorRootPublication {none,position,secondary_placement};
struct ActorResponsePlacementResult {
    RootVector resolved_position;
    ActorRootPublication publication{};
};
// Original 001e4be4..001e4d24 only. Inputs are already resolved by collision
// response; this does not run the solver/height curve or later orientation turn.
// Root IDs are the actual earlier-resolved primary/secondary registry entries.
// Tree and retained outputs commit together; no arbitrary external callbacks.
ActorResponsePlacementResult apply_actor_response_placement(ActorResponsePlacementState&,
    RootAttachments&,const ActorResponsePlacementInput&);
struct ActorResponseOrientationInput {
    RootAttachmentId secondary{};
    RootVector target{};
    std::uint32_t flags24{};
    bool flag_7a2{};
    float elapsed{};
};
// Original 001e4d28..001e4dcc. Disabled gates do not inspect root/target/time.
// Active finite nonnegative elapsed produces UNCLAMPED 3*elapsed normalized
// interpolation and resets the secondary root's counter.
bool apply_actor_response_orientation(RootAttachments&,const ActorResponseOrientationInput&);
}
