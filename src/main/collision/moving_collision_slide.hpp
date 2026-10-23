#pragma once
#include "collision_slide.hpp"
#include "animation_root.hpp"
#include <functional>

namespace omega {
using MovingSlideComponent=std::uint64_t;
using MovingSlideMatrix=std::array<float,16>;
struct MovingSlideHost {
    // The source ID resolves the actual source object, then its component+54.
    // Zero is an absent component, never a fabricated identity for lookup failure.
    std::function<MovingSlideComponent(std::uint32_t)> component;
    std::function<bool(MovingSlideComponent)> enabled;
    std::function<MovingSlideMatrix(MovingSlideComponent)> relative_matrix;
};
struct MovingSlideShape {
    RootVector world_origin; // original query+f0; W is the caller's center W
    MovingSlideMatrix world_to_query; // original query+1a0
};
// Original3bda00 ordered component-origin changes around mode4/5 consumption.
// Affine matrices and homogeneous point/vector W only. Shape omitted for sphere.
// Stages query/candidate changes, but live getter side effects cannot roll back.
void consume_moving_slide_candidates(SlideQueryState&,std::span<FaceCandidate>,
    const MovingSlideHost&,const std::optional<MovingSlideShape>& shape=std::nullopt);
}
