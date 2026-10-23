#include "npc_cover.hpp"
#include "npc_alert.hpp"
#include "npc_attack.hpp"
#include "npc_los_service.hpp"
#include "npc_node_search.hpp"
#include "npc_perception_think.hpp"
#include "npc_route.hpp"
#include "npc_search.hpp"
#include "vu_float.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
NpcRouteManager& routes(NpcAiHost& h){
    if(!h.routes)throw std::runtime_error("NPC AI: the node searches need the route manager 004ed9b0 (NpcAiHost::routes)");
    if(!h.route_client)throw std::runtime_error("NPC AI: the node searches need the actor's request handle (NpcAiHost::route_client)");
    return *h.routes;
}
const NpcNodeGraph& graph(NpcAiHost& h){
    if(!h.graph)throw std::runtime_error("NPC AI host: no node collection");
    return *h.graph;
}
NpcPerception& perception(NpcAiHost& h){
    if(!h.perception||!h.sense)throw std::runtime_error("NPC AI: the cover and firing-position searches read the perception (+1ac), which is not hosted");
    return *h.perception;
}
NpcAttackHost& attack(NpcAiHost& h){
    if(!h.attack)throw std::runtime_error("NPC AI: AttackFromCover needs the attack stages' host (NpcAiHost::attack)");
    return *h.attack;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("NPC AI cover host: ")+what);
    return f;
}
RootVector sub4(const RootVector& a,const RootVector& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};}
float vu_length(const RootVector& v){
    const float x=v[0]*v[0],y=v[1]*v[1],z=v[2]*v[2];
    return vu::sqrt((x+y)+z);
}
RootVector self_translation(NpcAiHost& h){
    if(!h.actor.world_matrix)throw std::runtime_error("NPC AI host: world_matrix");
    return h.actor.world_matrix()[3];                                          // virtual +f4 row 3
}
bool visible(const NpcAiComponent& c){return bit(c.perception_flags_20,0)&&bit(c.perception_flags_20,2);}
bool moving(const NpcAiComponent& c){return c.state_13c!=0;}                   // virtual +a4 = 0014d3b0
std::uint64_t target_of(NpcAiHost& h){
    const auto& p=perception(h);
    return npc_resolve_handle(*h.sense,p.target_40,p.target_id_44);
}
bool head_pending(NpcAiHost& h){auto& m=routes(h);return npc_route_head_pending(m,h.route_client->id);}   // 00163fd0
void route_to(NpcAiComponent& c,NpcAiHost& h,std::int32_t node){
    (void)npc_route_to_node(routes(h),c.path,h.route_client,node);            // 0015fb20(+1b0, node)
}
void set_pair(NpcAiComponent& c,std::int16_t tag,std::int16_t mode){c.pending_tag_110=tag;c.pending_mode_112=mode;}
// The node search of 0014fec0 / 00150150 / 0014fd30.
std::uint8_t search(NpcAiComponent& c,NpcAiHost& h,std::uint8_t byte,std::uint32_t mask,std::int8_t kind){
    auto& m=routes(h);
    const NpcNodeSearchQuery q{c.query_70,c.query_74,byte};                   // 0015e0f0
    const RootVector self=self_translation(h);
    const RootVector target=perception(h).last_known_00;                      // +1ac +00
    float d=vu_length(sub4(target,self));
    if(d<c.query_74)d=c.query_74;
    const float radius=15.0F+d;
    const auto start=npc_search_start_region(c.path,graph(h));                 // 001612d0(+1b0)
    return npc_node_search_request(m,h.route_client,radius,q,self,target,start,mask,kind,-1);
}
// Inventory +a0 set, or the held clip (00143940(inv, 6)) below the burst length +a4.
bool reload_due(const NpcAiComponent& c,NpcAttackHost& a){
    if(need(a.reloading_a0,"reloading_a0")())return true;
    return need(a.clip,"clip")()<c.ammo_a4;
}
std::int32_t held_item(NpcAttackHost& a){                                      // virtual +1a4 = 0014bcb0
    const auto slot=need(a.inventory_84,"inventory_84")();
    return slot==weapon_no_slot?-1:need(a.slot_item,"slot_item")(slot);
}
void send(NpcAiHost& h,const NpcAiSend& s){
    if(!h.send)throw std::runtime_error("NPC AI host: send");
    h.send(s);
}
}

