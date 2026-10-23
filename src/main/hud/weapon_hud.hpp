#pragma once
// HUD values (COMBAT.md C4): the health and armor meters the pad producer
// 0024b5d0 updates every frame (0024cec8..0024ceec -> 002441e0 / 00244180)
// and the ammo text 00242190. Only the values are computed; the element
// setters (003f44e0, text 003e7ec0) and drawing are the host's.
#include "actor_damage.hpp"
#include "weapon_inventory.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace omega {

// 002441e0 (health element hud +6c) / 00244180 (armor element hud +70):
// nothing without the element, else 45 x (float(max) - float(current)) /
// float(max) (45.0 = 0x42340000). A zero maximum divides by zero (the EE
// gives FLT_MAX); here it throws.
std::optional<float> hud_meter_value(bool element,std::int32_t current,std::int32_t maximum);
struct HudMeters {
    std::optional<float> health,armor;
    bool operator==(const HudMeters&) const=default;
};
// 0024cec8..0024ceec: health = trunc(+40) against +3398, armor +33a0 against +339c.
HudMeters hud_meters(const ActorHealthState& health,bool health_element,bool armor_element);

// 00242190(hud, actor). Inputs from the HUD object: +6a8 (text hidden) and
// the HUD state object at +6a4: its +d4 (1 hides the count) and +d8 (the icon
// with nothing held).
struct HudAmmoInput {
    bool hidden_6a8{};
    std::int32_t state_d4{};
    std::int32_t empty_icon_d8{};
};
struct HudAmmoText {
    std::string text;                 // 003e7ec0(hud +69c, text)
    std::int32_t icon_694{};          // record +11c (hold class 7) or +13c, or the empty icon
    std::uint8_t byte_698{7};
    bool operator==(const HudAmmoText&) const=default;
};
// Nothing held: empty text. Otherwise, unless hidden: hold class 1 or 7 ->
// "%d" of the spare rounds; a weapon with a clip (00143710: record +70 != 0)
// -> "%d/%d" clip/spare (slot 0 adds slot 1); else empty.
HudAmmoText hud_ammo_text(const WeaponInventory& inventory,const WeaponDatabase& database,const HudAmmoInput& input);

}
