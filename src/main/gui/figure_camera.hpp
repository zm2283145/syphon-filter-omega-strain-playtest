#pragma once
// Checkpoint 132f8: the stats screen's figure camera and placements, 0035b390
// (GuiCharacterDisplay's callback 0035df30 over GuiScoresEquip's four display
// actors +1a4). Row-vector convention throughout: a point p maps to
// sum p[k] * row[k] (+ row 3 for a position), as the VU products do.
//  - The camera rotation R: Mtx44_SetRotation (0035b568) of the +1d0 / +1d4 /
//    +1d8 angles (cosf 00140e80 / sinf 00140e70 at 0035b4d4..0035b510), the
//    same formula as the hand rest quaternion's (upper_body_ik).
//  - The eye (0, -5, 1.5) through R (sp+300), the camera world [R; eye R]
//    inverted (Mtx_InvertRigid, 0035b678) as the view of record 005381f0 + 0xa80
//    (mode 0x15 at 005392d0, half fov 0x3f060a92, near 0x3f800001, far 40).
//  - Per actor s: identity x Rx(+1c0) x Ry(+1c4) x Rz(+1c8) (0035b790..
//    0035b83c: 001d3e20 returns its argument, 00132130, 0023b450), row 3 =
//    +1e0[s], the four rows through R (0035b84c..0035b8b8) and the result set
//    as the anchor's world placement (Root_SetWorldPlacement 0035bd80 on
//    'flor', or 0035bb20 on the empty key 004bb530 when 004ffba0 == 2; rows
//    0..2 as a quaternion, row 3 the position).
#include "ni_event_object.hpp"
#include <array>

namespace omega {
using FigureRows = std::array<std::array<float, 4>, 4>;
// 0035b4d4..0035b568: Mtx44_SetRotation (00140e20) of +1d0 / +1d4 / +1d8; row 3 = (0, 0, 0, 1).
FigureRows figure_camera_rotation(const std::array<float, 4>& angles);
// The eye (0, -5, 1.5) through R: the camera's world position.
std::array<float, 3> figure_camera_eye(const FigureRows& rotation);
// Any camera-frame eye through R (Checkpoint 132cc6: GuiAgentCreate's 0028d870 eye (+98, -+9c, +a0)).
std::array<float, 3> figure_camera_eye(const FigureRows& rotation, const std::array<float, 3>& eye);
// The record's P x A x view columns for a world point (c = sum col[k] p[k]),
// as eqmodify_figure_clip_2bfa90 for any rotation: view = (p - eye) R^T,
// A the record's y / z swap, P 003754a0's GL form of the record above.
std::array<std::array<float, 4>, 4> figure_camera_clip(const FigureRows& rotation);
// The same columns for any camera-frame eye (through R), half fov, near and far
// (Checkpoint 132cc6: the agent screens' record, half fov +94, near 2, far 15000).
std::array<std::array<float, 4>, 4> figure_camera_clip(const FigureRows& rotation, const std::array<float, 3>& eye,
                                                        double half_fov, double near_plane, double far_plane);
struct FigurePlacement {
    RootVector orientation{0, 0, 0, 1};
    RootVector position{0, 0, 0, 1};
};
// 0035b790..0035b8b8 for one actor: the X / Y / Z turns +1c0 / +1c4 / +1c8
// and the translation +1e0[s] through the camera rotation.
FigurePlacement figure_slot_placement(const std::array<float, 4>& turns, const std::array<float, 4>& translation,
                                      const FigureRows& rotation);
// A direction fixed in camera space in world space (d R): MANLIGHT's
// directions for the actor draw under this camera (386c28 / 387a60).
std::array<float, 3> figure_camera_direction(const FigureRows& rotation, const std::array<float, 3>& d);
}  // namespace omega
