#include "npc_dead_drop.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("AI dead-state drop 001a3520: host ")+what+" required");
    return f;
}
}

std::optional<std::uint64_t> npc_dead_inventory_drop(WeaponInventoryOps& inv,const WeaponDatabase& db,const NpcDeadDropView& v,
    const NpcDeadDropHost& h){
    if(v.backpack_3464!=0)return std::nullopt;                               // 001a3538
    const auto& state=inv.inventory();
    bool any=false;                                                          // 001434d0
    for(const auto& s:state.slots)if(s.item!=-1){any=true;break;}
    if(!any&&v.armor_33a0==0){
        const bool present=v.root[1]!=0&&v.root[0]!=0&&v.root[2]==0;
        if(!(present&&v.pickupable_3468!=0)&&!need(h.items_3528,"items_3528")())return std::nullopt;   // 004704f0
    }
    if(v.byte_14==0)return std::nullopt;                                     // 001a35b8
    const std::uint64_t backpack=need(h.create_backpack,"create_backpack")();   // 00257640(actor, 1, 0)
    if(state.byte_a8==0)                                                     // 00142320
        for(int k=7;k>=0;--k){
            const auto slot=static_cast<std::uint8_t>(k);
            const auto& s=state.slots[slot];
            if(s.item==-1||db.record(s.item).byte(0xa8)==0)continue;         // 00147840, record +a8
            need(h.add_weapon,"add_weapon")(backpack,s.item,s.spare,s.clip);    // 00255af0(bp, item, +c, +8, 0)
            inv.remove(slot,false);                                          // 00142450(inv, slot, 0, 0, 0)
        }
    if(v.armor_33a0!=0)need(h.add_armor,"add_armor")(backpack,v.armor_33a0,v.max_armor_339c);   // 00255a50
    return backpack;
}

}
