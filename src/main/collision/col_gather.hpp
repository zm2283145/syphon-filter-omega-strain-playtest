#pragma once
#include "geometry.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace omega {
struct ColGatherResult {
    std::vector<std::uint32_t> triangle_indices;
    std::size_t nodes_visited{},leaves_visited{},triangle_references{},triangle_aabb_pass{},surface_rejected{};
};
// Original local-space COL5 octree/leaf gather order, with an explicit AABB
// and surface exclusion byte. Does not choose world objects, transform owners,
// perform contact response, or deduplicate repeated triangle references.
ColGatherResult gather_col5_triangles(std::span<const std::uint8_t> bytes,
    const Bounds& query,std::uint8_t excluded_surface_flags);
// The query-independent part of the gather (header, tables, tree links, leaf lists,
// triangle bounds and surface flags), parsed and validated once per resource so
// repeated queries only traverse. Same results and counters as the span overload.
struct Col5Index;
std::shared_ptr<const Col5Index> index_col5(std::span<const std::uint8_t> bytes);
ColGatherResult gather_col5_triangles(const Col5Index& index,
    const Bounds& query,std::uint8_t excluded_surface_flags);
// The same gather into a caller-kept result (checkpoint 132win11, port-side speed only): the
// counters are reset and triangle_indices is cleared, keeping its storage across queries.
void gather_col5_triangles(const Col5Index& index,const Bounds& query,
    std::uint8_t excluded_surface_flags,ColGatherResult& result);
}
