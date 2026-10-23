#include "actor_reload.hpp"
#include "secondary_playback.hpp"
#include <stdexcept>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
// 0013fd20(inv, deficit, ammo type): the rounds taken from the other slots.
std::int32_t transfer_ammo(WeaponInventory& inv,const WeaponDatabase& db,std::int32_t deficit,std::int32_t ammo,
    bool notice_actor,std::vector<WeaponReloadEvent>& events){
    std::int32_t taken=0;
    for(std::uint8_t k=0;k<6;++k){
        if(k==inv.current_84)continue;
        auto& s=inv.slots[k];
        if(s.item==-1)continue;
        const auto& rec=db.record(s.item);                                // 00147840
        if(rec.ammo_type_10c()!=ammo||rec.hold_class_f0()!=0||s.spare<=0)continue;
        if(notice_actor){WeaponReloadEvent e;e.kind=WeaponReloadEvent::Kind::transfer_notice;e.item=s.item;e.slot=k;events.push_back(e);}
        if(deficit<s.spare){s.spare-=deficit;return taken+deficit;}  // 0013fe0c
        deficit-=s.spare;taken+=s.spare;s.spare=0;
    }
    return taken;
}
}

ActorReloadResult handle_actor_reload(const ActorReloadInput& in,const ActorReloadHost& host){
    ActorReloadResult r;
    const auto m=in.mode_320c;
    if(m==0x14||m==0x1b||m==0x1a||m==0x1f){r.refused=true;return r;}
    const auto f=in.flags_3210;
    if(bit(f,4)||bit(f,5)||bit(f,8)||bit(f,7)||bit(f,16)){r.refused=true;return r;}
    if(!host.reload_allowed)throw std::runtime_error("Actor reload: 00143560 is not hosted");
    if(!host.reload_allowed())return r;
    r.allowed=true;
    if(!host.item_kind)throw std::runtime_error("Actor reload: 00143510 is not hosted");
    bool named=true;                                                     // s2 = s0 (1)
    if(host.item_kind()==1||host.item_kind()==3){}
    else if(host.item_kind()==2||host.item_kind()==4){}
    else if(host.item_kind()==5||host.item_kind()==6){}
    else named=false;
    if(!in.secondary_3314||!named)return r;
    SecondaryOneShot e;                                                  // 003c9e30(ev, 00542b60), vtable 004da930
    e.code=1;e.action=0;e.receiver=in.actor;e.posted=0;e.value=0.0F;
    if(!host.one_shot)throw std::runtime_error("Actor reload: 001cae20 is not hosted");
    r.requested=true;
    r.accepted=host.one_shot(secondary_motion_name(actor_reload_motion),1,e);
    if(!r.accepted)return r;
    if(!host.start)throw std::runtime_error("Actor reload: 0013fb90 is not hosted");
    host.start();
    return r;
}

void reload_weapon(WeaponInventory& inv,const WeaponDatabase& db,bool notice_actor,std::vector<WeaponReloadEvent>& events){
    if(inv.current_84==weapon_no_slot)return;
    if(inv.current_84>=inv.slots.size())throw std::runtime_error("Weapon reload: held slot out of range");
    auto& s=inv.slots[inv.current_84];
    if(s.item==-1)return;
    const auto& rec=db.record(s.item);                                    // 00147840
    const std::int32_t need=rec.clip_70()-s.clip;
    std::int32_t loaded=need,transferred=0;
    if(s.spare<need){
        transferred=transfer_ammo(inv,db,need-s.spare,rec.ammo_type_10c(),notice_actor,events);
        loaded=s.spare;
    }
    s.clip=s.clip+(loaded+transferred);
    if(!inv.infinite_reserve_99)s.spare-=loaded;
    if(inv.current_84==0&&inv.slots[1].item!=-1){inv.slots[1].clip=s.clip;inv.slots[1].spare=s.spare;}
    WeaponReloadEvent e;e.kind=WeaponReloadEvent::Kind::sound;e.item=s.item;e.name=rec.reload_sound_20();
    events.push_back(e);
}

}
