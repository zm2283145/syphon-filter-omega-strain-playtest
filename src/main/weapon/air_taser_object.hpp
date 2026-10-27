#pragma once
// The air taser object (00266630, 0xd0 bytes): the inventory +0 of roles 1
// and 3 (weapon_inventory.hpp) and the state of the native air taser
// (air_taser.hpp). The list it joins (004f8350) is drawn by 002629c0 ->
// 00262a20 (play_main.cpp, Checkpoint 132fx3).
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {

using AirTaserMatrix=std::array<RootVector,4>;          // rows 0..3, row 3 the translation

// One 64-byte wire segment (the vector at +10: +14 count, +18 data).
struct AirTaserSegment {
    RootVector pos{};                    // +0 world
    RootVector local{};                  // +10 in the controller frame
    RootVector vel{};                    // +20 (x, z used)
    // +30..+3c: the vertex colour (RGBA, 1.0 = 0x80) 00262a20 draws the wire point with (formerly read as widths;
    // segment() sets (w, w, 1, 1), 00264370 rewrites r and g: a blue-white wire).
    float colour_30{},colour_34{},colour_38{},colour_3c{};
    bool operator==(const AirTaserSegment&) const=default;
};
// One 0x60-byte spark node of the list at +20 (payload +10..+5f).
struct AirTaserSpark {
    std::uint64_t effect{};              // +10 (0 when its allocation failed)
    float time{};                        // +14 the wire position (segment index + fraction)
    AirTaserMatrix matrix{};             // +20
    bool operator==(const AirTaserSpark&) const=default;
};
struct AirTaserObject {
    std::optional<std::int32_t> record_0;   // the item ID of the held record
    std::uint64_t owner_4{};
    std::uint8_t state_8{};                 // 0 idle, 1 flight, 2 shock, 3 reel
    std::optional<std::size_t> light_c;     // index into the point pool
    std::vector<AirTaserSegment> wire_10;
    std::vector<AirTaserSpark> sparks_20;
    std::uint64_t victim_2c{};
    std::int32_t victim_id_30{-2};          // the ctor's -2; -1 once attached to nothing
    bool no_target_34{};
    std::int32_t subtarget_38{};
    std::int32_t bone_3c{};
    RootVector hit_40{};
    RootVector angles_50{};
    AirTaserMatrix frame_60{};              // row 3 (+90): the wire tip / the hand
    RootVector aim_a0{};
    float travelled_b0{},shock_b4{},spark_b8{};
    bool lethal_bc{},knockout_bd{};
    std::uint64_t particle_c0{};
    float wobble_c4{},wobble_c8{},wobble_cc{};
    bool operator==(const AirTaserObject&) const=default;
};

}
