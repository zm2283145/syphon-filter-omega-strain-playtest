#pragma once
#include "collision_triangles.hpp"
#include "geometry.hpp"
#include <optional>
#include <span>
#include <vector>

namespace omega {
struct ActorMovementRelationNode {
    std::optional<std::size_t> parent;
    std::vector<std::size_t> children;
};
// Concrete1e8ca0: ascend to the source hierarchy root, then search its ordered
// descendants for the target. Caller binds actual relation nodes, not actor IDs.
// Supported snapshots are coherent acyclic forests, at most65,536 nodes.
bool actor_movement_related(std::span<const ActorMovementRelationNode>,std::size_t source,std::size_t target);
// Resolved reads at the original pair-call boundary. Controller callbacks,
// dirty transform refresh and relation-list lookup remain with their live owner.
struct ActorMovementPairInput {
    bool self_controller_54{},other_controller_54{};
    bool other_flag_2c{},other_flag_2d{},other_flag_2e{};
    std::uint8_t other_mode_108{};
    bool physical_enabled_3d0{},same_actor{},excluded_by_relation{};
    RootVector other_position{}; // target virtual+e0
    Bounds self_world_bounds,other_local_bounds;
    CollisionMatrix other_world_transform{}; // refreshed component+50 matrix+10
    float factor{};
};
struct ActorMovementPairResult {
    RootVector endpoint;
    bool adjusted{}; // original return value; can be true with factor zero
};
// 00182f80 / 00182f30 / 00182f00 over the eight corners with VU arithmetic: a
// result that overflows clamps to +-FLT_MAX; a lane whose vmadda product
// overflows is indeterminate (NaN), which the pair rejects only if it reads
// it (it reads Z alone). Inverted local bounds (the constructor's sentinel
// 00183010, minimum +FLT_MAX and maximum -FLT_MAX) are accepted.
Bounds actor_movement_world_bounds(const Bounds& local,const CollisionMatrix&);
ActorMovementPairResult adjust_actor_movement_pair(const RootVector& endpoint,const ActorMovementPairInput&);
// The pair's horizontal radius test alone (same ordered arithmetic); a live
// caller uses it to fetch target bounds only for nearby targets, as 1e8760 does.
bool actor_movement_pair_in_range(const RootVector& endpoint,const RootVector& other_position);
// Original ordered outer-loop factors; ordinary=15*dt, accelerated=22*dt,
// each capped at one. Actor eligibility decides which factor is actually used.
struct ActorMovementFactors {float ordinary{},accelerated{};};
ActorMovementFactors actor_movement_factors(float elapsed);
// 1e86e8 delta then 1e29b4..29f8 primary-weight application. Full XYZW math.
RootVector actor_movement_adjustment_delta(const RootVector& original,const RootVector& adjusted);
RootVector apply_actor_movement_adjustment(const RootVector& intended_displacement,
    const RootVector& adjustment,float primary_weight);
}
