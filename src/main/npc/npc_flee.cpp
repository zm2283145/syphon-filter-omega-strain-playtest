#include "npc_flee.hpp"
#include "npc_alert.hpp"
#include "npc_node_search.hpp"
#include "npc_perception_think.hpp"
#include "npc_route.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("NPC AI mode 6 (001548f0): "+what);}
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
bool visible(const NpcAiComponent& c){return bit(c.perception_flags_20,0)&&bit(c.perception_flags_20,2);}   // perception +20
NpcPerception& perception(NpcAiHost& h){
    if(!h.perception||!h.sense)fail("the perception (+1ac) is not hosted");
    return *h.perception;
}
// 001549c4..00154a78 / 00154aec..00154ba0.
void search(NpcAiComponent& c,NpcAiHost& h){
    if(!h.routes||!h.route_client)fail("the node search needs the route manager 004ed9b0 (NpcAiHost::routes / route_client)");
    if(npc_route_head_pending(*h.routes,h.route_client->id))return;         // 00163fd0
    if(!h.graph)fail("the node search needs the node collection");
    if(!h.actor.world_matrix)fail("the actor's virtual +f4 is not hosted");
    const NpcNodeSearchQuery q{0.0F,-10000.0F,0};                           // 0015e0f0(sp, 0, 0.0, -10000.0)
    const RootVector self=h.actor.world_matrix()[3];                         // virtual +f4 row 3
    const RootVector target=perception(h).last_known_00;                     // +1ac +00
    const auto start=npc_search_start_region(c.path,*h.graph);               // 001612d0(+1b0)
    (void)npc_node_search_request(*h.routes,h.route_client,10000.0F,q,self,target,start,6,2,-1);
    c.byte_13f=2;
}
}

bool npc_flee_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        if(m.event==3){                                                      // 00154944
            if(c.state_13c==0&&visible(c))search(c,host);                    // virtual +a4, +20 bits 0 / 2
            c.timer_e8=60;                                                   // 00154a88
            return false;
        }
        if(m.event==0){                                                      // 00154a98
            c.timer_e8=60;
            request_npc_weapon_draw(c,0,20);                                 // virtual +bc(0, 20)
            c.firing_node_e0=-1;c.cover_node_e4=-1;                          // 00154abc / 00154ad0
            search(c,host);
            return true;
        }
        if(m.event==1)request_npc_weapon_draw(c,1,20);                       // 00154bbc virtual +bc(1, 20)
        return false;
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;                        // 00154bd8: key 004ed9b8
    if(m.event==7){
        if(m.value!=10)return false;                                         // 00154c08: +28
        auto& p=perception(host);
        const auto& h=*host.sense;
        const auto target=npc_resolve_handle(h,p.target_40,p.target_id_44);  // +40 valid while its +c is +44
        if(target==0)return false;
        if(!npc_is_enemy(c,h,target))return false;                           // 00154cd0 virtual +70
        if(visible(c))return false;                                          // 00154ce8
        if(!h.object)fail("the target's view is not hosted");
        const auto v=h.object(target);
        if(!v)fail("no view of the target");
        npc_request_look_at(c,target,v->id,0,1.0F);                          // 00154d88 virtual +128(target, 0, 1.0)
        return false;
    }
    if(m.event==6)c.timer_e8=1;                                              // 00154da0
    return false;
}
}
