#pragma once
// Camera collision body (camera +70, vtable 004dad90) for the static world.
//
// 0020df80 (body vtable +c) bounds the boom points (each pushed 1.72 along the
// camera's forward row), the look target and the pivot, expanded by
// 1.72 + 0.25. 00181270 gathers world triangles in that box into the shared
// collection that 0020dee0 (vtable +10) stores at body +4.
//
// 0020d680 then sweeps an oriented ellipsoid (radius 1.72, scale 1, 0.5, 0.96,
// basis = camera orientation rows; 003bf750 -> 003c66b0, static executor
// 003bd900 with no surface masks):
//   A: from the look target to the pivot; a hit moves the pivot to the
//      contact centre;
//   B: from that pivot to (point + forward * 1.72); a hit sets body +60 and
//      returns contact centre - forward * 1.72.
// The body offset +b0 lerps toward +a0 (cleared by 00280470 on foot), so it
// stays zero here and is rejected otherwise.
#include "camera_rig.hpp"
#include "collision_collection.hpp"
#include <span>

namespace omega {

Bounds camera_body_bounds(const CameraBody&);
CameraCollisionResult limit_camera_point(const CameraBody&, const RootVector& point,
    std::span<const FaceCandidate> world_candidates);

}
