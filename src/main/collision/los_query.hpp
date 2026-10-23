#pragma once
#include "animation_root.hpp"
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace omega {
// Authored input records corresponding to the expanded triangles consumed by
//003c37a0/003c45b0, already collected in the original query coordinate space.
struct LosTriangle {
    std::array<RootVector,3> vertices;
    RootVector normal;
    std::uint8_t flags{};
    std::array<std::uint32_t,4> source{}; // Preserved opaque+60 descriptor words.
};
struct LosRay {
    RootVector origin,displacement;
    std::uint8_t required_flags{},excluded_flags{};
};
struct LosHit {
    RootVector normal,point;
    std::array<std::uint32_t,4> source;
    std::size_t candidate_index{};
};
struct LosResult {
    RootVector displacement;
    std::optional<LosHit> hit;
    std::size_t visited{},filtered{};
};
// Fresh ordered intersection kernel only; no world discovery or service façade.
// Flag2 normal flips persist even when the candidate misses. The authored host
// transaction publishes those flips only if the entire finite-domain query succeeds.
LosResult trace_los_triangles(const LosRay&,std::span<LosTriangle>);

// The executor 003c3a60 (the list 00504030's queries): the first pass skips the
// candidates with a pass flag (and is otherwise the kernel above); when one was
// seen, the second pass 003c4a80 reports every pass-flag candidate (with the
// required and without the excluded flags) the first-pass-shortened segment
// crosses, in candidate order, without recording it as the hit.
struct LosPassCrossing {
    RootVector point;
    std::size_t candidate_index{};
};
struct LosPassResult {
    LosResult first;
    std::vector<LosPassCrossing> crossings;
};
LosPassResult trace_los_pass_triangles(const LosRay&,std::uint8_t pass_flags,std::span<LosTriangle>);
}
