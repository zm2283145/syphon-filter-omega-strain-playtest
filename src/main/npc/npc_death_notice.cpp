#include "npc_death_notice.hpp"
#include "actor_fire.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("NPC death notice 0014f680: host ")+what+" required");
    return f;
}
void add(std::vector<NpcDeathNoticeEvent>& ev,NpcDeathNoticeEvent::Kind k,std::int32_t value=0,std::uint64_t object=0){
    ev.push_back({k,value,object});
}
}

// 00186f30(actor, killer, type, category).
void npc_kill_statistics(const NpcDeathNoticeInput& in,const NpcDeathNoticeHost& h,std::vector<NpcDeathNoticeEvent>& ev){
    using K=NpcDeathNoticeEvent::Kind;
    if(in.killer==0)return;
    const auto k=need(h.killer,"killer")(in.killer);
    if(!k||!k->player)return;                                                  // +58 virtual +54
    if(!k->byte_14){
        need(h.remote_statistics,"remote_statistics")(h.actor,in.killer,in.type,in.category);
        return;
    }
    if(!h.statistics)throw std::runtime_error("NPC death notice 0014f680: the agent record's statistics (001314f0(004ffb50, -1)) are not supplied");
    AgentStatistics& stats=*h.statistics;
    auto statistic=[&](std::int32_t index){add_agent_statistic(stats,static_cast<std::uint32_t>(index),1);add(ev,K::statistic,index);};
    statistic(0);                                                              // 00333f10(stats, 0, 1)
    if(!need(h.killer_hostile,"killer_hostile")(in.killer)){
        statistic(agent_statistic_friendly_kills);                             // 00186fe4
        const std::int32_t d=read_agent_statistic(stats,agent_statistic_level_friendly_kills);   // 00333ee0(stats, 0x19)
        const std::int32_t line=d==4?0x11:d==3?0xf:d==2?0xe:d==1?0xc:0x10;
        add(ev,K::voice,line,in.killer);                                       // 0018c770
    }
    switch(in.type){
    case 9: statistic(1);return;
    case 4: if(h.actor_3488!=0)statistic(4);return;
    case 2: statistic(4);return;
    case 10: statistic(3);return;
    case 5: return;
    default: break;
    }
    if(in.category==2)statistic(2);
}

namespace {
std::int32_t trunc_word(float v){
    if(!(v>-2147483648.0F&&v<2147483648.0F))throw std::runtime_error("NPC death notice: a delay outside the cvt.w.s range");
    return static_cast<std::int32_t>(v);
}
}

void npc_death_notice(NpcAiComponent& c,NpcPerception& p,WeaponInventoryOps& inv,const NpcDeathNoticeInput& in,
    const NpcDeathNoticeHost& h,std::vector<NpcDeathNoticeEvent>& ev){
    using K=NpcDeathNoticeEvent::Kind;
    if(h.actor==0)return;                                                      // 0014f6ac
    add(ev,K::route_reservations,0,h.actor);                                   // 00163c50
    add(ev,K::script_death,in.type,in.killer);                                 // 004ff278, transport 0
    npc_kill_statistics(in,h,ev);                                              // 00186f30
    const float pause=std::bit_cast<float>(static_cast<std::uint32_t>(c.field_84));
    if(c.field_a8>0&&pause<=0.0F&&(c.perception_flags_20&1U)!=0&&(c.perception_flags_20&4U)!=0&&inv.full_auto()&&h.secondary
       &&secondary_state_allows_fire((*h.secondary)[0])&&secondary_state_allows_fire((*h.secondary)[1])){
        c.force_item_12c=-1;                                                   // 0014b470 with item -1
        c.force_count_130=c.field_a8;
        c.force_delay_134=trunc_word(c.fire_delay_8c);                         // delay 0.0: +8c
        add(ev,K::death_burst,c.field_a8);
    }
    if(h.ammo_pct){const auto pct=h.ammo_pct();inv.randomize_dropped_ammunition(need(h.random,"random"),pct[0],pct[1]);}   // 00142170
    else inv.randomize_dropped_ammunition(need(h.random,"random"));                                                       // 00142170
    perception_clear(p);                                                       // 0016dc60
    add(ev,K::perception_cleared);
    const bool present=h.actor_root[1]!=0&&h.actor_root[0]!=0&&h.actor_root[2]==0;
    if(!(present&&h.actor_3468!=0)){
        add(ev,K::think_list_removed,need(h.remove_from_think_list,"remove_from_think_list")());   // 0015ad70
        if(present){need(h.append_to_dead_list,"append_to_dead_list")();add(ev,K::dead_list_added);}
    }
}

bool npc_corpse_removable_164(const NpcCorpseView& v){
    const bool bit2=(v.flags_141&4U)!=0;
    const bool present=v.root[1]!=0&&v.root[0]!=0&&v.root[2]==0;
    if(present&&v.pickupable_3468!=0)return false;
    return bit2;
}
namespace {
bool corpse_free(const NpcCorpseView& v){
    if(!npc_corpse_removable_164(v))return false;
    if(v.held_354c)return false;
    return !(v.action_114==7&&((v.flags_3204>>7)&1U)!=0);
}
}
bool npc_dead_removal_allowed(const NpcCorpseView& self,const std::vector<const NpcCorpseView*>& dead_list,
    const RootVector& camera,float far_distance){
    if(self.life_108!=3)return false;
    if(!corpse_free(self))return false;
    const NpcCorpseView* oldest=nullptr;
    if(dead_list.size()>=4)                                                    // *004ea370 >= 4
        for(const auto* c:dead_list)if(corpse_free(*c)){oldest=c;break;}
    if(oldest==&self)return true;
    const float far2=far_distance*far_distance;
    if(!self.eye_c0)throw std::runtime_error("NPC dead removal 00159020: the actor's eye (virtual +c0) is not hosted");
    const RootVector eye=self.eye_c0();
    const float dx=eye[0]-camera[0],dy=eye[1]-camera[1],dz=eye[2]-camera[2];
    const float d2=(dx*dx+dy*dy)+dz*dz;
    if(d2<400.0F)return false;
    if(self.word_38==0x80U)return true;
    return !(d2<far2);
}

}
