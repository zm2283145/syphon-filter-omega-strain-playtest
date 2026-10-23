#pragma once
// omega_play --loadout: the level-start equipment selection the sandbox's
// skipped loadout screen GuiEquipmentModify (002bcb20..002c1dc0) would make,
// applied through the screen's own Accept 002bcb50 (equipment_loadout.hpp).
// COMBAT.md "Checkpoint 120l loadout".
//
// The original's start: TORONTO1 init::Init calls game::SetPlayerWeapons(106,
// -1) (00185330: Baretta_92F in slot 0, equipped) on the inventory 00144dc0
// built with the Flashlight 103 in slot 7; the screen's preview actor starts
// as a copy of the player's attachments and Accept applies the edited copy
// with 00143b60 / 00142450 / 00143df0(inv, item, 1, 0, 1, 0) / 00142c50.
// A fresh profile's AWARDS SetLevels unlocks StunBaton 129 and Baretta_92F
// 106; the screen's lists are then hip {106}, melee {None, 129} and None only
// for back, leg and grenade.
#include "equipment_loadout.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// The screen's categories in attach-point order: back 2, hip 3, leg 5,
// grenade 8, melee 9 (a list offers the items whose class +30 is the point).
inline constexpr std::array<std::uint8_t,5> loadout_category_points{2,3,5,8,9};

struct EquipmentLoadoutSelection {
    std::array<std::int32_t,5> items{-1,-1,-1,-1,-1};   // per category; -1 None
    std::uint8_t hand_point{};                           // the screen's +fc; 0 none
    // Point 4, the dual-wield off-hand pistol's holster (002bd560's attach-table edit with the dual wield perk
    // 00332f90: p[4] = p[3]; 002bcb50 step 1: p[4] = p[1]): an item, -1 None, unset = the player's (Checkpoint
    // 132wp1).
    std::optional<std::int32_t> point_4;
    bool operator==(const EquipmentLoadoutSelection&) const=default;
};

// "default": each list's item entry for a fresh profile (hip 106, melee 129;
// back/leg/grenade offer only None) with the hip weapon in the hand.
EquipmentLoadoutSelection default_equipment_loadout();
// "default" or a comma list of back=/hip=/leg=/grenade=/melee=<item id or
// none> and hand=<category or none>; unnamed categories are None, the hand
// defaults to the hip. Throws on a malformed entry.
EquipmentLoadoutSelection parse_equipment_loadout(std::string_view spec);
// True when every selected item is one a fresh profile's lists offer; other
// items are the all-weapons list mode (*0x51ee38), a host test convenience.
bool fresh_profile_loadout(const EquipmentLoadoutSelection& selection);

// The screen's preview and Accept over the player's inventory. The weapon
// models (record +13c, resources 00145710 preload loads) are not hosted, so
// the attach tables use one stand-in model per item (+40a the item, +40c 0)
// on a copy of the database; the player's table is the inventory's slots at
// their hand (held) or holster points as 00143df0 / 00142bb0 place them. The
// preview is that table with the hand models at their holster points (the
// state Accept's step 1 produces) and the selection's categories replaced
// (and point 4 when the selection sets it).
// Throws when a selected item's class is not its category's point. The
// inventory events (with the stand-in attach events) are appended.
std::array<EquipmentAcceptStep,4> apply_equipment_loadout(WeaponInventory& inventory,const WeaponDatabase& database,
    const WeaponInventoryHost& host,const EquipmentLoadoutSelection& selection,std::vector<WeaponInventoryEvent>& events);
// The stand-in model handle of an item (attach events carry it).
std::int32_t loadout_stand_in_model(std::int32_t item);
bool loadout_stand_in(const WeaponInventoryEvent& event);

std::string describe_inventory(const WeaponInventory& inventory);

}