bool npc_region_bit(const NpcAiComponent& c,std::int32_t region){
    const std::int32_t word=(region<0?region+31:region)>>5;                    // sra with the rounding of a signed division
    if(word<0||word>=static_cast<std::int32_t>(c.allowed_regions_bc.size()))
        throw std::runtime_error("NPC AI: the allowed-region bit of region "+std::to_string(region)+" lies outside +bc..+dc");
    return ((c.allowed_regions_bc[static_cast<std::size_t>(word)]>>(static_cast<std::uint32_t>(region)&31U))&1U)!=0;
}

std::uint8_t npc_firing_position_search(NpcAiComponent& c,NpcAiHost& host){
    const auto& g=graph(host);
    if(c.firing_node_e0>=0&&npc_region_bit(c,g.node(c.firing_node_e0).parameter_7c)){
        const auto node=c.firing_node_e0;
        if(!head_pending(host)&&npc_region_bit(c,g.node(node).parameter_7c))route_to(c,host,node);
        c.flags_141=static_cast<std::uint8_t>(c.flags_141|1U);
        c.byte_13f=3;
        return 1;
    }
    if(head_pending(host))return 0;
    const auto r=search(c,host,1,0x40,1);
    c.byte_13f=3;
    return r;
}

std::uint8_t npc_cover_search(NpcAiComponent& c,NpcAiHost& host){
    const auto& g=graph(host);
    if(c.cover_node_e4>=0&&npc_region_bit(c,g.node(c.cover_node_e4).parameter_7c)){
        npc_los_check_request(c,host);                                         // 00151790
        c.byte_13f=10;
        return 1;
    }
    if(head_pending(host))return 0;
    const auto r=search(c,host,1,6,2);
    c.byte_13f=2;
    return r;
}

std::uint8_t npc_approach_search(NpcAiComponent& c,NpcAiHost& host){
    if(head_pending(host))return 0;
    const auto r=search(c,host,0,0x80,1);
    c.byte_13f=4;
    return r;
}

std::uint8_t npc_los_check_request(NpcAiComponent& c,NpcAiHost& host){
    const RootVector origin=perception(host).last_known_00;
    if(!c.path.route_74)throw std::runtime_error("NPC AI: 00151790 without a route buffer (the original reads the point at address 0)");
    // A buffer no route was written to holds heap content: measured as the
    // origin for actors loaded with the level (NPC_COMBAT.md 5.6, as 00160130
    // in npc_travel_speed_for_purpose); for a spawned actor the host's labelled
    // undefined-read policy (NpcAiHost::spawned_route_policy_reads) takes the
    // same origin; otherwise it throws.
    RootVector point{0.0F,0.0F,0.0F,0.0F};
    if(c.path.route_74->point_00)point=*c.path.route_74->point_00;
    else if(host.level_loaded){if(host.unwritten_route_reads)++*host.unwritten_route_reads;}
    else if(host.spawned_route_policy_reads)++*host.spawned_route_policy_reads;   // the undefined-read policy of 0014d730: the origin
    else throw std::runtime_error("NPC AI: 00151790 reads the route buffer's point before any route of an actor not loaded with the level (heap content)");
    RootVector d=sub4(point,origin);
    d[2]=d[2]+4.5F;
    if(c.path.field_58>=0&&(graph(host).node(c.path.field_58).flags&4U))d[2]=d[2]-3.0F;
    if(d[0]==0.0F&&d[1]==0.0F&&d[2]==0.0F)return 0;                         // vaddx.xyz, vi17 == 0xe
    auto& m=routes(host);
    if(!m.los)throw std::runtime_error("NPC AI: 00151790 needs the LOS manager 004ea0b0 (NpcRouteManager::los)");
    auto client=host.route_client;
    NpcLosRequest q;q.origin_30=origin;q.displacement_40=d;q.mask_58=4;q.mode_5c=2;
    q.receiver_24=[client](const NpcLosResult& r){
        if(!client->deliver_los_result)throw std::runtime_error("NPC AI: the LOS-check result to a component without its delivery (NpcRouteClient::deliver_los_result)");
        client->deliver_los_result(r.hit_30);
    };
    const bool accepted=npc_los_request(*m.los,q);
    if(accepted)c.flags_68|=1U;
    return accepted?1:0;
}

