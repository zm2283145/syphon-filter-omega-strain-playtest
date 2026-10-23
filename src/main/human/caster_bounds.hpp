#pragma once
// Actor bounds +0x60/+0x70 (render object +0x0/+0x10, skeleton space) of a
// human character, as the shadow slots 003d4810 read them (CHARACTER_SHADOWS.md
// section 2a). Interpreter-verified against the original chain:
//   001af460 (human activation 001b4f40 / 001b53d0): every node's bound sphere
//            (node +0x150..+0x16f) reset by 001af720, then the entries of the
//            human table (vector 004ee9d8, built by 001afd50) set by 001af600;
//   003a9220 (world entry 00392aa0, context +0x14 set): context bounds +0x30/+0x40;
//   00389fa0 (world step 5) to 0038a118: copy to the render object, first
//            unioned with the slot-0 attachment sphere (0038a620) when render
//            +0x2410 holds an object.
#include "character_shadows.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace omega {
// One entry of the human bound-sphere table (static entries at 004eebb0 +
// 0x20*k, pushed in order by 001afd50): skeleton node and radius. Every
// centre is (0,0,0,1), so 001af600 sets mode +0x151 = 1 and 003a9220 uses
// the node matrix translation (node +0x40).
struct HumanBoundSphere { std::uint32_t node{}; float radius{}; };
std::span<const HumanBoundSphere> human_bound_spheres();
// The object in render slot 0 (render +0x2410; 0038c7d0 stores it, called
// from the weapon code at 00141d3c / 00143f70): node = render byte +0x2450,
// point = object +0x130 with W replaced by 1 (0019e660). Radius 0.3
// (0x3e99999a, 0038a034). When the point's x, y and z are all zero the node
// translation is used instead (the MAC zero test at 0038a000).
struct ActorAttachmentSphere { std::size_t node{}; std::array<float,3> point{}; };
struct ActorBounds { ShadowVector minimum{},maximum{}; };
// 001af460 then 003a9220 over the skeleton-space node matrices (column-major,
// node +0x10..+0x4f, collection order = skeleton file order). Throws when a
// table node lies outside the collection (001af330 does not check it).
ActorBounds human_context_bounds(std::span<const std::array<float,16>> nodes);
// 00389fa0 up to 0038a118: the context bounds, unioned with the slot-0
// attachment sphere when there is one. Throws for an attachment node
// outside the collection.
ActorBounds human_actor_bounds(std::span<const std::array<float,16>> nodes,const std::optional<ActorAttachmentSphere>& attachment);
// Checkpoint 132f8: the same chain over any bound-sphere table. A display
// actor that never ran the human table builder (the stats screen's figures:
// 0035d430 does not call 00290610) has every node disabled, so 003a9220
// leaves the reset bounds (FLT_MAX x3) / (-FLT_MAX x3) and only the
// attachment sphere, if any, sets them.
ActorBounds actor_context_bounds(std::span<const std::array<float,16>> nodes,std::span<const HumanBoundSphere> table);
ActorBounds actor_bounds(std::span<const std::array<float,16>> nodes,std::span<const HumanBoundSphere> table,
                         const std::optional<ActorAttachmentSphere>& attachment);
}
