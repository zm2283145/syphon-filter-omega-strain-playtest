#pragma once
// man.cc ejected shells (Checkpoint 131c, COMBAT.md "Checkpoint 131c"): the man model's sixteen
// shell slots (+2780.. of the model, actor +60), spawned by 00384a70 after each hand's flash
// (0x1409c8), moved by 0038a3a8 and drawn by 00389604 / 00385c00. The hand history 0038a278
// keeps each hand's node origin of this and the previous update; half their difference is the
// shell's inherited velocity. The shell model is the 00shells.skl table model (db +cc, table
// entry 5): 00147880 gave the gun its part record (+404 the chunk start, +408 the QWC).
#include "gun_model_load.hpp"
#include "weapon_shot_frame.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace omega {

// One slot, 0x40 bytes at model +27c0 + 0x40 i.
struct ShellSlot {
    RootVector position{};          // +27c0 (w carried)
    RootVector rotation{};          // +27d0 (quaternion)
    RootVector velocity{};          // +27e0 (model-local: turned by the rotation each update)
    float fall_27f0{};              // +27f0: the fall speed, grows by 0.02 per 1/30 s
    float floor_27f4{};             // +27f4: the root's z less 4
    std::uint32_t chunk_27f8{};     // +27f8: gun +404 (the shells model's part chunk start)
    std::uint16_t qwords_27fc{};    // +27fc: gun +408 (the DMA REF QWC)
    std::uint16_t age_27fe{};       // +27fe: 0 free, else updates since the spawn (from 1)
    bool operator==(const ShellSlot&) const=default;
};

struct ShellEjectState {
    std::array<RootVector,2> previous_2780{};   // +2780 / +2790
    std::array<RootVector,2> current_27a0{};    // +27a0 / +27b0
    std::array<ShellSlot,16> slots{};
    std::int8_t live_2c12{};                    // +2c12: the live slots after the last update (sb)
    bool operator==(const ShellEjectState&) const=default;
};

// 00384a70(model, hand): nothing when the hand's gun (+2410 + 4 hand, a host check) has no shell
// record (+404 = 0); returns false then, without drawing 00127dd0. Otherwise the slot is the
// first free one, else the oldest (the first of the largest age; slot 0 when none is older than
// 0). `attach_world` is 00385420's matrix of attach point `hand` (the port's attach_point_world)
// and `root_world` the model's node 0 world matrix (0019f350 through the component, or the
// component alone without a node collection). 00384d40 places the shell: rotation = the
// quaternion (the 001cb470 block) of the attach matrix turned by gun +220's rotation matrix,
// position = the attach matrix applied to (gun +170 + gun +230) with w = 1. Then one 00127dd0
// draw r: velocity = (0, -s, s, 0) + (current - previous) * 0.5 with s = 0.1 + 0.002 * (r & 15).
bool shell_eject_spawn(ShellEjectState& state,const GunModel& gun,std::size_t hand,const ShotFrame& attach_world,
    const ShotFrame& root_world,const std::function<std::uint32_t()>& random);
// 0038a278 for a hand whose +2410 model is set: previous = current, current = row 3 of the
// world matrix of the hand's attach node (byte +2450 + hand).
void shell_eject_history(ShellEjectState& state,std::size_t hand,const ShotFrame& node_world);
// 0038a3a8(dt) (in 00389fa0, after the actor's logic; dt is the actor update's): for each live
// slot the velocity is damped by 1 - 0.05 * dt / (1/30), turned by the slot's rotation and
// added to the position; the z falls by +27f0, which then grows by 0.02 * dt / (1/30). The age
// counts up; a slot is freed at age 60001 or when its z is below the floor. +2c12 = the count.
void shell_eject_update(ShellEjectState& state,float dt);
// 00389604's matrix of a live slot: the rotation's rows (w 0) and the position.
ShotFrame shell_eject_world(const ShellSlot& slot);
// 00389604 / 00385c00: the slots 00385c00 sends (the first +2c12 live slots in slot order;
// none when +2c12 <= 0). A count above the live slots throws (the original reads stale
// scratchpad entries).
std::vector<std::size_t> shell_eject_drawn(const ShellEjectState& state);

}
