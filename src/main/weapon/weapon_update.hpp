#pragma once
// The per-frame weapon inventory update 00144080(inventory, dt), called by the
// actor update (001b1d6c) (COMBAT.md, Native status): burst continuation
// through the discharge, cooldown, the time since the last shot +b4, the
// muzzle smoke, the looping-sound stop 00144ba0, the spin-up, the player's
// carried-value message (00142e80) and the inventory object's update
// (00262d90), then the torch branch (hold class 10: the light and the flicker
// object, torch_light.hpp). A non-zero object state without the air taser
// host throws.
#include "torch_light.hpp"
#include "weapon_discharge.hpp"
#include <cstdint>
#include <functional>

namespace omega {

struct WeaponUpdateHost {
    // The burst target's weak handle (+90 object, +94 its +c) still resolves.
    std::function<bool(std::uint64_t object,std::int32_t id)> valid;
    // Muzzle smoke gates: actor +38 (0x80 after the draw pass) and the float at
    // 005383d8 (the smoke needs it above 35.0); both are host readings.
    std::uint32_t actor_word_38{0x80};
    float value_005383d8{};
    std::uint8_t life_state{};      // actor virtual +108 (read for hold class 10 only)
    // The torch branch's services; needed while a torch is lit or its light /
    // object exist (absent then: throws naming 00144330).
    const TorchLightHost* torch{};
    // 00262d90(inventory +0, dt) (air_taser_update), called while the object's
    // state +8 is set (state 0 does nothing). Empty: a set state throws.
    std::function<void(float dt)> air_taser;
};

// 00142e80: the largest record +f4 over slots 0..5 that hold an item and are
// flagged (+0) or current; 0 when none.
std::int32_t weapon_carried_value(const WeaponInventory& inventory,const WeaponDatabase& database);
// 00144ba0(inventory, hand): stops the looping fire sound of the hand.
void stop_weapon_loop_sound(WeaponInventory& inventory,const WeaponDatabase& database,std::uint8_t hand,
    std::vector<WeaponDischargeEvent>& events);
// 00144080. `discharge` is the host of the burst continuation shots (its
// player answer also selects the carried-value message).
void update_weapon_inventory(WeaponInventory& inventory,const WeaponDatabase& database,float dt,const WeaponUpdateHost& host,
    const WeaponDischargeHost& discharge,std::vector<WeaponDischargeEvent>& events);

}
