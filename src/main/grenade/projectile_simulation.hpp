#pragma once
#include "animation_root.hpp"
#include <cstdint>
#include <optional>

namespace omega {
// Retained fields read/written by the prefix of original0025ad80. This is
// explicit caller state, not a substitute projectile constructor/world object.
struct ProjectileIntegrationState {
    RootVector position{0,0,0,1}, previous_position{0,0,0,1};
    RootVector velocity{}, acceleration{}, force{}, launch{};
    float inverse_mass{}, previous_dt{}, age{};
    bool retired{}, motion_disabled{}, first_launch{true};
    bool operator==(const ProjectileIntegrationState&) const = default;
};
struct ProjectileIntegrationInput {
    float dt{};
    RootVector gravity{}; // Runtime004f5430 vector, before type7 adjustment.
    float launch_force_scale{}; // Runtime0048af30, independently of mass+60.
    std::uint8_t definition_type{};
    float type7_gravity_offset{}; // Runtime0048af98.
};
enum class ProjectileIntegrationBoundary {
    retired, await_lifetime_continuation, await_world_continuation
};
struct ProjectileIntegrationDraft {
    ProjectileIntegrationState state;
    ProjectileIntegrationBoundary boundary{ProjectileIntegrationBoundary::retired};
    // Raw XYZW position difference. The later query separately forces W=0
    // and its origin W=1, after the previous-position cutoff/retirement branch.
    std::optional<RootVector> displacement;
    // Original type7 restores gravity by subtraction, which can round away
    // from its initial value. Caller owns publication of this shared write.
    std::optional<float> gravity_z_after;
};
// Pure draft: failure leaves all inputs unchanged. A successful draft still
// requires the original cutoff, synchronous world query, response and lifetime
// continuation before an owning world transaction can publish a complete step.
ProjectileIntegrationDraft prepare_projectile_integration(
    const ProjectileIntegrationState&,const ProjectileIntegrationInput&);
}
