#pragma once
#include "collision_slide.hpp"
#include "collision_triangles.hpp"
#include "moving_collision_slide.hpp"
#include <functional>

namespace omega {
// Original edge/vertex +30 reconstruction boundary: caller resolves the fresh
// COL/owner triangle from this candidate identity. Called after EACH hit pass.
using ActorBodySourceFaceNormal=std::function<Vec3(std::uint32_t source_id)>;
struct ActorBodyQueryInput {
    // Values after the original 1e7f40 movement adjustment. No guessed root or
    // animation-to-physics displacement is supplied by this component.
    Vec3 resolved_origin,resolved_displacement;
    float primary_position_z{},radius_370{},parameter_390{};
    float height_offset_unflagged{},height_offset_flag1{}; // runtime4a0338/340
    std::uint8_t mode_14{};
    std::uint32_t flags_34{};
    bool supported_dd{},override_9c0{};
    float blend_a20{},secondary_weight{},tertiary_weight{};
};
struct ActorBodyQueryPlan {
    bool transformed{};
    Vec3 center,scale{1,1,1};
    // Transformed path: the caller's center W, kept as the query's stored
    // center (+f0, 3c6f5c). The constructor builds its placement with W forced
    // to 1 (3c67f4), so only the moving executor's component-origin change
    // (3bdbbc) reads it.
    float center_w{1};
    CollisionMatrix query_to_world{},world_to_query{},candidate_normal_to_query{},face_normal_to_world{};
    SlideQueryState initial_query;
    unsigned passes{1};
};
struct ActorBodyQueryResult {
    SlideQueryState query_state;
    Vec3 world_displacement;
    std::optional<FaceContact> world_contact; // original +20 radial/face normal, +50 point
    // Original +30 equals +20 on face hits. Edge/vertex +30 requires a fresh
    // source/owner reconstruction via1e60c0; absent without an explicit resolver,
    // never replaced by the radial normal.
    std::optional<Vec3> source_face_normal;
};
ActorBodyQueryPlan make_actor_body_query(const ActorBodyQueryInput&);
// Common transformed constructor after the caller has resolved its live root
// branch. Consumes displacement, radius, mode/flags, support and blend only;
// original origin, root weights and height inputs are already resolved here.
// center_w is the center's W lane as the caller computed it (any finite value).
ActorBodyQueryPlan make_transformed_actor_body_query(const ActorBodyQueryInput&,Vec3 center,Vec3 scale,float center_w=1);
// Static-only executor: candidates have no active movement-component history.
// World-coordinate candidates are already gathered and owner-expanded. Keeps
// order/source IDs. Sphere-path normal flips commit; transformed-path copies
// leave world candidates untouched, matching the separate query collection.
// Resolver side effects belong to the caller transaction and cannot be undone
// by this function. Candidate storage/content must stay fixed across callbacks.
ActorBodyQueryResult run_static_actor_body_query(const ActorBodyQueryPlan&,std::span<FaceCandidate> world_candidates,
    const ActorBodySourceFaceNormal& source_face_normal={});
// Original moving-history executor, with fresh source-component lookup. The
// caller supplies actual component state; absent components resolve to zero.
ActorBodyQueryResult run_actor_body_query(const ActorBodyQueryPlan&,std::span<FaceCandidate> world_candidates,
    const MovingSlideHost&,const ActorBodySourceFaceNormal& source_face_normal={});
}
