#pragma once
// Port-only (checkpoint 132md11, research/MODDING.md "Speed"): automatic levels of detail for a mod's dense replacement
// models. Not game code. A quadric-error simplifier (Garland and Heckbert) by half-edge collapse: a vertex moves onto a
// neighbour, so every level indexes the model's own vertices (one vertex buffer, an index range per level).
//
// Attributes are kept by construction: only a vertex alone at its position (no UV / normal seam through it) collapses,
// an interior one onto any neighbour and a border one only along its border; seams, non-manifold and degenerate
// corners stay. A collapse that flips or slivers a triangle, or breaks the link condition, is refused.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace omega::gfx {

struct SimplifiedLevel {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    // The largest collapse error taken so far: the square root of the merged quadric's area-weighted mean squared
    // distance, in position units (an estimate of how far the level's surface is from the original).
    float error = 0;
};

struct SimplifyOptions {
    float ratio = 0.25F;              // each level about this fraction of the previous one's triangles
    std::size_t min_triangles = 512;  // no level below this
    float min_reduction = 0.1F;       // a level is kept only if it removes at least this fraction of the previous one's
    std::size_t max_levels = 6;
};

// The coarser levels of a mesh, finest first (none when it does not simplify). Deterministic: the same input gives the
// same levels. Throws on an index outside `positions`.
std::vector<SimplifiedLevel> simplify_levels(std::span<const std::array<float, 3>> positions,
                                             std::span<const std::array<std::uint32_t, 3>> triangles,
                                             const SimplifyOptions& options = {});

}  // namespace omega::gfx
