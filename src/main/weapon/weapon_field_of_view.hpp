#pragma once
// The weapon field of view 00142f20 (weapon.cc) that the player's pad writes
// to the followed camera every frame (0x0024f968..0x0024fa08, COMBAT.md
// "Checkpoint 121c"):
//   a1 = (pad input +38 == 8), the aim view;
//   a2 = 1 when sp+fc8 (0055a49c, Triangle) is not 0.0, else -1 when sp+fcc
//        (0055a4a0) is not 0.0, else 0: the zoom direction;
//   f12 = camera +c64, f13 = the pad's elapsed time;
// then +c64 = 00192740(fov, 001f05d0(0.01), 001f05d0(179.99)) and 0024fc20
// writes +c68 = +c64.
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <cstdint>
#include <functional>

namespace omega {

struct WeaponFieldOfViewInput {
    bool aim{};                     // a1
    std::int32_t zoom{};            // a2: -1, 0 or 1
    float current_c64{};            // f12: the camera's +c64
    float elapsed{};                // f13
    bool byte_4ea140{};             // the global byte read at 0x001430bc (0 at boot)
    // The zoom sound (001431a8..0014329c): true = start, 0020f990(msg,
    // 004f2170 bank 1 "E_ZOOM", inventory +88 owner), +20 = 1, 003c9420(msg,
    // 004ffb50, 0); false = stop, 0020eba0(msg, 004f2170, +88) then the same
    // post. Unset, either path throws naming its address.
    std::function<void(bool start)> zoom_sound;
};

// 00143370(z, a): 2 atanf(tanf(a pi/180 / 2) / z) x 57.29578, in degrees.
float weapon_zoom_angle(float zoom,float angle);

// 00142f20(inv, aim, zoom, +c64, elapsed): updates the inventory's +e8
// (the zoom angle in degrees), +ec, +ed and +ee and returns the new field of
// view in radians. The zoom sound (inventory +f0): a moving zoom (s1) with
// +f0 == 0 starts it and sets +f0 = 1; no movement with +f0 != 0 stops it
// and clears +f0 (in.zoom_sound).
float weapon_field_of_view(WeaponInventory& inventory,const WeaponDatabase& database,const WeaponFieldOfViewInput& in);

// 0x0024f9c4..0x0024f9fc: the pad's clamp of 00142f20's result to
// [001f05d0(0.01), 001f05d0(179.99)] (00192740: max.s then min.s).
float pad_field_of_view_clamp(float fov);

}
