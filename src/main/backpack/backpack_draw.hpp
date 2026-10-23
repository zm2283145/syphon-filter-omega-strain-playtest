#pragma once
// The dropped backpack's draw (Checkpoint 132o, research/LEVEL_OBJECTS.md), native and
// interpreter-verified (tests/backpack_draw_goldens.hpp, tools/ee/gen_backpack_draw.py):
//   the scene frame files a present backpack (+49 = 3) into list 8150 with the gearpack box
//   00253fb0 and its transform's matrix (0013e250); the walk calls its virtual +58, the gate
//   00253fe0: drawn only when its region +2f8 is active, its owner +300 (a valid one: +304 =
//   the owner's +c) is not, and it holds armor or a weapon entry. The gate writes the shared
//   gearpack model's (*004ffd30 + b8, the weapon library's table model 0) fade byte +416 from
//   the age +314, refreshes the light reference 00535d40 (virtual +e0 = 003cd420: the
//   transform's row 3) and calls 003836b0(gear, +38, transform +10, 0).
//   003836b0: skipped when the model is not loaded (+417); the vertex alpha (+416 + 1, or
//   0x80); 0037c350's flags (bit 31: not drawn); the LOD from the view-space distance of the
//   matrix (trunc(|t| / 22), 2 and above -> 1); the eleven matrices: identity, each a copy
//   of its parent's with the bone offset (+150 + 32k) added to its origin, matrix 3 turned by
//   the stun rotation (+415), then each through the object's matrix; the parts below +40c
//   other than +410, +411, +412 and +414 whose LOD record word is not 0.
// The VU1 programs are not run: the parts are skinned on the CPU (gun_part_geometry) in world
// space; 0038b120 (the object matrix through the camera stack for the VU) and the light
// matrix rows (00537dd0) are the renderer's. The alpha register writes {1, 5, a, 3}
// (005382c8) and the vertex alpha are returned; play applies them as the renderer's GS state
// (TEST ATE GEQUAL AREF a, AFAIL RGB_ONLY; ABE with ALPHA 0x44), as for the man draw.
#include "backpack_object.hpp"
#include "gun_model_draw.hpp"
#include "weapon_backpack.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {

inline constexpr std::uint32_t backpack_draw_gate=0x00253fe0;
inline constexpr std::uint32_t gear_model_draw=0x003836b0;
inline constexpr std::uint32_t gear_draw_lod_distance=0x41b00000;   // 22.0

// What 00253fe0 reads besides the backpack: the root bytes (+2c, +2d, +2e) of the region
// object +2f8 (needed when +2f8 is not 0) and the owner +300's +c and root bytes (needed
// when +300 is not 0). Missing when needed: throws.
struct BackpackDrawView {
    std::optional<std::array<std::uint8_t,3>> region_root;
    std::optional<WeaponBackpackOwner> owner;
};

// 00253fe0's call of 003836b0 and its side effects.
struct BackpackDrawCall {
    std::int8_t fade_416{};          // the byte stored at the gear model's +416 (-1: +314 <= 0)
    RootVector light_reference{};    // 003cd420: the transform's +40 (00535d40 when it differs)
    ShotFrame matrix{};              // a2: the transform's +10 .. +4f
    std::uint32_t code{};            // a1: the backpack's +38 (the scene frame's cull code)
};

// 00253fe0(bp). `code_38` is the backpack's +38 (0013e250's 003c8990 code). The transform's
// dirty byte +50 is cleared (its virtual +58 = 00132900). nullopt: the gate returns early.
std::optional<BackpackDrawCall> backpack_draw_gate_00253fe0(const WeaponBackpack& bp,BackpackTransform& transform,
    std::uint32_t code_38,const BackpackDrawView& view);

// 003836b0(model, code, matrix, 0).
struct GearDraw {
    std::uint8_t vertex_alpha{0x80};             // fp: +416 + 1 when +416 >= 0, else 0x80 (the packet's 0x47000000 + fp)
    std::array<std::int32_t,4> alpha_5382c8{};   // {1, 5, a, 3}: a = +416 when >= 0, else 0x80
    std::uint32_t flags{};                       // 0037c350(renderer, matrix, code, 0x480000), mode word 0
    std::int32_t lod{};
    GunSlotMatrices matrices{};                  // the eleven matrices in world space
    std::vector<std::size_t> parts;              // drawn in order
};
// `fade_416`: the model's +416 as the gate left it. `view_translation`: row 3 of the matrix
// through the camera stack's top (the view-space position the LOD measures). `frame`: the
// counter 005383c8 the stun angle reads. nullopt: +417 set or flags bit 31 (code 0x80).
std::optional<GearDraw> gear_draw_003836b0(const GunModel& model,std::int8_t fade_416,std::uint32_t code,
    const ShotFrame& matrix,const RootVector& view_translation,std::uint32_t frame);

// 003836b0's LOD: trunc(sqrt((x*x + y*y) + z*z) / 22.0), 2 and above -> 1.
std::int32_t gear_draw_lod(const RootVector& view_translation);

}
