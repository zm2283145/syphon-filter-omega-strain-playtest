#include "npc_avoid_grenade.hpp"
#include "npc_node_search.hpp"
#include "npc_route.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("NPC AI mode 10 (001535e0): "+what);}
template <class F> const F& need(const F& f,const char* what){if(!f)fail(std::string("the host does not supply ")+what);return f;}
// 0040c540(&+f4) and the projectile's view (null object: nullopt).
std::optional<NpcProjectileView> projectile_at(const NpcAiComponent& c,NpcAiHost& host,const char* site){
    const auto object=need(host.object_by_id,"0040c540 (NpcAiHost::object_by_id)")(c.object_f4);
    if(object==0)return std::nullopt;
    const auto view=need(host.projectile,"the projectile view (NpcAiHost::projectile)")(object);
    if(!view)fail(std::string(site)+": +f4 names an object that is not a thrown projectile");
    return view;
}
}

bool npc_avoid_grenade_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind!=NpcAiMessage::Kind::ai)return false;                          // 00153600..00153614: key 004ee550
    if(m.event==3){                                                          // 00153630
        const auto p=projectile_at(c,host,"00153638");
        const bool present=p&&p->root[1]!=0&&p->root[0]!=0&&p->root[2]==0;   // +2d, +2c, !+2e
        if(!present){
            c.priority_4c=0;                                                 // 001536a0 (the delay slot)
            request_npc_ai_mode(c,1,0);                                      // 0015369c 0016e170(+44, 1, 0)
        }
        c.timer_e8=4;                                                        // 001536b0
        return false;
    }
    if(m.event==0){                                                          // 001536c0
        if(const auto p=projectile_at(c,host,"001536c8")){
            if(!host.routes||!host.route_client)fail("the node search needs the route manager 004ed9b0 (NpcAiHost::routes / route_client)");
            if(!npc_route_head_pending(*host.routes,host.route_client->id)){  // 00153708 00163fd0
                if(!host.graph)fail("the node search needs the node collection");
                const NpcNodeSearchQuery q{0.0F,-10000.0F,0};                // 0015372c 0015e0f0(sp+c8, 0, 0.0, -10000.0)
                const RootVector self=need(host.actor.world_matrix,"the actor's virtual +f4")()[3];   // 00153740
                const auto start=npc_search_start_region(c.path,*host.graph);   // 00153770 001612d0(+1b0)
                (void)npc_node_search_request(*host.routes,host.route_client,10000.0F,q,self,p->point_bc,start,6,2,-1);   // 001537a8
                c.byte_13f=2;                                                // 001537b4
            }
        }
        const float run=npc_travel_speed(3);                                 // 001537c4 virtual +148(3)
        if(c.speed_priority_64<11){                                          // 001537d0
            c.travel_speed_60=run;c.speed_priority_64=10;
            c.flags_141=static_cast<std::uint8_t>(c.flags_141&~0x40U);
        }
        c.timer_e8=4;                                                        // 0015380c
        return true;
    }
    if(m.event==1){                                                          // 00153818
        if(c.speed_priority_64<11)c.speed_priority_64=10;
        return true;
    }
    return false;
}
}
