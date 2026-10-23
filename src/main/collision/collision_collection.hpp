#pragma once
#include "collision_triangles.hpp"
#include "live_gather.hpp"
#include "collision_face.hpp"

namespace omega {
// Resolver follows the route-specific original geometry/resource indirection
// and returns caller-owned raw COL5 bytes. Storage and its contents must remain
// fixed throughout collection; world object fields/callbacks remain live.
using CollisionColResolver=std::function<std::span<const std::uint8_t>(GatherIdentity,GatherRoute)>;
struct CollisionCollection {
    LiveGatherStats traversal;
    std::vector<ExpandedCollisionTriangle> triangles;
};
// Owns clear/emit/finalize; those three callbacks in the supplied host must be
// empty. All live world lookups remain required. The whole returned collection
// is staged; host callback effects require a surrounding caller transaction.
CollisionCollection collect_live_col5(const GatherBounds&,LiveGatherHost,
    const CollisionColResolver&,std::uint8_t excluded_surface_flags);
// A query-local ordinal connects the bounded XYZ kernels to full-width source
// identities. Keep this batch alive and unchanged across retained query passes.
struct CollisionCandidateBatch {
    std::vector<FaceCandidate> candidates;
    std::vector<CollisionTriangleSource> sources;
};
CollisionCandidateBatch prepare_collision_candidates(std::span<const ExpandedCollisionTriangle>);
// The same into a caller-kept batch (cleared first; checkpoint 132win11, port-side speed only).
void prepare_collision_candidates(std::span<const ExpandedCollisionTriangle>,CollisionCandidateBatch& result);
CollisionTriangleSource resolve_collision_contact(const CollisionCandidateBatch&,const FaceContact&);
// Original1e60c0 COL5 branch: re-resolve the retained geometry, then copy source
// vertices before a fresh owner lookup. This is not the cached contact normal.
ExpandedCollisionTriangle refresh_collision_source_triangle(const CollisionTriangleSource&,
    const std::function<std::span<const std::uint8_t>(std::uint64_t)>& geometry,
    const std::function<std::optional<CollisionMatrix>(std::uint64_t)>& owner);
}
