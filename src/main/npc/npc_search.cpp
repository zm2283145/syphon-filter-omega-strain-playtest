#include "npc_search.hpp"
#include "npc_alert.hpp"
#include "npc_attack.hpp"
#include "npc_cover.hpp"
#include "npc_perception_think.hpp"
#include "npc_route.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
bool visible(const NpcAiComponent& c){return bit(c.perception_flags_20,0)&&bit(c.perception_flags_20,2);}   // perception +20 bits 0, 2
const NpcNodeGraph& graph(NpcAiHost& h){
    if(!h.graph)throw std::runtime_error("NPC AI host: no node collection");
    return *h.graph;
}
NpcPerception& perception(NpcAiHost& h){
    if(!h.perception||!h.sense)throw std::runtime_error("NPC AI: Search and the look-around read the perception (+1ac), which is not hosted");
    return *h.perception;
}
// The slot +1b8 (null before Activate assigned one).
NpcLookSlot* slot(const NpcAiComponent& c,NpcAiHost& h){
    if(c.look_slot_1b8<0)return nullptr;
    if(!h.system)throw std::runtime_error("NPC AI: the look-around's slot list 004ea1d0 needs the shared AI state (NpcAiHost::system)");
    if(c.look_slot_1b8>=static_cast<std::int32_t>(h.system->look_slots.size()))throw std::runtime_error("NPC AI: look slot +1b8 outside 004ea1d0");
    return &h.system->look_slots[static_cast<std::size_t>(c.look_slot_1b8)];
}
NpcLookSlot& slot_read(const NpcAiComponent& c,NpcAiHost& h,const char* who){
    auto* s=slot(c,h);
    if(!s)throw std::runtime_error(std::string("NPC AI: ")+who+" reads the list of look slot +1b8 before Activate assigned one (address 4)");
    return *s;
}
// Look entries 0 and 1 (+16c + 20 i) and entry 2 (+194..+1a5, RequestFacial's).
NpcAiComponent::LookEntry& entry(NpcAiComponent& c,unsigned i){
    if(i<2)return c.look_entries[i];
    if(i==2)return c.look_entry_2;
    throw std::runtime_error("NPC AI: the look-around on look entry "+std::to_string(i)+" (outside the three entries)");
}
// A point's object resolved (the weak handle {object, id}); nullopt: it does
// not resolve, or is not a node object of this collection.
std::optional<std::int32_t> resolve(const NpcLookPoint& p,const NpcNodeGraph& g){
    if(p.object==0)return std::nullopt;
    const auto n=g.node_of_object(p.object);
    if(!n)throw std::runtime_error("NPC AI: a look point that is not a node object");
    if(g.node(*n).id!=static_cast<std::uint32_t>(p.id))return std::nullopt;
    return n;
}
// The VU's zero flag (vaddx.xyz by vf0.x, MAC flags == 0xe): exact zeros and
// the denormals the VU flushes.
bool vu_zero(float x){const auto k=std::fpclassify(x);return k==FP_ZERO||k==FP_SUBNORMAL;}
void request_combat(NpcAiComponent& c){
    request_npc_ai_mode(c,c.aggressiveness_6c<std::bit_cast<float>(0x3f666666U)?3:4,0);   // c.olt.s +6c, 0.9
}
}

