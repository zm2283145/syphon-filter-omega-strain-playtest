#pragma once
// Actor action 0x17 (00192800, through 001a92a0 table 0049e680): the
// running-jump request that the pad (0024b5d0 prelude, 0024d150) sends every
// frame while input +0x26 is set, i.e. inside a type-15/16 hotbox
// (CROUCH_ZONES_AND_RUNNING_JUMP.md). When the actor runs toward the target
// box's long axis, it sends itself action 0x15 (side 1) or 0x16 (side 2)
// with the crossing point; the take-off is 001914c0 (handle_actor_jump_to_grab).
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace omega {
// The message 0x17 (CAS action type 004e0180, built by 00195c80(0x17, 6)):
// +0x28 kind, +0x2c the target's +0x0c key (input +0x28, 0016f7e0), +0x30
// the side (input +0x27).
struct ActorJumpZoneMessage {
    std::uint8_t kind{6};
    std::uint32_t target_2c{};
    std::uint32_t side_30{};
};
// Actor fields read by 00192800.
struct ActorJumpZoneActor {
    bool byte_14{};                      // actor +14 (local authority)
    bool component_is_player{};          // +58 virtual +54
    std::uint32_t flags_3204{};          // stance word; bit 1 = running
    std::int32_t mode_320c{};
    RootVector desired_31c0{};           // pad direction (+31c0)
    // 'flor' root (+3290): world orientation (00132400 -> 001323f0) and
    // position (root +0x10 with a parent, else +0x60; and 00132370).
    RootParentTransform flor;
};
// The target as 0040c540 resolves it: nullopt when the key does not resolve
// or the object's class word (+4c) is not the hotbox class 004ee6b8. Its
// virtual +0x88 (00173660) returns &box+0x70 {min, max}; world is the
// transform component's +0x10 matrix (rows), absent for an identity basis.
struct ActorJumpZoneTarget {
    std::array<float,3> minimum{},maximum{};
    std::optional<std::array<float,16>> world;
};
// The self-sent take-off (CAS action type 004e0140, constructor 0055d480,
// kind 4, event byte 1, transport 1: handled before 00192800 returns).
struct ActorRunningJumpTakeoff {
    std::int32_t action{};               // 0x15 (side 1) or 0x16 (side 2)
    RootVector payload{};                // +0x30: (T.xyz, 1.0) or (T.xyz, yaw)
    bool operator==(const ActorRunningJumpTakeoff&) const=default;
};
// 00192800. No send when: +14 clear with a player component; not running
// (+3204 bit 1 clear); mode 2, 4 or 5; kind != 6; no hotbox target.
// Segment: c = (min + max) * 0.5; d = max - min, or 0 when min.x > max.x;
// L = max.s(d.x, max.s(d.y, d.z)); p0 / p1 = c with the first axis whose
// extent equals L replaced by min / max; both through the transform rows.
// Heading: v = +31c0 rotated by the 'flor' quaternion (w lane 0, the
// 001a2b20 VU sequence), z = 0, xyz normalized with vrsqrt; n = (-v.y, v.x).
// a = (p1 - p0).n, b = (F - p0).n (xyz sums), negated when a < 0; requires
// 0 <= b <= a and a != 0; T = p0 + (p1 - p0) * (b / a); requires
// (T - F).v > 0. Side 1: payload (T.xyz, 1); side 2: e = (-(p1-p0).y,
// (p1-p0).x, 0, 0), 0 - e when e.v < 0, yaw = atan2f(-e.x, e.y) (00121ab8).
// Sides other than 1 and 2 throw (the original sends an uninitialized action).
std::optional<ActorRunningJumpTakeoff> request_actor_running_jump(const ActorJumpZoneMessage& message,
    const ActorJumpZoneActor& actor,const std::optional<ActorJumpZoneTarget>& target);

// Rotates point.xyz by the quaternion with the 001a2b20 VU sequence (the
// point's w lane taken as 0); the result keeps point.w.
RootVector rotate_actor_local_point(const RootVector& point,const RootVector& orientation);
}
