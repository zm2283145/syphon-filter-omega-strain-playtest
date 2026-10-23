#pragma once
// The AI branch of the dead state 001a3780 (token 0x92 / 0x93 for an actor
// whose +58 controller is not the player): 001a3520(actor), the drop of the
// actor's weapons and armor into a backpack (a world inventory object,
// "backpack.cc"), interpreter-verified (tests/npc_dead_drop_goldens.hpp):
//   1. nothing when +3464 (the actor's backpack) is set;
//   2. the drop is considered when the weapon inventory +3524 holds any item
//      (001434d0: an item word other than -1 in the 8 slots), or the armor
//      +33a0 is nonzero, or the actor is present (+2d, +2c, !+2e) with
//      +3468 (SetPickupable), or the item list +3528 has an entry with a
//      nonzero count (004704f0; read only when the earlier tests fail);
//   3. and taken only when +14 is set: +3464 = 00257640(actor, 1, 0), then
//      00142320(inventory, backpack) (unless the inventory's +a8 is set: for
//      slots 7..0 holding an item whose record +a8 is nonzero, 00255af0(
//      backpack, item, spare, clip, 0) then 00142450(inventory, slot, 0, 0,
//      0), the native WeaponInventoryOps::remove without unloading), then
//      with armor 00255a50(backpack, armor, +339c). The multiplayer send
//      (005721c8 set) is not reproduced.
// The item list is only tested, not moved. The backpack object itself (its
// constructor 00257640 with 003cf0e0, vtable 004dc350, the contents
// 00255af0 / 00255a50, its update and pickup) is a host object.
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

struct NpcDeadDropView {
    std::uint64_t backpack_3464{};               // 0: none
    std::int32_t armor_33a0{},max_armor_339c{};
    std::array<std::uint8_t,3> root{};           // +2c, +2d, +2e
    std::uint8_t pickupable_3468{},byte_14{};
};
struct NpcDeadDropHost {
    std::function<bool()> items_3528;            // 004704f0(+3528)
    std::function<std::uint64_t()> create_backpack;                                   // 00257640(actor, 1, 0)
    std::function<void(std::uint64_t backpack,std::int32_t item,std::int32_t spare,std::int32_t clip)> add_weapon;   // 00255af0(.., 0)
    std::function<void(std::uint64_t backpack,std::int32_t armor,std::int32_t max_armor)> add_armor;                  // 00255a50
};
// Returns the new +3464 (nullopt: unchanged).
std::optional<std::uint64_t> npc_dead_inventory_drop(WeaponInventoryOps& inventory,const WeaponDatabase& database,
    const NpcDeadDropView& view,const NpcDeadDropHost& host);

}