bool npc_attack_from_cover_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        switch(m.event){
        case 3:{                                                                // the timer
            auto& a=attack(host);
            auto& p=perception(host);
            const auto t=target_of(host);
            std::int32_t rating=0;                                             // s4
            if(t!=0){
                if(!host.sense->object)throw std::runtime_error("NPC AI: no object views (NpcPerceptionHost::object)");
                const auto v=host.sense->object(t);
                if(!v)throw std::runtime_error("NPC AI: AttackFromCover's target is not known to the host");
                if(v->actor_class)rating=need(a.weapon_rating_of,"weapon_rating_of")(t);   // 00142b50(target +3524)
            }
            bool due=reload_due(c,a);
            if(!due){
                const std::int32_t primary=c.held_item_b0;                      // virtual +1c0(c, 0)
                if(!(c.switch_distance_b8<=0.0F)&&bit(c.perception_flags_20,4)&&primary>=0){
                    const float d=vu_length(sub4(self_translation(host),p.last_known_00));
                    if(primary==held_item(a))due=!(10.0F+d<=c.switch_distance_b8);
                    else due=d<c.switch_distance_b8;
                }
            }
            if(due&&rating>0){
                if(c.pending_mode_112!=2){npc_cover_search(c,host);set_pair(c,3,2);}
                c.timer_e8=30;
                return false;
            }
            if(moving(c)){c.mode_counter_ec=0;c.timer_e8=30;return false;}
            if(c.field_a8<c.ammo_a4&&t!=0&&rating>0){
                const float threat=npc_perception_threat(p,*host.sense,t);    // 0016c730
                const float r=c.aggressiveness_6c*static_cast<float>(rating);
                if(!(threat<=r)){
                    const auto block=need(a.aim_block,"aim_block")();           // +20 progress, +24 accuracy
                    if((r*(block[0]*block[1]))-threat<-1.0F){npc_cover_search(c,host);set_pair(c,3,2);}
                }
            }
            if(visible(c)){c.mode_counter_ec=0;c.timer_e8=30;return false;}
            if(c.mode_counter_ec>=5&&npc_search_allowed(c,host))request_npc_ai_mode(c,5,0);
            if(c.mode_counter_ec==2&&npc_search_allowed(c,host))npc_look_around(c,0,host);   // virtual +134(c, 0)
            c.mode_counter_ec+=1;
            c.timer_e8=30;
            return false;}
        case 7:{                                                                // awareness notice
            if(moving(c)||visible(c))return false;
            if(m.word_2c==6||m.word_2c==9)return false;
            const auto t=target_of(host);
            if(t==0)return false;
            if(!host.sense->object)throw std::runtime_error("NPC AI: no object views (NpcPerceptionHost::object)");
            const auto v=host.sense->object(t);
            if(!v)throw std::runtime_error("NPC AI: AttackFromCover's target is not known to the host");
            if(v->id!=m.value)return false;
            c.firing_node_e0=-1;
            npc_firing_position_search(c,host);
            c.mode_counter_ec=0;c.timer_e8=-1;
            return false;}
        case 4:
            if(visible(c)){
                if(!host.actor.mode_320c)throw std::runtime_error("NPC AI host: mode_320c");
                if(host.actor.mode_320c()!=0x1a)c.field_a8=c.ammo_a4;           // a new burst
            }
            return false;
        case 5:c.timer_e8=30;return false;
        case 8:
            if(c.aggressiveness_6c<=0.5F){npc_cover_search(c,host);set_pair(c,3,2);}
            return false;
        case 0:                                                                 // enter
            if(c.byte_13f!=3&&c.byte_140!=3)npc_firing_position_search(c,host);
            set_pair(c,3,3);
            c.timer_e8=moving(c)?-1:30;
            c.mode_counter_ec=0;
            return false;
        default:return false;
        }
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;
    switch(m.event){
    case 6:c.timer_e8=30;return false;
    case 2:
        if(c.pending_mode_112!=3)request_npc_ai_mode(c,c.pending_mode_112,0);
        return false;
    case 1:case 3:{
        auto& a=attack(host);
        if(reload_due(c,a)&&need(a.reload_allowed,"reload_allowed")()){        // 00143560(inv)
            NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=0;s.action=0x3c;s.action_kind=0;
            send(host,s);
        }
        set_pair(c,3,3);
        return false;}
    default:return false;
    }
}

