#include "weapon_update.hpp"
#include "actor_fire.hpp"
#include <stdexcept>

namespace omega {

std::int32_t weapon_carried_value(const WeaponInventory& inv,const WeaponDatabase& db){
    std::int32_t best=0;
    for(std::uint8_t k=0;k<6;++k){
        const auto& s=inv.slots[k];
        if(s.byte_0==0&&k!=inv.current_84)continue;
        if(s.item==-1)continue;
        const auto v=db.record(s.item).word_f4();
        if(best<v)best=v;
    }
    return best;
}

void stop_weapon_loop_sound(WeaponInventory& inv,const WeaponDatabase& db,std::uint8_t hand,std::vector<WeaponDischargeEvent>& events){
    if(hand>1)throw std::runtime_error("Weapon update: hand outside 0/1");
    auto& sound=inv.loop_sound_c4[hand];
    if(!sound)return;
    WeaponDischargeEvent e;e.kind=WeaponDischargeEvent::Kind::loop_sound_stop;e.value=hand;e.name=db.record(*sound).fire_sound_10();
    events.push_back(e);
    sound.reset();
}

void update_weapon_inventory(WeaponInventory& inv,const WeaponDatabase& db,float dt,const WeaponUpdateHost& host,
    const WeaponDischargeHost& discharge,std::vector<WeaponDischargeEvent>& events){
    using K=WeaponDischargeEvent::Kind;
    // 00144090..00144104: burst continuation, hand 0 then hand 1 (the discharge
    // itself fires with the active hand +a9).
    for(std::size_t h=0;h<2;++h){
        if(!(inv.burst_bc[h]>0))continue;
        std::uint64_t target=inv.burst_target_90;
        if(target!=0){
            if(!host.valid)throw std::runtime_error("Weapon update: no weak-handle check for the burst target");
            if(!host.valid(target,inv.burst_target_id_94))target=0;
        }
        WeaponDischargeInput in;
        in.target=target;in.target_id=inv.burst_target_id_94;in.subtarget=inv.burst_subtarget_9c;in.aim_view=inv.aim_view_98;in.continuation=true;
        discharge_weapon(inv,db,in,discharge,events);
    }
    for(std::uint8_t h=0;h<2;++h){
        if(!(inv.cooldown_ac[h]<=0.0F))inv.cooldown_ac[h]=inv.cooldown_ac[h]-dt;
        inv.value_b4[h]=inv.value_b4[h]+dt;
        if(!inv.aim_view_98&&host.actor_word_38!=0x80&&!(host.value_005383d8<=35.0F)&&
            (static_cast<std::uint32_t>(weapon_held_class(inv,db))&0xffU)!=8&&
            !(inv.value_b4[h]<=0.5F)&&inv.value_b4[h]<1.5F){
            if(!discharge.shot_frame)throw std::runtime_error("Weapon update: no shot frame for the muzzle smoke");
            WeaponDischargeEvent e;e.kind=K::muzzle_smoke;e.value=h;e.frame=discharge.shot_frame(h!=0,false);
            events.push_back(e);
        }
        if(inv.burst_bc[h]==0&&!(inv.value_b4[h]<=0.5F))stop_weapon_loop_sound(inv,db,h,events);
        if(!(inv.spinup_cc[h]<=0.0F)){
            const float v=inv.spinup_cc[h]-dt;
            inv.spinup_cc[h]=v<0.0F?0.0F:v;
        }
    }
    if(discharge.player){
        const auto value=weapon_carried_value(inv,db);
        if(value!=inv.selection_a4){
            inv.selection_a4=value;
            WeaponDischargeEvent e;e.kind=K::weapon_value_message;e.value=value;events.push_back(e);
        }
    }
    if(inv.object_0&&inv.object_0->state_8!=0){
        if(!host.air_taser)
            throw std::runtime_error("Weapon update: the inventory object's states 1-3 (00264e10 / 00263890 / 00262e00) are not hosted");
        host.air_taser(dt);
    }
    const bool lit=torch_lit(static_cast<std::uint32_t>(weapon_held_class(inv,db)),host.life_state);
    if(!lit&&!inv.torch_light_d4&&!inv.torch_flicker_d8.handle)return;     // 001448a8 with nothing to release
    if(!host.torch)
        throw std::runtime_error("Weapon update: the torch branch (00144330: the light 00373e70 and the flicker object) is not hosted");
    if(update_torch_light(inv,lit,dt,*host.torch)&&host.torch->lights)host.torch->lights->dirty_42=1;   // 00535d62
}

}