void npc_look_begin(NpcAiComponent& c,std::uint8_t index,std::uint8_t nearest,std::int32_t priority,NpcAiHost& host){
    if(priority<c.look_160)return;
    if(auto* s=slot(c,host))s->points.clear();
    c.look_timer_15c=0.0F;c.look_index_164=index;c.look_nearest_165=nearest;
    c.byte_166=0;c.look_160=priority;c.look_resume_168=0;
}
void npc_look_add_point(NpcAiComponent& c,std::int32_t node,std::int32_t priority,float duration,NpcAiHost& host){
    if(priority<c.look_160)return;
    const auto& g=graph(host);
    if(auto* s=slot(c,host)){
        NpcLookPoint p;p.object=npc_node_object(node);p.id=static_cast<std::int32_t>(g.node(node).id);p.duration=duration;
        s->points.push_back(p);
    }
    c.byte_166=1;
}
void npc_look_add_nodes(NpcAiComponent& c,std::int32_t region,std::int32_t node,std::int32_t exclude,float duration,NpcAiHost& host){
    const auto& g=graph(host);
    if(node>=0){
        for(const auto& l:g.node(node).links){
            if(l.target==exclude)continue;
            if(region<0||g.node(l.target).parameter_7c==region)npc_look_add_point(c,l.target,c.look_160,duration,host);
        }
        return;
    }
    if(region<0)return;
    if(!host.routes||!host.routes->regions)throw std::runtime_error("NPC AI: 0014ef80 reads the region groups 004ffbe8 (NpcRouteManager::regions)");
    const auto& groups=host.routes->regions->groups;
    if(static_cast<std::size_t>(region)>=groups.size())throw std::runtime_error("NPC AI: 0014ef80 with a region beyond the groups (no bound check in the original)");
    for(const auto n:groups[static_cast<std::size_t>(region)])npc_look_add_point(c,n,c.look_160,duration,host);
}
std::optional<std::int32_t> npc_look_facing_node(NpcAiComponent& c,NpcAiHost& host){
    const auto& s=slot_read(c,host,"0014ee90");
    const auto& g=graph(host);
    std::int32_t best=0;std::optional<std::int32_t> out;
    for(const auto& p:s.points){
        if(!(best<p.count))continue;
        best=p.count;
        out=resolve(p,g);
    }
    return out;
}
void npc_stop_movement(NpcAiComponent& c,std::int32_t priority,float duration,NpcAiHost& host){
    set_npc_movement_state(c,0,priority,host);                                 // 0014d510(c, 0, priority)
    c.stop_timer_7c=duration;
    c.look_resume_168=0;
}
void npc_look_around(NpcAiComponent& c,std::int32_t priority,NpcAiHost& host){
    if(!host.actor.eligible)throw std::runtime_error("NPC AI host: eligible");
    if(!host.actor.eligible())return;                                        // +2d, +2c, !+2e
    npc_look_begin(c,0,1,priority,host);
    const std::int32_t exclude=static_cast<std::uint8_t>(c.state_13c)==1?c.path.previous_54:-1;
    if(c.path.hold_4c>=0)npc_look_add_nodes(c,-1,c.path.hold_4c,exclude,1.0F,host);
    else npc_look_add_nodes(c,c.path.destination_5c,-1,exclude,1.0F,host);
}

void npc_stop_and_look_around(NpcAiComponent& c,std::int32_t priority,NpcAiHost& host){
    if(!host.actor.eligible)throw std::runtime_error("NPC AI host: eligible");
    if(!host.actor.eligible())return;                                        // 0014dcb8..0014dce8
    const std::int8_t state=c.state_13c;                                     // 0014dcf8: lb +13c
    npc_look_around(c,priority,host);                                        // +134
    npc_stop_movement(c,priority,std::bit_cast<float>(0x461c4000U),host);    // +154, f12 = 10000.0
    if(static_cast<std::uint8_t>(state)!=0)c.look_resume_168=1;              // 0014dd28..0014dd34
}

void npc_look_around_select(NpcAiComponent& c,float dt,NpcAiHost& host){
    c.look_timer_15c-=dt;
    const unsigned index=c.look_index_164;
    if(!(c.look_timer_15c<=0.0F)){
        const auto& e=entry(c,index);
        if(e.active&&!(e.priority<c.look_160))return;                          // entry +164 still looking
    }
    auto& s=slot_read(c,host,"0014e0c0");
    const auto& g=graph(host);
    if(!host.attack||!host.attack->eye_c0)throw std::runtime_error("NPC AI: the look-around reads the actor's eye +c0 (NpcAttackHost::eye_c0)");
    NpcLookPoint* best=nullptr;
    float best_length=std::bit_cast<float>(0x49742400U);                       // 1e6
    for(auto& point:s.points){
        if(point.used)continue;
        if(!host.sense)throw std::runtime_error("NPC AI: the look-around reads its actor (NpcAiHost::sense)");
        const RootVector self=host.attack->eye_c0(host.sense->self,-1);        // actor virtual +c0(-1)
        const auto n=resolve(point,g);
        if(!n)throw std::runtime_error("NPC AI: a look point whose node no longer resolves (0014e238 calls +c0 of a null object)");
        const RootVector there=g.position(*n);                                 // node virtual +c0 -> +e4: +80, w 1
        RootVector v{self[0]-there[0],self[1]-there[1],self[2]-there[2],self[3]-there[3]};
        float length=0.0F;
        if(!(vu_zero(v[0])&&vu_zero(v[1])&&vu_zero(v[2]))){
            length=vu::sqrt((v[0]*v[0]+v[1]*v[1])+v[2]*v[2]);
            const float inverse=vu::div(1.0F,length);
            for(int i=0;i<3;++i)v[i]*=inverse;
        }
        bool seen=length<=3.0F||!(v[2]<std::bit_cast<float>(0x3f666666U));
        if(!seen)seen=npc_sight_test(perception(host),*host.sense,there,true,0);   // 0016c260(+1ac, eye, 1, 0)
        if(seen){
            point.used=1;
            if(best)++best->count;
            continue;
        }
        if(c.look_nearest_165){
            if(length<best_length){best_length=length;best=&point;}
            continue;
        }
        best=&point;
        break;
    }
    if(best){
        const float duration=best->duration;
        const std::int32_t priority=c.look_160;
        const auto object=resolve(*best,g);
        auto& e=entry(c,index);
        if(!(priority<e.priority)){
            if(e.active&&!(priority<e.priority)){e.active=0;e.priority=0;}     // virtual +a8(+164)
            e.object=object?npc_node_object(*object):0;
            e.id=object?static_cast<std::int32_t>(g.node(*object).id):-1;
            e.active=1;e.flag=1;e.duration=duration;e.priority=priority;
        }
        best->used=1;
        c.look_timer_15c=best->duration;
        c.look_167=1;
        return;
    }
    auto& e=entry(c,index);
    if(c.look_160<e.priority)return;
    e.active=0;e.priority=0;
    c.byte_166=0;c.look_160=0;
    if(c.look_resume_168)resume_npc_movement(c,host);                        // virtual +158
}