bool npc_take_cover_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        switch(m.event){
        case 2:{                                                                // the tick (00156524)
            auto& a=attack(host);
            const auto& pf=c.path;
            if(static_cast<std::uint8_t>(c.state_13c)==1&&pf.field_58>=0&&pf.field_58==pf.current_50){
                const float f=npc_route_distance(pf,self_translation(host));   // 00160130
                if(f<3.0F)request_npc_fire_state(c,0,0);                       // virtual +120
                else{
                    static constexpr float table[4]={20000.0F,13.0F,13.0F,18.5F};   // 00489e20
                    const float t=table[npc_speed_class(c)];                   // virtual +14c
                    const float r=pf.remaining_40;
                    if(!(r<=t)&&r<t+1.0F){
                        if(!host.actor.mode_320c)throw std::runtime_error("NPC AI host: mode_320c");
                        if(host.actor.mode_320c()!=2&&npc_sight_test(perception(host),*host.sense,pf.target_00,false,0)){
                            if((graph(host).node(pf.field_58).flags&4U)&&!(c.flags_141&1U))npc_request_action(c,1,0,0.0F,host);
                            npc_request_action(c,2,0,0.0F,host);                // the dive
                        }
                    }
                }
            }
            if(!visible(c)){
                const std::int32_t primary=c.held_item_b0;                      // virtual +1c0(c, 0)
                bool sw=false;
                if(!(c.switch_distance_b8<=0.0F)&&bit(c.perception_flags_20,4)&&primary>=0){
                    const float d=vu_length(sub4(self_translation(host),perception(host).last_known_00));
                    if(primary==held_item(a))sw=!(10.0F+d<=c.switch_distance_b8);
                    else sw=d<c.switch_distance_b8;
                }
                if(sw){
                    auto& select=need(a.select_item,"select_item");               // virtual +1ac = 0014bae0
                    if(c.held_item_b0==held_item(a))select(c.held_item_b4);       // +1c0(c, 1)
                    else select(c.held_item_b0);
                }else if(c.mode_counter_ec==0)c.mode_counter_ec=reload_due(c,a)?1:2;
            }
            if(c.mode_counter_ec==1&&need(a.reload_allowed,"reload_allowed")()){   // 00143560(inv)
                NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=0;s.action=0x3c;s.action_kind=0;
                send(host,s);
                c.mode_counter_ec=2;
            }
            return true;}
        case 3:                                                                 // the timer (00156950)
            c.timer_e8=30;
            if(!moving(c)){
                auto& a=attack(host);
                if(need(a.inventory_84,"inventory_84")()!=weapon_no_slot&&need(a.ready,"ready")()){   // 001b16c0, 00143400(inv, 0)
                    npc_firing_position_search(c,host);
                    set_pair(c,2,3);
                }
                if(visible(c)){c.cover_node_e4=-1;c.firing_node_e0=-1;}
            }
            return true;
        case 4:
            if(visible(c))request_npc_fire_state(c,1,0);
            return true;
        case 5:c.timer_e8=40;return false;
        case 6:
            if(m.value!=0&&visible(c)&&moving(c)){set_npc_movement_state(c,0,20,host);c.timer_e8=30;}
            return false;
        case 0:                                                                 // enter
            if(c.byte_13f!=2&&c.byte_140!=2)npc_cover_search(c,host);
            c.timer_e8=moving(c)?-1:40;
            c.mode_counter_ec=0;
            return true;
        case 1:request_npc_fire_state(c,1,0);return true;                       // exit
        default:return false;
        }
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;
    switch(m.event){
    case 5:npc_los_check_request(c,host);return false;
    case 6:
        if(m.value>=0&&c.pending_50==0&&(graph(host).node(m.value).flags&4U)&&!(c.flags_141&1U))
            npc_request_action(c,1,0,0.0F,host);                                  // crouch at a CrouchCover node
        request_npc_fire_state(c,1,0);
        c.timer_e8=40;
        return false;
    case 1:
        if(c.pending_mode_112==2)request_npc_ai_mode(c,4,0);
        return false;
    case 2:
        if(c.pending_tag_110==2)request_npc_ai_mode(c,c.pending_mode_112,0);
        return false;
    case 3:c.timer_e8=30;return false;
    default:return false;
    }
}

