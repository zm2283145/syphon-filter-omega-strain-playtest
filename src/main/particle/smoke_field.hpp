#pragma once
// Checkpoint 132cp9: the smoke field of game::SmokeToronto3 (002326f0 ->
// 002329d0 -> 00232bf0 -> 00232c00(0x15, -1, 20.0, 0, 40.0, 0.4)): a 0xc0
// object (vtable 004dbc70, derived from the particle asset 004df860 with
// +60 = -1 and no instance +64) that keeps particle instances of an effect
// table entry (00236f20) on a disc around the camera object and a ring of
// them ahead of its motion. research/EFFECTS.md "Checkpoint 132cp9 smoke
// field".
//   ctor   00235df0(obj, type, f12 = inner, f13 = fade, f14 = ring, f15 = strength)
//   logic  vt+48 00234c40(obj, f12 = dt)
//   draw   vt+58 00235a00(obj): 003a1ad0(instance, +38, identity at the node)
//   dtor   vt+8 00235ae0
#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>

namespace omega::particle {

using SmokeVector = std::array<float, 4>;
using SmokeMatrix = std::array<SmokeVector, 4>;

// One 0x20 list value: +0 the instance (0036b180(0x70) + 003a2dd0), +10 the
// position (the identity frame's row 3 for the update and the draw).
struct SmokeNode {
    std::uint64_t instance = 0;
    SmokeVector position{};
};

struct SmokeField {
    std::int8_t type = 0;      // +80 (the effect table index)
    float inner = 0.0f;        // +a8 f12: the ring's spacing and the hole test
    float fade = 0.0f;         // +ac f12 + f13: full strength inside
    float outer = 0.0f;        // +b0 f14 + f12: the disc radius R
    float strength = 0.0f;     // +b4 f15
    float step = 0.0f;         // +a4 2 asinf(f12 / (2 f14)): the ring's angle step
    std::int32_t ring = 0;     // +a0 cvt.w(0.5 + (pi / 2) / +a4): ring points per update
    // +90: the camera point of the last update. 0036b180 does not clear and
    // the constructor never writes it, so the first update reads the heap's
    // old bytes; the port starts from zero (research/EFFECTS.md).
    SmokeVector previous{};
    std::list<SmokeNode> nodes;  // +74 (00139070 / 00138c60; push_back 002359a0 -> 00138ea0)
    std::uint32_t flags_38 = 0x80;  // +38: the scene walk 0013e250 sets 2 (vt+88 returns null: no cull 003c8990)
};

// 00235df0. The base 003cf0e0 (key 0040c240(0xe7), kind +49 = 4, +2c = 1),
// +60 = -1, +64 = 0, +70 = 1.0, +2f = 1 are the host's.
SmokeField make_smoke_field(std::int8_t type, float f12, float f13, float f14, float f15);

struct SmokeFieldHost {
    // 00236f20(+80) inline: the effect table entry's id (+0), or nullopt for
    // no entry (flag byte +4 clear or index >= 0x29).
    std::function<std::optional<std::int32_t>(std::int8_t)> effect_id;
    // *(00159560(004ffc14)) vt+f4 (the player's 'flor' root world matrix,
    // 00132280); nullopt when the camera object is null.
    std::function<std::optional<SmokeMatrix>()> camera;
    std::function<std::uint32_t()> random;  // 00127dd0
    // 0036b180(0x70) then 003a2dd0(mem, id, &identity with row 3 = position, 1.0).
    std::function<std::uint64_t(std::int32_t id, const SmokeMatrix& matrix)> create;
    // 003a2ba0(instance, -1) then 0036b130.
    std::function<void(std::uint64_t)> destroy;
    // The instance's +5c.
    std::function<void(std::uint64_t, float)> set_alpha;
    // 003a2400(instance, &identity with row 3 = node position, 0, f12 = dt).
    std::function<void(std::uint64_t, const SmokeMatrix& matrix, float dt)> update;
};

// vt+48 00234c40.
void update_smoke_field(SmokeField& field, float dt, const SmokeFieldHost& host);

// vt+58 00235a00: each node with an instance, in list order.
void draw_smoke_field(const SmokeField& field,
                      const std::function<void(std::uint64_t instance, std::uint32_t flags, const SmokeMatrix& matrix)>& draw);

}  // namespace omega::particle
