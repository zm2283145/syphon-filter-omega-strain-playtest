#pragma once
// Camera zoom body (base camera +170, vtable 004dad70, query +1c0 with the
// body pointer at +1f4 and the gather mode 4 at +1f0) used by the camera
// update 0017a640 while the field of view +c64 is below 1.1170107 (64 deg):
//   0020d3e0(body, +50 component matrix +10, +c64)  setup
//   001811c0(query, camera, elapsed)                 vtable +c 0020d130 (bounds),
//                                                   the gather 00181260(query +0,
//                                                   *(004ea0c0), bounds, 4),
//                                                   vtable +10 0020cf90 (sweep, fov)
//   +178 replaces +c64 as the field of view whose tangent the +a90 curve follows.
// The setup keeps the view rotation as a quaternion (+10), the eye (+30) and
// a reach along the forward row (+40); 0020cf90 sweeps the box
// 003bcee0 -> 003c6fc0 (executor 003bced0 -> 003c3220) along the reach and,
// on a hit, narrows the field of view to the contact distance. The gather of
// world triangles is a host service (CameraZoomHost); the host's sweep runs
// the native sweep_camera_zoom on what it gathered.
#include "animation_root.hpp"
#include "collision_face.hpp"
#include <array>
#include <functional>
#include <span>
#include <vector>

namespace omega {

struct CameraZoomBody {
    bool hit_4{};                  // +4: the last sweep hit
    float fov_8{};                 // +8: field of view (setup: the camera's +c64)
    RootVector rotation_10{};      // +10: quaternion of the matrix rows 0..2
    float scale_20{};              // +20: c * 1.6121628 / (c + 0.05), c = 1 / tan(fov / 2)
    float near_24{};               // +24: scale * 0.05
    RootVector eye_30{};           // +30: matrix row 3
    RootVector reach_40{};         // +40: matrix row 1 * (c * 1.6121628 - near)
};
struct CameraZoomBounds {
    RootVector minimum{}, maximum{};   // W = 1
};
// The arguments of 003bcee0 as 0020cf90 passes them (a1 is *(004ea0c0), the
// host's world context; t0 = 0).
struct CameraZoomSweep {
    RootVector eye{};                          // a2 = body +30
    RootVector reach{};                        // a3 = body +40
    std::array<RootVector,4> rows{};           // t1: rows of +10, row 3 = (0, 0, 0, 1)
    std::array<float,3> extents{};             // t2 = 004a3fb0: 1.0, 0.05, 0.75
    std::array<float,4> scalars{};             // f12..f15: 0.05, 0.05, 0, 1
    float fov{};                               // f16 = body +8
    float radius{};                            // f17 = 004a3fc0: 1.72
};
// The result object (constructor 003c0280) after 003bced0(result, 0, 0).
struct CameraZoomHit {
    bool hit{};                    // byte +0
    RootVector vector_80{};        // +80 (XYZ read)
};
struct CameraZoomHost {
    std::function<void(const CameraZoomBounds&)> gather;              // 00181260
    std::function<CameraZoomHit(const CameraZoomSweep&)> sweep;       // 003bcee0 + 003bced0
};
// One gathered world triangle for the zoom sweep: the candidate (world
// coordinates, as prepare_collision_candidates builds it) and the first float
// of its COL triangle record (ExpandedCollisionTriangle::coefficient), whose
// sign 003bf9e0 / 003c0020 (via 003c0180) applies to the projected normal.
struct CameraZoomCandidate {
    FaceCandidate face;
    float coefficient{};
};
// 003bcee0 -> 003c6fc0 then 003bced0 -> 003c3220 on the gathered candidates:
// the view box frame (+120 query-to-world, +1a0 world-to-query, both scaled
// by tan(fov / 2) * 0.05 across X and Z), the perspective pass 003bf760 (near
// clip 003bfab0 at Y = 1.72 / 0.05, projection 003bf900, normals 003c0020),
// the face query 003bf540 / 003bd900 along the projected reach, and the
// executor's unprojection of the final displacement into +80. `projected`
// receives the candidates in query order (diagnostics and goldens).
CameraZoomHit sweep_camera_zoom(const CameraZoomSweep&, std::span<const CameraZoomCandidate> world,
    std::vector<FaceCandidate>* projected=nullptr);

// 0020d3e0(body, matrix, fov).
void setup_camera_zoom(CameraZoomBody&, const std::array<RootVector,4>& matrix, float fov);
// 0020d130: the eye and the reach end widened by +20, then by 0.25.
CameraZoomBounds camera_zoom_bounds(const CameraZoomBody&);
// 0020cf90 up to the 003bcee0 call.
CameraZoomSweep camera_zoom_sweep(const CameraZoomBody&);
// 0020cf90 after 003bced0: +4 = hit; on a hit
// +8 = 2 * atanf(1 / ((+24 + |v.xyz|) * 0.6202847)).
void finish_camera_zoom(CameraZoomBody&, const CameraZoomHit&);
// 001811c0 for the zoom query: bounds, gather, sweep, finish.
void update_camera_zoom(CameraZoomBody&, const CameraZoomHost&);
// The 0020d3e0 rows-to-quaternion VU block (0020d404..0020d5a4).
RootVector camera_zoom_quaternion(const std::array<RootVector,4>& matrix);

}
