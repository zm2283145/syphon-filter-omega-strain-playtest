#pragma once
#include "collision_face.hpp"

namespace omega {
enum class SlideQueryMode { projection=4,vertical_projection=5 };
struct SlideQueryState {
    FaceQuery query;
    SlideQueryMode mode{SlideQueryMode::vertical_projection};
    float vertical_normal_scale{1}; // original +114, not implicitly clamped
    bool check_corners{true}; // original +118
    Vec3 displacement,direction;
    float displacement_squared{};
    Bounds bounds;
    std::optional<FaceContact> contact;
    std::vector<Vec3> normal_history;
    std::size_t visited{},filtered{},aabb_pass{};
};
// Static modes4/5 with homogeneous points/vectors implied by this XYZ API.
// Construction is the only history reset. Candidate order and duplicates matter.
SlideQueryState make_slide_query(const FaceQuery&,float vertical_normal_scale=1,bool check_corners=true,
    SlideQueryMode mode=SlideQueryMode::vertical_projection);
// Stages state and mutable two-sided candidate normals; commits only on success.
// Repeated calls continue the same query, including U and prior contact history.
void consume_slide_candidates(SlideQueryState&,std::span<FaceCandidate>);
}
