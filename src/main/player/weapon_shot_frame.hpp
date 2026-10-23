#pragma once
// The shot frame of the controller virtual +64 (COMBAT.md §4, step 5): the
// player component's 00176400 and the AI component's 0014f380. Outside the
// aim view both take the hand's attach point of the actor model (actor +60)
// through 00385240 -> 00385370 -> 00385420: the attach point's world matrix
// (its skeleton node through the model component, 0019f350, or the component
// alone without a node collection) times the point's local matrix, the
// attached part's muzzle offset (+130) added to the origin, and the rows
// reordered (2, 0, 1, 3). In the aim view the player uses the camera
// transform with the origin moved one unit along the camera's second row.
// The matrices are inputs: reading them from the live model and camera is
// the host's (the native skeleton evaluation provides node matrices).
#include "weapon_discharge.hpp"
#include <optional>

namespace omega {

// One hand attach point of the actor model: node byte +2450+hand and the
// local matrix at +2460+0x50*hand (rows +2470, +2480, +2490, then +2460).
struct WeaponAttachPoint {
    std::uint8_t node{};
    ShotFrame local{};
};
// 0019f350: row i = C0*N[i].x + C1*N[i].y + C2*N[i].z + C3*N[i].w.
ShotFrame shot_frame_node_world(const ShotFrame& node,const ShotFrame& component);
// 00385240 for the hand: `world` is the node's world matrix (or the model
// component when the model context +2c04 has no node collection, byte +14);
// `muzzle_offset` is the attached part's +130 (model +2410[hand] non-null).
ShotFrame weapon_muzzle_frame(const ShotFrame& world,const WeaponAttachPoint& point,const std::optional<RootVector>& muzzle_offset);
// 00176400 in the aim view: the camera's transform component (component
// +1a0, its +50, matrix +10..+4c) with row 3 = ((r0*0 + r1*1) + r2*0) + r3*1.
ShotFrame player_aim_view_frame(const ShotFrame& camera);

}
