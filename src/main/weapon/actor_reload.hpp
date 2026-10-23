#pragma once
// The reload (COMBAT.md "Reload"): actor message 001a92a0 action 0x3c
// (001aa048..001aa2e8) and the inventory's reload start 0013fb90 with the
// ammunition transfer 0013fd20. Interpreter-verified
// (tools/ee/gen_reload_action.py, tools/ee/gen_weapon_reload.py).
#include "secondary_component.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// 0049e4d0, the motion every reloadable item kind (1..6) requests.
inline constexpr char actor_reload_motion[]="DATA_ID_RifleChangeMagazine_Right";

struct ActorReloadInput {
    std::int32_t mode_320c{};         // actor +320c
    std::uint32_t flags_3210{};       // actor +3210
    bool secondary_3314{};            // actor +3314 is non-null
    std::uint64_t actor{};            // the event's receiver +2c
};
struct ActorReloadHost {
    std::function<bool()> reload_allowed;     // 00143560(+3524)
    std::function<std::uint8_t()> item_kind;  // 00143510(+3524)
    // 001cae20(+3314, name, kind, &event): the event built by 003c9e30(ev,
    // 00542b60), vtable 004da930, code +24 = 1, action +28 = 0, receiver +2c
    // = the actor, +30 = 0, +34 = 0.
    std::function<bool(const std::array<std::uint8_t,48>&,std::uint8_t,const SecondaryOneShot&)> one_shot;
    // Accepted: +3524 +a0 = 1, then 0013fb90(+3524).
    std::function<void()> start;
};
struct ActorReloadResult {
    bool refused{};      // the mode / +3210 gate (001aa048..001aa0bc)
    bool allowed{};      // 00143560 answered true
    bool requested{};    // 001cae20 was called
    bool accepted{};     // and accepted the one-shot
};
// 001aa048: refused in modes 0x14 / 0x1a / 0x1b / 0x1f and with +3210 bits
// 4, 5, 7, 8 or 16; then 00143560; item kinds 1..6 (00143510, asked once per
// tested pair as in the original) name the motion, other kinds request
// nothing; nothing either without +3314.
ActorReloadResult handle_actor_reload(const ActorReloadInput& in,const ActorReloadHost& host);

struct WeaponReloadEvent {
    enum class Kind : std::uint8_t {
        // 0013fdb8: actor +14 set: 003fdb50(record +80) and 003fdb50(0049b1c0
        // "TransferAmmo") formatted with 0049b1b8 "%s %s" (00128bd0) and shown
        // by 00242b00(*004ffc2c); the slot is the source of the transfer.
        transfer_notice,
        // 0013fc70: 001ca210(desc, 1, record +20, 0, 0), 0020f990(msg, desc,
        // +88), +20 = 1, to 004ffb50 with transport 0.
        sound,
    } kind{};
    std::int32_t item{};       // the record's item
    std::uint8_t slot{};       // transfer_notice: the source slot
    std::string name;          // sound: record +20
    bool operator==(const WeaponReloadEvent&) const=default;
};
// 0013fb90(inv): with a held slot (+84 != 6) holding an item: need = clip size
// (+70) - clip; spare < need: 0013fd20(inv, need - spare, record +10c) takes
// rounds from slots 0..5 other than the held one holding an item of the same
// ammo type (+10c), hold class +f0 0 and spare > 0, and the clip gets spare +
// the transfer; otherwise it gets need; spare loses what it gave unless +99;
// with slot 0 held and slot 1 occupied (+18 != -1) slot 1's clip and spare
// mirror slot 0's; then the reload sound. `notice_actor` is +88 non-null with
// its byte +14 set.
void reload_weapon(WeaponInventory& inv,const WeaponDatabase& db,bool notice_actor,std::vector<WeaponReloadEvent>& events);

}
