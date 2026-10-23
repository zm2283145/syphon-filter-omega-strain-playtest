#pragma once
#include "actor_physical_prediction.hpp"
#include <cstdint>

namespace omega {
struct ActorPhysicalMode {
    std::int32_t value{};
    std::uint32_t bits{};
    bool operator==(const ActorPhysicalMode&) const = default;
};
struct ActorPhysicalHistory {
    ActorPhysicalMode previous,current,pending; //18/20/28
    std::uint32_t previous_flags{},current_flags{},pending_flags{}; //30/34/38
    float radius{},target_radius{}; //370/374
    bool operator==(const ActorPhysicalHistory&) const = default;
};
// Call after requested initialization, before prediction. Only the low thirteen
// mode bits are copied; high bits belong to each destination and remain there.
ActorPhysicalHistory advance_actor_physical_history(const ActorPhysicalHistory&,float elapsed);
bool actor_physical_mode_runs_response(std::int32_t mode);

// 1e9630 then 1e9600: retain original subtraction/addition rounding. This resets
// the integrator only, not the root, contacts, or complete physical actor.
ActorPhysicalPredictionState reset_actor_physical_integrator(
    const ActorPhysicalPredictionState&,const RootVector& target);

struct ActorPhysicalTail {
    RootVector contact_motion{}; //1e0
    std::uint8_t initialized{},reset_requested{}; //10/11
    bool operator==(const ActorPhysicalTail&) const = default;
};
// Separate original boundaries: override clears after support capture; tail
// clears only after auxiliary probes and support-height notifications succeed.
void clear_actor_physical_override(ActorPhysicalPredictionState&);
void finish_actor_physical_step(ActorPhysicalPredictionState&,ActorPhysicalTail&,float elapsed);

// Physical +e4/+1a8 come from the water-surface probe 001e5f30 (surface flag
// 0x80), so this classifies water contact despite the historical name.
// Both the frame entry and 1e5cf0 use this strict near-horizontal test. The
// runtime unavailable-height sentinel is supplied from verified game data.
float capture_actor_support_height(float height,float unavailable_height,float normal_z);
struct ActorSupportHeightChange {
    float height{};
    bool supported{},changed{},lost{};
};
ActorSupportHeightChange classify_actor_support_height(
    float previous_capture,float height,float unavailable_height,float normal_z);
}
