#pragma once
// The loadout screen GuiEquipmentModify (002bcb20..002c1dc0; FRONTEND.md,
// local analysis agent_frontend_loadout.md): its Accept handler 002bcb50.
// The screen holds the player (+48), a preview actor (+4c) whose model
// attachment table the UI edits, and +fc, the attach point of the weapon
// shown in the hand. Accept applies the preview's table to the player's
// inventory with the original inventory operations (weapon_inventory.hpp).
// Only the solo path is native; the screen UI, the preview actor, the pad
// reset 0036a0f0, the screen pop 00414ac0 and the resume 0012eee0 are left to
// the caller (returned as steps).
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace omega {

inline constexpr std::size_t model_attach_point_count=10;
// An actor's model attachment table (actor +60 +2410, points 0..9): the
// attached model resource (a weapon record's +13c), 0 for none.
using ModelAttachTable=std::array<std::int32_t,model_attach_point_count>;

// The model object fields Accept and 0038c7d0 read.
struct AttachedModel {
    std::int32_t item_40a{};        // lh +40a: the weapon record it was loaded for
    std::uint8_t byte_40c{};        // 0038c7d0 attaches only below 8
};
using AttachedModelLookup=std::function<AttachedModel(std::int32_t model)>;

// The attach helpers applied to a table for inventory events: attach =
// 0038c7d0 (stores the model unless it is non-null with +40c >= 8), detach =
// 0038c7c0 (stores 0), move_attach = 0038c790 (swaps the two points). The
// pose refresh 0038c5e0 they tail-call is not modelled. Other events are
// ignored.
void apply_attach_events(ModelAttachTable& table,std::span<const WeaponInventoryEvent> events,
    const AttachedModelLookup& model);

struct EquipmentModifyScreen {
    ModelAttachTable preview{};     // +4c actor +60 +2410
    std::uint32_t hand_point_fc{};  // +fc (0: no hand weapon)
};

// The caller's part of the solo tail of 002bcb50, in order.
enum class EquipmentAcceptStep {
    clear_loadout_pending,  // sb 0 -> 0x4ffc29 (the level start waits on it)
    pad_update,             // 0036a0f0(0x55a300)
    pop_screen,             // 00414ac0(004147a0(), screen, 0)
    resume,                 // 0012eee0(0x4ffb50, 0, 0)
};

// 002bcb50 after its event test (type 1, +2 = 0, button +8 = 0x80, +6 = 0):
//  1. if preview point 0 holds a model and +fc != 0: preview[+fc] = preview[0],
//     preview[0] = 0, and then, if preview point 1 holds one, preview[4] =
//     preview[1], preview[1] = 0 (the hand models back to their holsters);
//  2. 00143b60: holster the player's weapon (refused while switching is not
//     allowed);
//  3. for points 2..9: when the player's model differs from the preview's,
//     remove (00142450(inv, slot, 0, 0, 0)) the slot the point holsters
//     (byte 00489d60[point], the class table read with the point) if the
//     player has a model there, then add (00143df0(inv, item, 1, 0, 1, 0))
//     the preview model's item (+40a) if the preview has one;
//  4. if +fc != 0 and preview[+fc] holds a model: equip (00142c50) the slot
//     of its item (00143d20).
// The player's table is kept in step with the inventory events (the host's
// attached() reads it). Throws for multiplayer (00414790 / 00418a80, not
// hosted) and for +fc >= 10 (the original reads past the table).
std::array<EquipmentAcceptStep,4> accept_equipment_modify(WeaponInventory& inventory,const WeaponDatabase& database,
    WeaponInventoryHost host,ModelAttachTable& player,EquipmentModifyScreen& screen,const AttachedModelLookup& model,
    std::vector<WeaponInventoryEvent>& events,bool multiplayer=false);

}
