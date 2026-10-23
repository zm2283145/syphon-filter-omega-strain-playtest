#include "actor_fire.hpp"
#include <stdexcept>

namespace omega {
namespace {
// 0049f790 rows 0..24, byte +4c, as the boot image's static initializer leaves them.
constexpr std::array<bool,25> secondary_fire_states{
    false,false,true,true,true,true,true,false,true,true,true,true,true,false,false,false,true,false,false,false,false,false,false,true,false};
const WeaponRecord* held_record(const WeaponInventory& inv,const WeaponDatabase& db){
    if(inv.current_84==weapon_no_slot)return nullptr;
    if(inv.current_84>=8)throw std::runtime_error("Actor fire: current slot outside the slot array");
    return &db.record(inv.slots[inv.current_84].item);
}
}

std::int32_t weapon_held_class(const WeaponInventory& inv,const WeaponDatabase& db){
    const auto* rec=held_record(inv,db);
    return rec?static_cast<std::int8_t>(rec->hold_class_f0()):11;
}

bool weapon_can_attack(const WeaponInventory& inv,const WeaponDatabase& db){
    const auto* rec=held_record(inv,db);
    if(!rec)return false;
    const auto hold=rec->hold_class_f0();
    const auto& slot=inv.slots[inv.current_84];
    std::vector<WeaponInventoryEvent> none;const WeaponInventoryHost host;
    WeaponInventory copy=inv;
    switch(hold){
    case 0:case 8:case 9:return WeaponInventoryOps(copy,db,host,none).ready(false);
    case 1:case 7:return slot.spare>0;
    case 3:case 4:case 5:case 10:return true;
    default:return false;
    }
}

bool weapon_dry_fire(const WeaponInventory& inv,const WeaponDatabase& db,bool player,bool byte_14,std::vector<WeaponDischargeEvent>& events){
    const auto* rec=held_record(inv,db);
    if(!rec||rec->hold_class_f0()!=0)return false;
    const auto& slot=inv.slots[inv.current_84];
    if(slot.clip!=0)return false;
    if(slot.spare>0){
        WeaponDischargeEvent e;e.kind=WeaponDischargeEvent::Kind::reload_action;events.push_back(e);
        return true;
    }
    if(!byte_14||!player)return false;
    const auto kind=rec->item_kind_48();
    WeaponDischargeEvent e;e.kind=WeaponDischargeEvent::Kind::dry_fire_sound;
    e.value=kind==2?0x004f2990:kind==4?0x004f2b30:0x004f27f0;
    events.push_back(e);
    return false;
}

bool secondary_state_allows_fire(std::uint8_t state){
    if(state>=secondary_fire_states.size())throw std::runtime_error("Actor fire: secondary state outside the 0049f790 table");
    return secondary_fire_states[state];
}

ActorFireResult handle_actor_fire(WeaponInventory& inv,const WeaponDatabase& db,const ActorFireRequest& request,
    const ActorFireState& state,const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events){
    if(state.life_state==2||state.life_state==3)return ActorFireResult::ignored;
    const auto cls=static_cast<std::uint32_t>(weapon_held_class(inv,db))&0xffU;   // andi 0xff
    const bool unarmed=cls==11;
    const bool melee=cls>=3&&cls<=10&&((1U<<(cls-3))&0x83U)!=0;   // 3, 4, 10
    weapon_dry_fire(inv,db,host.player,host.byte_14,events);
    if(!unarmed&&!weapon_can_attack(inv,db))return ActorFireResult::ignored;
    const auto f=state.flags_3210;
    bool busy=(f>>4&1U)||(f>>5&1U)||(f>>7&1U)||(f>>8&1U)||(f>>20&1U);
    if(!busy&&(f>>2&1U))busy=!(state.flags_3248>>3&1U);
    if(cls==1||cls==7){                                                     // 001962c8 / 00196318
        if(busy||state.byte_33e4)return ActorFireResult::ignored;
        if(!host.start_throw)throw std::runtime_error(cls==1?"Actor fire: the throw motion (00197560) is not hosted":
            "Actor fire: the hold-class-7 path (00197140) is not hosted");
        host.start_throw(static_cast<std::int32_t>(cls));
        return ActorFireResult::throw_requested;
    }
    if(melee||unarmed){
        if(busy)return ActorFireResult::ignored;
        if(unarmed&&!state.agent_unlock_0)return ActorFireResult::ignored;
        if(state.byte_33e8)return ActorFireResult::ignored;
        if(!host.melee_attack)throw std::runtime_error("Actor fire: the melee attack (001414d0 / 00196480) is not hosted");
        host.melee_attack();                                                // 001963ac..001963c4
        return ActorFireResult::melee_requested;
    }
    if(!state.secondary_states)return ActorFireResult::ignored;
    if(!secondary_state_allows_fire((*state.secondary_states)[0])||!secondary_state_allows_fire((*state.secondary_states)[1]))
        return ActorFireResult::ignored;
    WeaponDischargeInput in;
    in.target=request.target;in.target_id=request.target_id;in.subtarget=request.subtarget;in.aim_view=request.aim_view;in.continuation=false;
    return discharge_weapon(inv,db,in,host,events)?ActorFireResult::fired:ActorFireResult::not_fired;
}

}
