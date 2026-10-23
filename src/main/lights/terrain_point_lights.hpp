#pragma once
// Terrain point lighting (research notes FLASHLIGHT.md section 18,
// LEVEL_LIGHTS.md section 12): the per-vertex light pass the terrain draw
// runs for type-4 point lights (the flashlight beam, weapon and effect point
// lights), in two halves.
//
// EE, per terrain instance (00394d48 -> 0037c350 -> 00373320 with mask =
// flags = 4 and no transform):
// - 00394d48: the reference is the VUM header bounds +0x30 / +0x40, so the
//   manager takes the bounds path (+0x40 = 0);
// - 00373320: centre = (min + max) * 0.5 (001832d0), radius = |max - centre|
//   (00183380, 00183350), 00373590 per table entry with (mask & type), the
//   qsort 001273c8 / 00373ba0;
// - 00373590: a type-2 entry is a candidate with key 0; any other is one when
//   d2 - (radius + range)^2 is negative (VU status S flag, cfc2 vi16 bit 1),
//   d2 = |entry position - centre|^2, with key d2 * weight;
// - 00372120 (flags bit 4, 00372548): the sorted candidates whose live type
//   matches the mask, at most six (+0x2130 / +0x2134);
// - 00371cf0: per listed light, A = position (+0x30) with w = the range
//   (vcall +0x34), B = 128 * colour (+0x20..+0x28) with w = inner (+0x10).
// The bounds path (001832d0, 00183380, 00183350, 00373590) is VU0 macro code
// and is modelled like the VU1 side below (overflow saturated to +-FLT_MAX:
// an instance whose header holds the empty bounds +FLT_MAX / -FLT_MAX gets
// centre 0 and radius sqrt(FLT_MAX), so every type-4 light in the table is a
// candidate); only 00371cf0's 128 * colour is COP1 (mul.s: binary32, round to
// nearest, denormal results flushed to zero, non-finite results throw).
//
// VU1, per batch (microcode set 0, 00477af0, the pass at 0x11f0..0x15b8):
// - batch bounds = the batch header qwords 0 / 1 (MeshBatch); centre =
//   0.5 * min + 0.5 * max (ACC form), radius = eleng(centre - max);
// - per light: skip the batch when (A.w + radius) - eleng(A.xyz - centre) has
//   its sign set; Q = 1 / (A.w - B.w);
// - per vertex: a = 1 - (eleng(A.xyz - p) - B.w) * Q; vertices with a <= 0
//   keep their colour; otherwise a is clamped to 1 and each of r, g, b becomes
//   ftoi0(min(itof0(c) + B * a, 255)). Lights apply in list order, each on
//   the previous light's truncated result.
// VU arithmetic is modelled as binary32 round to nearest with overflow
// saturated to +-FLT_MAX (the batches whose header holds the empty bounds
// +FLT_MAX / -FLT_MAX); the EFU's eleng is modelled as sqrt((x*x + y*y) + z*z)
// (its exact precision is not established).
#include "character_shadows.hpp"
#include "geometry.hpp"
#include "level_lights.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace omega {
// One record of the manager's light list (+0x2150 + 32 * k).
struct TerrainPointLight {
    ShadowVector a{};   // position xyz, w = range (vcall +0x34)
    ShadowVector b{};   // 128 * colour xyz, w = inner (+0x10)
};
// The terrain mask of 0037c350 (bit 2 of the shader-14 case, mask 6, is not
// hosted: see FLASHLIGHT.md section 18).
constexpr std::uint32_t terrain_point_light_mask=4U;
// 00373320 (bounds path) + 00372120 (flags 4) + 00371cf0 without a transform
// for one terrain instance with VUM header bounds (minimum, maximum). `table`
// is the frame's light table (rows[k].light names the live light). Throws for
// mask bits other than 4, a table whose rows and entries differ, non-finite
// values and a listed light whose range method is unknown.
std::vector<TerrainPointLight> terrain_point_lights(const LevelLights& lights,const LevelLightTable& table,
    const ShadowVector& minimum,const ShadowVector& maximum,std::uint32_t mask=terrain_point_light_mask);
// The VU1 batch test for one light: true when the light reaches the batch.
bool terrain_batch_reached(const TerrainPointLight& light,const MeshBatch& batch);
// The VU1 light pass over one batch: `colours` are the batch's vertex colour
// bytes (r, g, b), updated in place; `positions` its vertex positions (the
// VU's position + VU mem 12, MeshVertex::position). Throws when the spans'
// sizes differ from the batch's vertex count. A light whose range equals its
// inner distance divides by zero: Q = +-FLT_MAX (the EE interpreter's VU DIV),
// so it lights the vertices within the inner distance fully and no others.
void apply_terrain_point_lights(std::span<const TerrainPointLight> lights,const MeshBatch& batch,
    std::span<const std::array<float,3>> positions,std::span<std::array<std::uint8_t,3>> colours);
}
