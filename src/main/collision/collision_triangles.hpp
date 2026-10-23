#pragma once
#include "animation_root.hpp"
#include <functional>
#include <span>
#include <vector>

namespace omega {
using CollisionMatrix=std::array<float,16>;
struct CollisionTriangleSource {
    std::uint64_t geometry{}; // Authored stable handle, never an original pointer.
    std::uint32_t triangle{};
    std::uint64_t object{}; // Live owning object; zero only for isolated resource probes.
    bool operator==(const CollisionTriangleSource&) const = default;
};
struct CollisionTriangleDraft {
    std::array<RootVector,3> vertices;
    float coefficient{};
    std::array<std::uint8_t,16> surface; // Raw COL metadata; runtime material remapping is separate.
    std::array<bool,3> edge_flags{}; // Original +50 is false; +51/+52 come from triangle bytes11/12.
    CollisionTriangleSource source;
};
struct ExpandedCollisionTriangle : CollisionTriangleDraft {
    RootVector normal{};
};
// The resolver is REQUIRED even for static geometry (return nullopt). It runs
// after vertex/coefficient copies, at the emitter's fresh owner lookup point.
// The caller captures the object identity and resolves/refreshes its CURRENT
// owner, independently of the earlier broad-phase owner. Effects are external.
// Backing COL storage must remain allocated at a stable size across callbacks.
using CollisionTriangleOwner=std::function<std::optional<CollisionMatrix>()>;
CollisionTriangleDraft expand_col5_triangle(std::span<const std::uint8_t> bytes,
    CollisionTriangleSource source,const CollisionTriangleOwner& owner);
// Original003c1020 runs after gathering: cross of transformed edges times the
// retained coefficient, no normalization or deduplication. Pure atomic output.
std::vector<ExpandedCollisionTriangle> finalize_collision_triangles(std::span<const CollisionTriangleDraft>);
// The same into a caller-kept vector (cleared first; checkpoint 132win11, port-side speed only).
void finalize_collision_triangles(std::span<const CollisionTriangleDraft>,std::vector<ExpandedCollisionTriangle>& result);
}