void npc_dispatch_search_result(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    const auto node=m.value;
    if(node<0)return;
    if(c.byte_13f==2){c.cover_node_e4=node;c.firing_node_e0=m.word_2c;}
    else{c.firing_node_e0=node;c.cover_node_e4=m.word_2c;}
    if(!head_pending(host)&&npc_region_bit(c,graph(host).node(node).parameter_7c))route_to(c,host,node);
    c.flags_141=static_cast<std::uint8_t>(c.flags_141&~1U);
}

void npc_dispatch_los_result(NpcAiComponent& c,std::uint8_t hit,NpcAiHost& host){
    if(c.byte_13f==10){
        const auto node=c.cover_node_e4;
        if(hit!=0&&node>=0){
            if(!head_pending(host)&&npc_region_bit(c,graph(host).node(node).parameter_7c))route_to(c,host,node);
            c.flags_141=static_cast<std::uint8_t>(c.flags_141&~1U);
            c.byte_13f=2;
        }else{
            c.cover_node_e4=-1;
            npc_cover_search(c,host);
        }
    }else{
        const std::uint8_t clear=hit==0?1:0;
        if((c.flags_141&1U)!=clear){
            c.flags_141=static_cast<std::uint8_t>((c.flags_141&~1U)|clear);
            NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=0;
            s.message={NpcAiMessage::Kind::ai,6,clear,0};
            send(host,s);
        }
    }
    c.flags_68&=~1U;
}

namespace {
std::uint32_t& region_word(NpcAiComponent& c,std::int32_t region){
    const std::int32_t word=(region<0?region+31:region)>>5;
    if(word<0||word>=static_cast<std::int32_t>(c.allowed_regions_bc.size()))
        throw std::runtime_error("NPC AI: the allowed-region bit of region "+std::to_string(region)+" lies outside +bc..+dc");
    return c.allowed_regions_bc[static_cast<std::size_t>(word)];
}
}
void npc_allow_region(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node){
    const auto region=graph.node(node).parameter_7c;
    region_word(c,region)|=1U<<(static_cast<std::uint32_t>(region)&31U);
}
void npc_forbid_region(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node){
    const auto region=graph.node(node).parameter_7c;
    region_word(c,region)&=~(1U<<(static_cast<std::uint32_t>(region)&31U));
}
void npc_clear_allowed_regions(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node){
    for(auto& w:c.allowed_regions_bc)w=0;
    if(node>=0)npc_allow_region(c,graph,node);                                 // virtual +1f8
}
}
