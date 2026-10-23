#include "weapon_hud.hpp"
#include "actor_fire.hpp"
#include "water_effects.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {

std::optional<float> hud_meter_value(bool element,std::int32_t current,std::int32_t maximum){
    if(!element)return std::nullopt;
    if(maximum==0)throw std::runtime_error("HUD meter: zero maximum (division by zero)");
    const float m=game_int_to_float(maximum);
    return 45.0F*((m-game_int_to_float(current))/m);
}

HudMeters hud_meters(const ActorHealthState& h,bool health_element,bool armor_element){
    if(!std::isfinite(h.health_40)||h.health_40>=2147483648.0F||h.health_40< -2147483648.0F)
        throw std::runtime_error("HUD meter: health outside the integer range");
    HudMeters m;
    m.health=hud_meter_value(health_element,static_cast<std::int32_t>(h.health_40),h.max_health_3398);
    m.armor=hud_meter_value(armor_element,h.armor_33a0,h.max_armor_339c);
    return m;
}

HudAmmoText hud_ammo_text(const WeaponInventory& inv,const WeaponDatabase& db,const HudAmmoInput& in){
    HudAmmoText out;
    const std::int32_t item=inv.current_84==weapon_no_slot?-1:inv.slots.at(inv.current_84).item;
    if(item==-1){out.icon_694=in.empty_icon_d8;return out;}
    if(!in.hidden_6a8&&in.state_d4!=1){
        std::vector<WeaponInventoryEvent> none;const WeaponInventoryHost host;
        WeaponInventory copy=inv;
        WeaponInventoryOps ops(copy,db,host,none);
        const auto cls=static_cast<std::uint32_t>(weapon_held_class(inv,db))&0xffU;
        if(cls==1||cls==7)out.text=std::to_string(ops.spare(weapon_no_slot));
        else if(db.record(item).clip_70()!=0)
            out.text=std::to_string(ops.clip(weapon_no_slot))+"/"+std::to_string(ops.spare(weapon_no_slot));
    }
    const auto& rec=db.record(item);
    out.icon_694=rec.hold_class_f0()==7?rec.hud_icon_11c():rec.model_13c();
    return out;
}

}