bool npc_search_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        switch(m.event){
        case 3:                                                              // the +e8 countdown
            if(c.mode_counter_ec!=2)return false;
            c.timer_e8=1;
            if(c.byte_166)return false;                                      // still looking around
            c.mode_counter_ec=0;c.timer_e8=-1;
            if(!(c.movement_priority_a0<100))return false;
            if(bit(c.path.flags_60,3)){
                c.facing_node_1d0=-1;
                if(const auto f=npc_look_facing_node(c,host))c.facing_node_1d0=*f;   // class word *004ea770: a node
                request_npc_ai_mode(c,0,0);
            }else resume_npc_movement(c,host);                               // virtual +158
            return false;
        case 4:
            if(visible(c)){
                set_npc_movement_state(c,0,20,host);                         // 0014d510(c, 0, 20)
                request_combat(c);
            }
            return false;
        case 7:
            request_npc_ai_mode(c,1,0);
            return false;
        case 0:                                                              // enter
            if(visible(c))request_combat(c);
            else{
                c.timer_e8=-1;c.mode_counter_ec=0;c.firing_node_e0=-1;c.cover_node_e4=-1;
                if(c.path.destination_5c>=0){
                    auto& p=perception(host);
                    const std::int32_t region=p.region_18;
                    if(region>=0&&!npc_region_bit(c,region))(void)npc_approach_search(c,host);   // 0014fd30
                    else if(c.contact_158&&region>=0){
                        if(!host.routes||!host.route_client)throw std::runtime_error("NPC AI: Search's route to the last known point needs the route manager 004ed9b0");
                        if(!npc_route_head_pending(*host.routes,host.route_client->id))   // 00163fd0
                            (void)npc_route_to_point(*host.routes,c.path,host.route_client,p.last_known_00,region);   // 0015fa70(+1b0, +1ac, region)
                    }
                }
            }
            c.mode_counter_ec=0;
            return true;
        case 1:{                                                             // exit
            if(c.mode_counter_ec!=2)return false;
            auto& e=entry(c,c.look_index_164);
            if(!(e.priority<21))return false;
            e.active=0;e.priority=0;
            c.byte_166=0;c.look_160=0;
            if(c.look_resume_168)resume_npc_movement(c,host);                // virtual +158
            return false;}
        default:return false;
        }
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;
    switch(m.event){
    case 6:                                                                  // destination reached
        c.timer_e8=1;c.mode_counter_ec=2;
        npc_look_begin(c,1,1,20,host);
        npc_look_add_nodes(c,c.path.destination_5c,-1,-1,3.0F,host);
        return false;
    case 5:{                                                                 // waypoint reached
        if(c.mode_counter_ec!=1)return false;
        const auto hold=c.path.hold_4c;
        if(hold<0){c.mode_counter_ec=0;return false;}
        c.timer_e8=1;c.mode_counter_ec=2;
        npc_stop_movement(c,10,10.0F,host);                                  // virtual +154
        npc_look_begin(c,1,1,20,host);
        npc_look_add_nodes(c,c.path.destination_5c,hold,-1,2.0F,host);
        return false;}
    case 7:                                                                  // path-link type
        if(m.value==10&&c.mode_counter_ec==0)c.mode_counter_ec=1;
        return false;
    default:return false;
    }
}
}
