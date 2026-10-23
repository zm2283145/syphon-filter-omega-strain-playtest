#pragma once
#include "animation_root.hpp"
#include "geometry.hpp"
#include <array>
#include <bit>

namespace omega {
// Program constants written by static initializer 004cc5e0 and read by 001ea240.
struct ActorGatherBoundsTuning {
    float upper_660{22.0F};   // 004a0408
    float upper_5c0{12.0F};   // 004a0400
    float upper_3d3_extra{5.0F}; // 004a0358, added to 8.0
};
// Physical fields read by 001ea240, in its read order.
struct ActorGatherBoundsInput {
    bool byte_660{},byte_5c0{},byte_3d3{},byte_480{};
    std::int32_t mode_20{};
    std::array<bool,9> slot_3e0{};   // bytes at +3e0 + i*0xa0, i = 0..8
    RootVector predicted_2f0{},position_300{};
    RootVector floor_root_position{}; // 003ae5e0("flor") -> 00132400 -> 00132370
};
// Human physical virtual +0c (001ea240): broad-phase box around the floor
// root, unioned with itself shifted by the predicted step. XY half-range
// 2 (3 if any slot is set) plus 1; Z from (-3.5 or -8) - 1 to upper + 1.
Bounds actor_gather_bounds(const ActorGatherBoundsInput&,const ActorGatherBoundsTuning& tuning={});
}
