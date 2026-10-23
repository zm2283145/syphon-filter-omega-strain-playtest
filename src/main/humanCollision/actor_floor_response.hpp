#pragma once
#include "animation_root.hpp"
#include <cstdint>
#include <optional>

namespace omega {
struct ActorFloorResponseState {
    RootVector force{}; // physical +330
    bool flag_dc{},flag_dd{},contact_e8{};
    float height_distance{},contact_blend{}; // +2c4,+a20
    bool operator==(const ActorFloorResponseState&) const = default;
};
struct ActorFloorContact { float height{},normal_z{}; };
struct ActorFloorResponseInput {
    // Resolved roots selected by original key globals 4a03c8/d0/d8.
    float primary_weight{},secondary_weight{},tertiary_weight{};
    // Captured at 1e270c BEFORE this region resets dd; not recomputed here.
    bool previously_supported{};
    std::uint32_t flags34{};
    bool flag13{},override_9c0{};
    // force_limit_scale is gravity Z (004f5438), normally game_initial_gravity[2].
    float reference_height{},velocity_z{},relative_velocity_z{},force_limit_scale{},mass{};
    std::optional<ActorFloorContact> contact;
};
struct ActorFloorResponseDraft {
    ActorFloorResponseState state;
    bool support_accepted{},force_added{};
};
// Numerical response at 1e4020..1e43a8 only. The caller supplies the already
// resolved probe result and retained support snapshot. Query execution, contact
// descriptor ownership, later fallback probes and root publication are separate.
ActorFloorResponseDraft apply_actor_floor_response(
    const ActorFloorResponseState&,const ActorFloorResponseInput&);
}
