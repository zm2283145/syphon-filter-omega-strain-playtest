#include "npc_script_control.hpp"
#include "npc_alert.hpp"
#include "npc_perception_think.hpp"
#include <stdexcept>
#include <string>

namespace omega {
NpcRouteManager& npc_route_manager(NpcAiHost& host){
    if(!host.routes)throw std::runtime_error("NPC AI: route following needs the route manager 004ed9b0 (NpcAiHost::routes)");
    if(!host.route_client)throw std::runtime_error("NPC AI: route following needs the actor's request handle (NpcAiHost::route_client)");
    return *host.routes;
}
std::uint8_t npc_move_to_node(NpcAiComponent& c,std::int32_t node){
    request_npc_ai_mode(c,7,100);                                            // 0016e170(+44, 7, 100)
    c.pending_node_138=node;c.object_f4=-1;
    return 1;
}
std::uint8_t npc_move_to_object(NpcAiComponent& c,std::int32_t object_id){
    c.object_f4=object_id;c.pending_node_138=-1;                             // 0014be20..0014be2c
    request_npc_ai_mode(c,7,100);                                            // 0016e170(+44, 7, 100)
    return 1;
}
std::uint8_t npc_carry_body_to_node(NpcAiComponent& c,std::uint8_t body_life,bool body_controller,std::int32_t node,
    std::int32_t body_id){
    if(body_life!=2&&body_life!=3)return 0;                                  // 0014bd90..0014bda8
    if(!body_controller||c.mode_44==8)return 0;                              // body +58, component +44
    request_npc_ai_mode(c,8,100);                                            // 0016e170(+44, 8, 100)
    c.carry_node_f0=node;c.object_f4=body_id;                                // 0014bdd4..0014bde0
    return 0;
}
void npc_follow(NpcAiComponent& c,std::optional<std::int32_t> target_id,float a,float b){
    if(!target_id)return;                                                    // 00148968: beqz s0
    c.object_f4=*target_id;                                                  // 00148980: +f4 = target +c
    c.follow_100=a;c.follow_104=b;                                           // 00148988 / 00148990
    c.follow_108=0;c.follow_10c=0;                                           // 00148994 / 0014899c
    request_npc_ai_mode(c,9,100);                                            // 0016e170(+44, 9, 100)
}
void npc_set_mode_at_node(NpcAiComponent& c,std::uint32_t mode,std::int32_t node,NpcAiHost& host){
    const auto low=static_cast<std::uint8_t>(mode&0xffU);
    if(!(low<7))return;
    request_npc_ai_mode(c,low,100);
    c.byte_13f=static_cast<std::uint8_t>(mode&0xffU);                        // sb a1, +13f
    auto& m=npc_route_manager(host);
    npc_route_to_node(m,c.path,host.route_client,node);                      // 0015fb20(+1b0, node +78)
}
bool npc_script_control_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        if(m.event==0){
            auto& manager=npc_route_manager(host);
            npc_route_release(manager,host.route_client->id);                // 00163c50(manager, +30)
            if(c.pending_node_138>=0)npc_route_to_node(manager,c.path,host.route_client,c.pending_node_138);
            else if(c.object_f4!=-1){
                if(!host.object_by_id)throw std::runtime_error("NPC AI: mode 7 toward an object +f4 needs 0040c540 (NpcAiHost::object_by_id)");
                if(const auto object=host.object_by_id(c.object_f4)){                // 001546f4 0040c540
                    if(!host.sense||!host.sense->object)throw std::runtime_error("NPC AI: mode 7 toward an object +f4 needs its virtual +108 (NpcAiHost::sense)");
                    const auto view=host.sense->object(object);
                    if(!view)throw std::runtime_error("NPC AI: mode 7 toward an object +f4 with no view of it");
                    if(view->life_108==3){                                          // 00154718: dead -> route toward the body
                        if(!host.go_to_body)throw std::runtime_error("NPC AI: mode 7 toward a dead object +f4 needs its host (NpcAiHost::go_to_body)");
                        auto& b=*host.go_to_body;
                        using C=NpcSenseObject::Controller;
                        std::int32_t region;
                        if(view->controller==C::player){                            // 0015473c +54, 00154758 003cc950
                            if(!host.sense->player_region)throw std::runtime_error("NPC AI: mode 7 toward a dead player needs 004ed9c0 (NpcPerceptionHost::player_region)");
                            if(view->team<0)throw std::runtime_error("NPC AI: mode 7: 004ed9c0 indexed by a negative 003cc950");
                            region=host.sense->player_region(view->team);           // 00154760..00154774
                        }else if(view->controller==C::ai){
                            region=view->region_5c;                                 // 0015478c +58, 001547a8 +58 +1b0 +5c
                        }else{
                            if(!b.region_at)throw std::runtime_error("NPC AI: mode 7 toward a dead object needs 0015d790 (NpcGoToBodyHost::region_at)");
                            region=b.region_at(view->position,c.path.destination_5c,host.sender_f20);   // 001547bc +1b0 +5c, 001547c4 +e8, 001547d4 (f20: the sender's, 132uk6d)
                        }
                        if(!b.world_f4||!b.route_to_point)throw std::runtime_error("NPC AI: mode 7 toward a dead object needs its +f4 and 0015fa70 (NpcGoToBodyHost)");
                        (void)b.route_to_point(b.world_f4(object)[3],region);      // 001547ec +f4, 00154800 0015fa70
                        c.timer_e8=-1;                                              // 00154808
                    }
                }                                                                   // live or unresolved: 00154810 (Jog, no route)
            }
            const float jog=npc_travel_speed(2);                                // virtual +148(2)
            if(c.speed_priority_64<11){
                c.travel_speed_60=jog;c.speed_priority_64=10;
                c.flags_141=static_cast<std::uint8_t>(c.flags_141&~0x40U);
            }
            return true;
        }
        if(m.event==1){
            if(c.speed_priority_64<11)c.speed_priority_64=10;
            return true;
        }
        return false;
    }
    if(m.kind==NpcAiMessage::Kind::path&&m.event==2)resume_npc_movement(c,host);   // virtual +158
    return false;
}
void npc_route_ready(NpcAiComponent& c,NpcAiHost& host){
    c.byte_140=c.byte_13f;c.byte_13f=0;                                      // lb +13f, sb +140
    if(!host.graph)throw std::runtime_error("NPC AI host: no node collection");
    NpcPathHost ph;
    ph.actor_translation=[&host]{return host.actor.world_matrix()[3];};
    ph.send=[&host](const NpcPathEvent& e){
        NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=0;s.message={NpcAiMessage::Kind::path,e.event,e.value};
        host.send(s);
    };
    if(!start_npc_route(c.path,*host.graph,ph))return;                        // 00160ce0
    set_npc_movement_state(c,1,20,host);                                     // 0014d510(c, 1, 20)
    npc_travel_speed_for_purpose(c,c.byte_140,host);                          // virtual +d4(+140, 10)
}
}
