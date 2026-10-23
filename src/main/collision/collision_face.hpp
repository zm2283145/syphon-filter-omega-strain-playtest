#pragma once
#include "geometry.hpp"
#include <optional>

namespace omega {

// Explicit diagnostic inputs, not original in-memory structs. Supplied candidates
// must already be in query coordinates and in the caller's chosen order.
struct FaceCandidate {
    std::array<Vec3, 3> vertices;
    Vec3 normal;
    std::uint8_t flags{};
    std::uint32_t source_id{};
    // Port-only: set after a caller maps world candidates into an ellipsoid query frame.
    // The rounded vertices and separately mapped normal of a sliver no longer agree to
    // the plane tolerance; the original uses them as mapped, so only the world-space
    // check (validate_face_candidate before mapping) applies.
    bool query_transformed{};
};
struct FaceQuery {
    Vec3 origin, displacement;
    float radius{};
    float min_start_distance{}, min_approach_cosine{}, min_direct_cosine{}, max_end_distance{};
    std::uint8_t required_flags{}, excluded_flags{};
};
struct FaceContact { Vec3 normal, point; std::uint32_t source_id{}; unsigned type{1}; };
struct FaceResult {
    enum class Status { none, hit, unsupported_edge_vertex } status{};
    Vec3 displacement;
    std::optional<FaceContact> contact;
    std::optional<std::size_t> unsupported_index;
    std::size_t visited{}, filtered{}, aabb_pass{};
};

// Fresh static, untransformed mode-0 query. Two-sided normals mutate in place.
// Unsupported fallback stops this diagnostic; no live actor should use partial results.
// Port-only invariant check of one world/static candidate (finite, nonzero, normal on plane).
void validate_face_candidate(const FaceCandidate& candidate);
FaceResult query_faces(const FaceQuery& query, std::span<FaceCandidate> candidates, bool include_edges = false);
// Edge/vertex primitives only: no face admission or mask gates. Uses the current
// result displacement; no hit leaves it intact. Arithmetic failure is atomic.
bool query_edge_vertices(const FaceQuery& query, FaceResult& result, const FaceCandidate& candidate);
// Static COL5 conversion in file order. This does not reconstruct broad-phase order
// or apply owner transforms. Flags come from the selected surface record.
std::vector<FaceCandidate> decode_col_faces(std::span<const std::uint8_t> bytes);
void write_face_result(std::ostream& out, const FaceResult& result,
                       std::span<const FaceCandidate> candidates);
}
