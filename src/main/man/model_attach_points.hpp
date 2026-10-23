#pragma once
// Model attach points (research/COMBAT.md, "C1d: render attach points"): what the
// hard-mesh setup 0038e100 (run at actor activation, 00392940) writes to the
// model's ten attach slots, node byte +2450 + slot and local matrix +2460 +
// 0x50 * slot (rows +2470, +2480, +2490, translation +2460), when the character
// has a hard mesh (a pair named "HARD_..." or slot 2 "?HARD...", found in the
// part-mesh cache). Interpreter-verified with the VIF0/VU0 model of the
// position-query program on synthetic hard meshes.
//
// Each chunk is skinned by the query program with the 21 inverse-bind matrices
// (node +d0) alone, so positions are bone-local bind coordinates. Its groups are
// walked as vertex triples (0038fea0 for every third vertex of a group, until
// the triples cover the chunk's vertex count): the first vertex's texture
// coordinates pick the slot (trunc(4u) + 4 trunc(4v), u and v clamped to [0, 1];
// slots 10 and above are skipped), the corner opposite the longest edge gives
// the translation (through the bone's scale matrix, node +110), and the two
// edges from that corner give the axes. Slot 7 is then a copy of slot 0.
// Without a hard mesh the original builds fixed default frames (0038e690..
// 0038fe44, node bytes 0x14, 0x13, 5, 0, 0, 5, 0xe, 5, 0, 0); that path is
// not ported and is not reachable through this API.
#include "skinned_mesh.hpp"
#include "weapon_shot_frame.hpp"
#include <array>
#include <optional>
#include <span>

namespace omega {

using ModelAttachSlots=std::array<std::optional<WeaponAttachPoint>,10>;
// 0038e100's hard-mesh path. `slots` holds the model's slots before the call
// (nullopt: unknown contents) and is updated in place. `bone_scales` are the
// nodes' +110 matrices; nullopt when the skeleton context has no node
// collection (byte +14 clear), which uses the identity (0014b210). A triple
// reaching past its chunk, or groups that end before the triples cover the
// chunk, throw (the original reads stale scratchpad / VU0 memory there).
void hard_mesh_attach_points(const SkinnedMesh& hard_mesh,std::span<const ShotFrame> inverse_bind,
    std::optional<std::span<const ShotFrame>> bone_scales,ModelAttachSlots& slots);

}
