#pragma once
#include "animation_root.hpp"
#include <array>
#include <cstdint>

namespace omega {
// Teleport Message handler 001bc4b0 arithmetic (ACTOR_TELEPORT.md §2). The
// owning actor applies these to its roots, model anchors and other fields.
// Every result is checked: nonfinite or subnormal arithmetic throws.
enum class ActorTeleportMode : std::uint8_t {
    translate=0,   // row 3 is the target flor position
    rotate=1,      // rows 0-2 are the absolute rotation
    place=2,       // modes 0 and 1 together (the revive's mode)
    offset=3,      // row 3 is the delta itself
    compose=4,     // rows 0-2 composed with the current flor orientation
};
using ActorTeleportMatrix=std::array<RootVector,4>;   // message rows 0..3
// Throws for a mode above 4.
bool actor_teleport_translates(ActorTeleportMode mode);
bool actor_teleport_rotates(ActorTeleportMode mode);
// §2.2 step 1: modes 0/2 subtract the flor placement position from row 3 on
// all four lanes; mode 3 returns row 3 unchanged.
RootVector actor_teleport_delta(ActorTeleportMode mode,const ActorTeleportMatrix& matrix,const RootVector& floor_position);
// 001bc490: XYZ added, W kept.
RootVector add_actor_teleport_xyz(const RootVector& value,const RootVector& delta);
// 001e9300 / actor +34c0: all four lanes added.
RootVector add_actor_teleport_lanes(const RootVector& value,const RootVector& delta);
// §2.2 step 2 on one unparented root: pose-cache position (+90) and placement
// position (+60) XYZ += delta, W kept, both dirty bytes set. Orientation,
// pose orientation, motion cache and counter are untouched.
void translate_actor_teleport_root(RootAnimationState& root,const RootVector& delta);
struct ActorTeleportRotation {
    RootVector orientation{0,0,0,1};   // q
    float heading{};                   // h, written to the selected heading history entry
};
// §2.3 steps 1-2. floor_orientation is the flor placement orientation (+50),
// read by mode 4 only. The rows-to-quaternion block is camera_matrix_to_quaternion
// (0027d808), which omits the VU "0 +" lane moves that only matter for a -0
// diagonal difference.
ActorTeleportRotation actor_teleport_rotation(ActorTeleportMode mode,const ActorTeleportMatrix& matrix,
    const RootVector& floor_orientation);
// §2.3 steps 3-4 on the unparented flor root: pose orientation (+80) =
// q (x) (conj(old +50) (x) +80), +50 = q, both dirty bytes set, counter +160 = 0.
void rotate_actor_teleport_floor_root(RootAnimationState& root,const RootVector& orientation);
}
