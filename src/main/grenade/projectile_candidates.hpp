#pragma once
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace omega {
using ProjectileQueryMatrix=std::array<float,16>;
// Original22ba30 transposes only the upper3x3, then rotates negated translation
// when XYZ is nonzero. This is the original operation, not a general inverse.
ProjectileQueryMatrix prepare_projectile_query_frame(const ProjectileQueryMatrix& world_frame);
// Inputs already resolved by the object's original bounds/position virtuals.
// Caller owns weak identity checks, owner exclusion and synchronous reads.
struct ProjectileCandidateGeometry {
    std::uint64_t identity{};
    RootVector position{0,0,0,1},minimum{},maximum{};
};
struct ProjectileCandidateDistance {
    std::uint64_t identity{};
    float distance{};
};
// Exact proximity arithmetic22bbf8..22bd60. A returned value is frame-space Y,
// not a contact point, ray fraction or final collision decision.
std::optional<float> projectile_candidate_distance(const ProjectileQueryMatrix& query_frame,
    const RootVector& position,const RootVector& minimum,const RootVector& maximum,float segment_length);
// Diagnostic/host preparation for an already resolved ordered snapshot. Keeps
// duplicate entries and stable equal-distance order. Rejects a21st match: the
// original invokes a diagnostic and omits that candidate. No partial result is
// returned on overflow. Actual object callbacks and mutable requests remain
// the owning world's subsequent synchronous traversal.
std::vector<ProjectileCandidateDistance> prepare_projectile_candidates(
    const ProjectileQueryMatrix& world_frame,std::span<const ProjectileCandidateGeometry> objects,float segment_length);
}
