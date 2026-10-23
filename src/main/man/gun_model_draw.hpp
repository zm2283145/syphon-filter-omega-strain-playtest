#pragma once
// The attached weapon models of a man model's draw (Checkpoint 131, COMBAT.md "Checkpoint 131"):
// the attach-point section of 003870c0 (00388b08..: the eleven GUN matrices of each attach point
// 0..7, the flare and stun rotations), the part loop 003855d0 (which parts of the model are
// drawn, and their LOD records) and the flare counters actor +2df0 / +2df1 (model +2d90 /
// +2d91: set to 1 by the discharge's flash, aged by 0038a228). The VU1 programs are not run:
// a part is skinned on the CPU (skin_skinned_mesh) with the eleven matrices as its palette.
#include "gun_model_load.hpp"
#include "skinned_mesh.hpp"
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace omega {

// Model +2d90 / +2d91 (actor +2df0 / +2df1): the hands' flare counters.
struct GunFlareCounters {
    std::array<std::uint8_t,2> hand{};
    // 0013ff40's flash (WeaponDischargeEvent::Kind::flash, value = hand): the byte = 1.
    void fire(std::uint8_t hand_index);
    // 0038a228 (in the morph host 00389fa0, after the actor's logic): a nonzero counter is
    // incremented and cleared at 9.
    void age();
    bool operator==(const GunFlareCounters&) const=default;
};

using GunSlotMatrices=std::array<ShotFrame,11>;

// 003870c0 for one attach point s (0..7) whose model is drawn: `point_world` is the point's
// matrix (0019f350 of the local rows +2470 / +2480 / +2490 / +2460+0x50s through the node's
// world matrix, as shot_frame_node_world gives it), matrix 0. Matrix k (1..10) is a copy of
// its parent's (word +15c+32k; a parent not below k throws: the original would read the last
// draw's matrix) with matrix 0's rows times the bone offset (+150+32k, xyz; vmulax / vmadday /
// vmaddz) added to its origin. Then matrix 3 is turned by the flare rotation (s 0 with +2d90 or
// s 1 with +2d91 at 1 or 2; angle 72 * ((frame >> 1) & 7) degrees about x) and by the stun
// rotation (model +415; 81 * ((frame >> 1) & 31) degrees about y). `frame` is the counter the
// angle reads (the world's frame counter).
GunSlotMatrices gun_slot_matrices(const GunModel& model,const ShotFrame& point_world,std::uint8_t point,
    const GunFlareCounters& counters,std::uint32_t frame);

// 003836b0(model, a1, a2, 1) as the HUD weapon preview 002593b0 calls it (a2 the identity, so
// the pushed top is unchanged): the same eleven matrices from an identity point world, no flare
// rotation (only the stun rotation, +415), and LOD 0.
GunSlotMatrices gun_preview_matrices(const GunModel& model,std::uint32_t frame);
// 003836b0's parts loop (003848e0..): p below +40c other than the sbytes +410, +411, +412 and
// +414, whose LOD 0 record word +2c0 + 16p is non-zero; texture ids +3c0 + 2p.
std::vector<std::size_t> gun_preview_parts(const GunModel& model);

// 003870c0's skip tests for an attach point (00388b24..00388b34): the point's hidden byte +2438 + s
// (actor +2498 + s, written by SetHardpointVis's 00142e40) != 0, then model +417 != 0 (not loaded).
bool gun_model_drawn(const GunModel& model,std::uint8_t hidden_2438=0);

// 003855d0(model, ..., lod, a3): the parts drawn at point s, in order: below +40c, other than
// +412 (the light), +410 (the holster, except at points 3 and 4), +414 at point 0 / +413
// elsewhere (the other body), and +411 (the flare) unless a3 is 0 and the stun flag +415 is
// set or point 0 / 1 has its counter at 1 or 2; a part whose LOD record word (+2c0 / +340 +
// 16p) is 0 is skipped. The flare has two send blocks (00385850 for point 0 / +2d90 and
// 003858d8 for point 1 / +2d91); the stun flag takes both, so the flare is listed twice.
// point_byte is the render object's byte +2442 + s (actor +24a2 + s, written by 00142e00 for
// the slot-6 IR switch and the slot-7 flashlight; 00385734..00385798): when it is not 0 the
// light +412 is drawn, and at point 6 the other body is +414 instead of +413.
std::vector<std::size_t> gun_drawn_parts(const GunModel& model,std::uint8_t point,const GunFlareCounters& counters,
    std::int32_t lod,bool a3=false,std::uint8_t point_byte=0);

// One part's LOD record (+2c0 / +340 + 16p: +0 the chunk start, +7 the chunk count, +8 the
// descriptors) decoded from the HOG, and its texture id (+3c0 / +3d0 + 2p).
struct GunPartMesh {
    SkinnedMesh mesh;
    std::int16_t texture{};
};
GunPartMesh gun_part_mesh(const GunModel& model,std::span<const std::uint8_t> hog,std::size_t part,std::int32_t lod);

// The CPU skinning admits the part when every chunk is a single-group entry 8 / 10 chunk or a
// flare chunk (entries 12 / 14: the first record's matrix only, normals unread, colour 0x7f).
bool gun_part_skinnable(const SkinnedMesh& mesh);
// Every chunk runs a flare program (12 / 14): the part is drawn unlit with vertex colour 0x7f.
bool gun_part_flare_program(const SkinnedMesh& mesh);

// The part's vertices placed by the eleven matrices (palette entry k = matrix k).
SkinnedMeshGeometry gun_part_geometry(const SkinnedMesh& mesh,const GunSlotMatrices& matrices);
// The same into caller-kept geometry (cleared first, storage reused; checkpoint 132win11, port-side speed only).
void gun_part_geometry(const SkinnedMesh& mesh,const GunSlotMatrices& matrices,SkinnedMeshGeometry& output);

// The holster draw of a hand's model (Checkpoint 131c). 003870c0's pre-step (003883d8.. for
// hand 0, 00388770..00388b08 for hand 1): when the hand's model (+2410 / +2414) has a holster
// part (+410 != -1), scratchpad slot 3 + hand's matrix 0 is holster point 3 + hand's world
// matrix (the local rows +2550 / +25a0 through node +2453 / +2454, as the attach points are)
// with its origin moved by its rows times the hand model's +150 (xyz; vmulax / vmadday /
// vmaddz, vadd.xyz). The slot section that follows overwrites that matrix with the point's own
// matrix 0 (its world matrix) when point 3 + hand draws a model.
ShotFrame gun_holster_matrix(const GunModel& hand_model,const ShotFrame& holster_point_world);
// 003855d0's tail (0038598c..00385bc8): after the attach points, each hand's model whose +410
// is not -1 sends its part +410 again at LOD `lod`, with only matrix 0 unpacked (scratchpad slot
// 3 + hand) and slot `hand`'s texture entry (0038af40 writes it from +3c0 / +3d0 when that slot's
// model is drawn). Neither +40c nor the record word is tested: nullopt for +410 = -1 and for an
// empty record (word and QWC 0: the REF sends nothing); a part outside +40c, or a record word 0
// with a QWC, throws.
std::optional<std::size_t> gun_holster_part(const GunModel& hand_model,std::int32_t lod);
// The part's skinning reads palette entry 0 only (the tail unpacks no other matrix; the VU's
// others are the last slot's).
bool gun_part_matrix0_only(const SkinnedMesh& mesh);

// Checkpoint 132cp7: the thermal texture entries of the attachments (0038a918, D_0053B358 != 0). For
// every attach slot s with a model (+2410 + 4s; no +417 / +2438 test there), part k below +40c gets
// the entry 0xb50 + 0x40 s + 8 k = texman[D_0053B368] (FLA, class 4) when k is the model's sbyte +411
// (0038ad10), else texman[D_0053B362] (COO, class 1). The holster tail reads slot hand's entry of
// part +410 the same way.
std::uint8_t gun_thermal_class(const GunModel& model,std::size_t part);
// 0038ad7c..0038ada4: with +2c12 != 0 the shell entry 0x700024d0 (copied to sp+c0 at 003879e8, the
// shell draw's TEX0 at 0x70000770) is texman[D_0053B366] (HOT, class 3).
inline constexpr std::uint8_t gun_shell_thermal_class=3;
}
