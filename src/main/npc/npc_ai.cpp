#include "npc_ai.hpp"
#include "actor_fire.hpp"
#include "game_libm.hpp"
#include "npc_alert.hpp"
#include "npc_attack.hpp"
#include "npc_carry_body.hpp"
#include "npc_cover.hpp"
#include "npc_follow.hpp"
#include "npc_perception_think.hpp"
#include "npc_script_control.hpp"
#include "npc_search.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
bool dying_or_dead(std::uint8_t life){return life==2||life==3;}
void need(const std::function<bool()>& f,const char* what){if(!f)throw std::runtime_error(std::string("NPC AI host: ")+what);}
NpcAiSend component_message(NpcAiMessage::Kind kind,std::uint8_t event,std::int32_t value,std::uint8_t transport){
    NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=transport;s.message={kind,event,value};return s;
}
NpcAiSend actor_action(std::int32_t action,std::uint8_t kind,std::uint8_t transport){
    NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=transport;s.action=action;s.action_kind=kind;return s;
}
// A send with transport 1 to the actor reaches 001a92a0 at once.
void send_actor(NpcAiHost& host,const NpcAiSend& s){
    host.send(s);
    if(s.transport==1){
        if(!host.actor.deliver)throw std::runtime_error("NPC AI host: no synchronous delivery to the actor");
        host.actor.deliver(s);
    }
}
NpcPathHost path_host(NpcAiHost& host){
    NpcPathHost h;
    h.actor_translation=[&host]{return host.actor.world_matrix()[3];};
    // Path events reach the +58 component with transport 0.
    h.send=[&host](const NpcPathEvent& e){host.send(component_message(NpcAiMessage::Kind::path,e.event,e.value,0));};
    return h;
}
const NpcNodeGraph& graph_of(const NpcAiHost& host){
    if(!host.graph)throw std::runtime_error("NPC AI host: no node collection");
    return *host.graph;
}
// The +2c/+2d/+2e check of a node association object (0014c08c..0014c0e8).
bool association_eligible(const NpcNode& n,const NpcAiHost& host){
    switch(n.association){
    case NpcNode::Association::eligible:return true;
    case NpcNode::Association::none:case NpcNode::Association::ineligible:return false;
    default:
        if(!host.node_association_eligible)throw std::runtime_error("NPC AI: the eligibility of a node's association object (+60) is not known to the host");
        return host.node_association_eligible(n.association_key);
    }
}
// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w, vrsqrt Q, vf0.w, vmulq.xyz.
// A zero sum gives Q = +FLT_MAX (the interpreter's zero divisor, VU_ROUNDING.md).
RootVector normalize_xyz(RootVector v){
    const float sum=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];
    const float q=sum==0.0F?std::numeric_limits<float>::max():vu::rsqrt(sum);
    for(int i=0;i<3;++i)v[i]*=q;
    return v;
}
bool zero_xyz(const RootVector& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}
float yaw_of(const RootVector& d){return game_atan2f(-d[0],d[1]);}                  // 00121ab8(-x, y)
// The navigation request of 0014bff0 (action 0 kind 0xd, +20 = 1, transport 1).
void send_navigation(NpcAiHost& host,const RootVector& target,float yaw,float speed_limit){
    NpcAiSend s;s.kind=NpcAiSend::Kind::navigation_request;s.transport=1;s.action=0;s.action_kind=0xd;
    s.target=target;s.yaw=yaw;
    s.speed=speed_limit<1.0F?speed_limit:1.0F;                               // min.s f5, f4 (1.0), f21
    if(host.network_heading)host.network_heading(yaw);                       // 001a99a4 (132ln5h13)
    send_actor(host,s);
}
// VU results flush denormals to zero (sign kept); each product and sum is
// one binary32 operation (VU_ROUNDING.md).
float vu_flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float vu_mul(float a,float b){return vu_flush(a*b);}
float vu_add(float a,float b){return vu_flush(a+b);}
float vu_sub(float a,float b){return vu_flush(a-b);}
// 001320f0: vopmula / vopmsub, W = vf0.w - vf0.w = 0.
RootVector vu_cross(const RootVector& a,const RootVector& b){
    return {vu_sub(vu_mul(a[1],b[2]),vu_mul(b[1],a[2])),vu_sub(vu_mul(a[2],b[0]),vu_mul(b[2],a[0])),
            vu_sub(vu_mul(a[0],b[1]),vu_mul(b[0],a[1])),0.0F};
}
// 00132060 / 00132030: (x*x + y*y) + z*z, Q = RSQRT (FLT_MAX for a zero
// sum), xyz *= Q (multiplied, not divided by the length), W kept.
RootVector vu_normalize(RootVector v){
    const float s=vu_add(vu_add(vu_mul(v[0],v[0]),vu_mul(v[1],v[1])),vu_mul(v[2],v[2]));
    const float q=s==0.0F?std::numeric_limits<float>::max():vu_flush(vu::rsqrt(s));
    for(std::size_t i=0;i<3;++i)v[i]=vu_mul(v[i],q);
    return v;
}
// 00159e84..0015a064: the relocation onto path +50.
void npc_stuck_relocation(NpcAiComponent& c,NpcAiHost& host){
    const auto& graph=graph_of(host);              // 0015ddb0 without a collection returns the default node 004ee130 (not hosted)
    const std::int32_t current=c.path.current_50;
    (void)graph.node(current);                     // 00159e84 0015ddb0 (the original has no range check)
    // 00159e98 0015ff90(path, +50): 0015ffd0 goes straight to its tail
    // 00160c40 for the node +50 (its route-buffer search is unreachable),
    // then 00160c40 again.
    hold_npc_path_node(c.path,graph,current);
    hold_npc_path_node(c.path,graph,current);
    NpcAiSend s;s.kind=NpcAiSend::Kind::teleport;s.transport=1;   // 0015a02c..0015a038: +20 = 1, a2 = 1
    s.mode_24=2;                                                  // 00159fe0 mode place (0015a430 false: the mode-3 copy 0015a400 is dead here)
    s.matrix=npc_relocation_frame(c.path.direction_10);
    s.matrix[3]=graph.position(current);                          // 00159f9c 0015c2c0; 0014b1e0 forces w = 1
    send_actor(host,s);                                           // 003c9420 -> 001a92a0 -> 001bc4b0
    c.stuck_1c4=0.0F;                                             // 0015a044
    c.stuck_1c0=999999.0F;                                        // 0015a054 (0x497423f0)
}
}

bool request_npc_ai_mode(NpcAiComponent& c,std::int32_t mode,std::int32_t priority){
    if(priority<c.priority_4c)return false;
    c.requested_48=mode;c.pending_50=1;c.priority_4c=priority;
    return true;
}
void request_npc_weapon_draw(NpcAiComponent& c,std::uint8_t value,std::int32_t priority){
    if(priority<c.draw_priority_120)return;
    c.draw_13d=value;c.draw_priority_120=priority;
}
void npc_teleport_to_node(NpcAiComponent& c,std::int32_t node,NpcAiHost& host){
    const auto& graph=graph_of(host);
    NpcAiSend s;s.kind=NpcAiSend::Kind::teleport;s.transport=1;s.mode_24=0;   // 0014b00c..0014b020: key 0055d4e0, +24 = 0
    const RootVector p=graph.position(node);                                     // 0014b000 0015c2c0
    s.matrix={RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},
              RootVector{p[0],p[1],p[2],1.0F}};                                  // 0014b210 / 0014b1e0 / 0014b160
    send_actor(host,s);                                                          // 0014b058 003c9420(msg, +30, 1)
    relocate_npc_path_follower(c.path,graph,node,path_host(host));               // 0014b07c 0015ff90(+1b0, node +78)
    npc_resume_ai_mode(c,host);                                                  // 0014b08c virtual +e0 = 0014b570
    c.movement_priority_a0=0;                                                    // 0014b0a4
    set_npc_movement_state(c,0,0,host);                                          // 0014b0a0 0014d510(c, 0, 0)
}
void npc_port_relocate_path_follower(NpcAiComponent& c,std::int32_t node,NpcAiHost& host){
    graph_of(host).node(node);
    relocate_npc_path_follower(c.path,graph_of(host),node,path_host(host));     // 0014b07c 0015ff90(+1b0, node +78)
    npc_port_drop_path_segment(c);
}
void npc_port_drop_path_segment(NpcAiComponent& c){
    // 0015ff90 moves +50 only to a node of the held route buffer; a node off it leaves the segment of the old route
    // (+50, its target +00 and direction +10) in place, and the stuck check (00159cc0) then measures the actor's
    // progress toward that far target and, after 2 s, relocates the actor onto the old route's node (00159e84).
    if(c.path.current_50!=c.path.hold_4c)c.path.current_50=-1;
    c.stuck_1c0=999999.0F;c.stuck_1c4=0.0F;                                      // as 0015a080..0015a08c
}
std::array<RootVector,4> npc_relocation_frame(const RootVector& direction){
    RootVector d=direction;d[2]=0.0F;                             // 00159ed8..00159edc: lq +10, sp+298 = 0
    const RootVector up{0.0F,0.0F,1.0F,0.0F};                    // 00159ee0..00159ef0
    const auto side=vu_cross(d,up);                               // 00159eec 001320f0
    if(!zero_xyz(side)){                                          // 00159ef4 001320b0
        const auto forward=vu_normalize(d);                       // 00159f04 00132060 (W = the direction's W)
        const auto x=vu_normalize(side);                          // 00159f0c 00132030, 00132020
        return {x,forward,vu_cross(x,forward),RootVector{0.0F,0.0F,0.0F,1.0F}};   // 00159f28 001320f0, 00132000, 00131f00
    }
    // 00159f70: with z = 0 the side is zero only for a zero direction.
    if(!zero_xyz(d))throw std::runtime_error("NPC AI: the stuck relocation frame 00131c70 (unreachable with z = 0)");
    return {RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},
            RootVector{0.0F,0.0F,0.0F,1.0F}};                     // 00159f90 00131c40
}
void npc_stuck_check(NpcAiComponent& c,NpcAiHost& host){
    auto& a=host.actor;
    bool check=true;
    if(c.contact_158)check=a.physical_present()&&!(a.physical_2c8()<0.5F);                      // 00159cc0..00159d10
    if(!(check&&c.state_13c==1&&!(c.travel_speed_60<=1e-4F)&&!a.killed_33a5()&&a.mode_320c()!=0x1a&&a.mode_320c()!=0x10)){
        c.stuck_1c0=999999.0F;c.stuck_1c4=0;                                                        // 0015a080..0015a08c
        return;
    }
    const auto& d=c.path.direction_10;
    const float ax=std::fabs(d[0]),ay=std::fabs(d[1]),az=std::fabs(d[2]);
    const RootVector p=a.world_matrix()[3];                                                          // 00159da4 virtual +f4
    RootVector delta;for(int i=0;i<4;++i)delta[i]=c.path.target_00[i]-p[i];
    float progress;
    if(!(ax<=ay))progress=!(ax<=az)?std::fabs(delta[0]):std::fabs(delta[2]);
    else progress=!(ay<=az)?std::fabs(delta[1]):std::fabs(delta[2]);
    if(!(c.stuck_1c0<=progress)){c.stuck_1c0=progress;c.stuck_1c4=0;return;}                       // 00159e38..00159e50
    if(!(c.stuck_1c4<=2.0F)&&c.path.current_50>=0)npc_stuck_relocation(c,host);                    // 00159e58..0015a064
    c.stuck_1c4+=c.since_think_1bc;                                                                  // 0015a068 (the relocation falls through)
}
void request_npc_fire_state(NpcAiComponent& c,std::uint8_t state,std::int32_t priority){
    if(priority<c.fire_priority_118)return;
    c.byte_13e=state;c.fire_priority_118=priority;
}
std::uint8_t npc_speed_class(const NpcAiComponent& c){
    const float v=c.travel_speed_60;
    if(v<std::bit_cast<float>(0x3a03126fU))return 0;
    if(v<0.25F)return 1;
    if(v<=0.75F)return 2;
    return 3;
}
void npc_request_action(NpcAiComponent& c,std::uint8_t action,std::int32_t priority,float value,NpcAiHost& host){
    auto act=[&](std::int32_t code){send_actor(host,actor_action(code,0,0));};
    switch(action){
    case 1:
        if(priority<c.field_9c)return;
        c.field_9c=priority;act(1);c.flags_124|=1U;return;
    case 9:
        if(priority<c.field_9c)return;
        c.field_9c=priority;act(2);c.flags_124&=~1U;return;
    case 2:act(8);c.flags_124|=2U;return;
    case 4:act(9);c.flags_124|=4U;return;
    case 0xa:act(0xa);c.flags_124&=~4U;return;
    case 8:
        if(!(c.flags_124&8U))act(0x40);
        c.flags_124|=8U;c.timer_128=value;return;
    case 0xb:
        if(c.flags_124&8U){act(0x41);c.flags_124&=~8U;}
        return;
    default:return;
    }
}
// 00151a90: the animation controller message (+24 the code) to a living AI
// component. Codes 6 / 8 / 9 queue an actor action (0055d480, event byte 1 but
// 0x1a keeps the default 5, kind 0, transport 0) by the path-link action +114
// and return 1; every other code (the stance codes 2..5 among them) returns 0.
bool npc_controller_message(NpcAiComponent& c,std::int32_t code,NpcAiHost& host){
    auto act=[&](std::int32_t action){send_actor(host,actor_action(action,0,0));};
    switch(code){
    case 6:act(c.action_114==4?0x11:0x13);return true;                      // 00151ad0
    case 9:if(c.action_114==4)act(0x11);return true;                         // 00151b40
    case 8:                                                                  // 00151bb0
        if(c.action_114==4)act(0x0f);
        else if(c.action_114==0xb)act(0x10);
        else if(c.action_114==3){
            if(!host.actor.mode_320c)throw std::runtime_error("NPC AI host: mode_320c");
            if(host.actor.mode_320c()!=0x10)act(0x1a);                        // actor (+30) +320c
        }
        return true;
    default:return false;                                                    // 00151d00
    }
}
void npc_select_item(NpcAiComponent& c,std::int32_t item,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random){
    if(item>=0)inventory.equip_item(item);                                    // 001439b0
    else inventory.equip_first();                                            // 00143c50
    npc_reroll_burst(c,inventory,actor_id,random);
}
void npc_reroll_burst(NpcAiComponent& c,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random){
    if(inventory.inventory().current_84!=weapon_no_slot){                     // 001b16c0
        if(!random)throw std::runtime_error("NPC AI: the burst reroll (0014bae0 / 0014bbd0) for an armed NPC needs the game RNG");
        c.ammo_a4=static_cast<std::int32_t>(3U+(actor_id&7U));
        if((random()&1U)!=0)c.ammo_a4=c.ammo_a4+1;
        const std::int32_t size=inventory.clip_size(weapon_no_slot);
        if(size<c.ammo_a4){const std::int32_t half=size/2;c.ammo_a4=half>0?half:1;}
    }else c.ammo_a4=0;
    c.field_a8=0;
}
void npc_store_weapon(NpcAiComponent& c,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random){
    inventory.holster_current();                                             // 0014bbec: 00143b60(actor +3524)
    npc_reroll_burst(c,inventory,actor_id,random);                           // 0014bbf4..0014bc90
}
void npc_remove_weapon(NpcAiComponent& c,std::int32_t item,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random){
    inventory.remove(inventory.slot_of_item(item),false);                    // 0014b7b0..0014b7c8: 00143d20, 00142450
    npc_reroll_burst(c,inventory,actor_id,random);                           // 0014b7d0..0014b868
}
void npc_resume_ai_aiming(NpcAiComponent& c){
    c.draw_priority_120=0;c.draw_13d=1;c.subtarget_11c=-1;                   // 0014a130
}
void npc_set_aim_subtarget(NpcAiComponent& c,std::int8_t subtarget){
    c.subtarget_11c=subtarget;                                               // 0014a180: dsll32 / dsra32 of the lb byte
}
void npc_resume_ai_travel_speed(NpcAiComponent& c){
    c.speed_priority_64=0;                                                   // 00149da0: sw zero, +64
}
void set_npc_movement_state(NpcAiComponent& c,std::int8_t state,std::int32_t priority,NpcAiHost& host){
    if(priority<c.movement_priority_a0)return;
    c.movement_priority_a0=priority;
    if(static_cast<std::uint8_t>(state)==static_cast<std::uint8_t>(c.state_13c))return;
    c.state_13c=state;
    if(state==0){
        host.send(component_message(NpcAiMessage::Kind::ai,5,-1,0));         // AI event 5 (stopped), transport 0
        if(c.draw_13d==0)request_npc_weapon_draw(c,1,20);                    // virtual +bc
        return;
    }
    if(bit(host.actor.flags_3204(),3))npc_request_action(c,9,0,0.0F,host);  // 0014d5e0
    c.stuck_1c0=999999.0F;c.stuck_1c4=0;
}
float npc_travel_speed(std::uint8_t speed_enum){
    static constexpr float table[4]={0.0F,0.1F,0.5F,0.9F};                    // 00489dd0
    if(speed_enum>3)throw std::runtime_error("NPC AI: travel speed enum outside the table 00489dd0");
    return table[speed_enum];
}
void set_npc_travel_speed(NpcAiComponent& c,float speed){
    if(!(c.speed_priority_64<101))return;
    c.travel_speed_60=speed;c.speed_priority_64=100;c.flags_141&=static_cast<std::uint8_t>(~0x40U);
}
void resume_npc_movement(NpcAiComponent& c,NpcAiHost& host){
    if(bit(c.path.flags_60,3))return;
    c.movement_priority_a0=0;
    if(c.state_13c==0)set_npc_movement_state(c,2,0,host);
    c.stop_timer_7c=0;
}
// 00156f90 (Patrol); mode 1 is 00156d00 (npc_alert.hpp).
bool dispatch_npc_ai_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(c.mode_44==1)return npc_alert_mode(c,m,host);
    if(c.mode_44==7)return npc_script_control_mode(c,m,host);               // 00154660
    if(c.mode_44==4)return npc_attack_mode(c,m,host);                       // 00155350
    if(c.mode_44==3)return npc_attack_from_cover_mode(c,m,host);            // 00155ba0 (N5c)
    if(c.mode_44==2)return npc_take_cover_mode(c,m,host);                   // 001564c0 (N5c)
    if(c.mode_44==5)return npc_search_mode(c,m,host);                       // 00154dd0 (N5c, static)
    if(c.mode_44==9)return npc_follow_mode(c,m,host);                       // 00153870 (Checkpoint 132zf12)
    if(c.mode_44==8)return npc_carry_body_mode(c,m,host);                   // 00153150 (Checkpoint 132uk5)
    if(c.mode_44!=0){
        // Jump table 0049b6a0: 6 = 001548f0, 10 = 001535e0 (8 = 00153150 and 9 = 00153870 are native).
        static constexpr const char* handlers[]{"00156f90","00156d00","001564c0","00155ba0","00155350","00154dd0",
            "001548f0","00154660","00153150","00153870","001535e0"};
        const auto mode=static_cast<std::size_t>(c.mode_44);
        throw std::runtime_error("NPC AI: the handler of mode "+std::to_string(c.mode_44)+" (jump table 0049b6a0"+
            (mode<std::size(handlers)?std::string(" -> ")+handlers[mode]:std::string())+") is not native");
    }
    if(m.kind!=NpcAiMessage::Kind::ai)return false;
    switch(m.event){
    case 4:                                                                  // target visible and clear changed
        if(m.value!=0)request_npc_ai_mode(c,1,0);
        return true;
    case 7:                                                                  // awareness notice
        request_npc_ai_mode(c,1,0);
        npc_travel_speed_for_purpose(c,1,host);                              // virtual +d4(1, 20)
        return false;
    case 0:request_npc_weapon_draw(c,0,10);return true;
    case 1:request_npc_weapon_draw(c,1,10);return true;
    default:return false;
    }
}
bool dispatch_npc_ai(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    need(host.actor.eligible,"eligible");
    if(!host.actor.eligible())return false;
    const bool handled=dispatch_npc_ai_mode(c,m,host);
    const auto life=host.actor.life_state();
    if(m.kind==NpcAiMessage::Kind::ai){
        if(life==3)return false;                                             // 001528a0: neither key nor CAS
        if(m.event==7){npc_dispatch_awareness_event(c,m,host);return false;}   // 00151df4
        if(m.event==4){npc_release_look_entry(c,host);return false;}            // 00151e68
        if(m.event==1){
            c.mode_44=c.requested_48;                                        // 0016e160
            host.send(component_message(NpcAiMessage::Kind::mode_change,static_cast<std::uint8_t>(c.mode_44),-1,0));
        }
        return false;
    }
    if(m.kind==NpcAiMessage::Kind::controller){                              // 00152aa4: key 0055a1d8
        if(life==3)return false;                                             // 003cb210 returns 0
        return npc_controller_message(c,m.event,host);                       // 00151a90
    }
    if(m.kind==NpcAiMessage::Kind::los_result){                              // 001528e0: key 005435a8
        if(life==3)return false;
        npc_dispatch_los_result(c,static_cast<std::uint8_t>(m.value&0xff),host);
        return false;
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;                        // 003cb210
    const auto& graph=graph_of(host);
    auto& pf=c.path;
    switch(m.event){
    case 5:{
        const auto node2=pf.current_50>=0?pf.current_50:-1;
        if(c.state_13c==0)pf.hold_4c=m.value;
        NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_waypoint_reached;
        graph.node(m.value);s.node=m.value;s.node2=node2;
        host.send(s);
        c.stuck_1c0=999999.0F;c.stuck_1c4=0;
        return false;}
    case 2:                                                                  // 00152108
        if(handled)return false;
        npc_route_ready(c,host);
        return false;
    case 7:{
        if(handled)return false;
        if(c.speed_priority_64==20)c.speed_priority_64=0;
        c.flags_124&=~2U;
        const auto type=m.value;
        // Types 5 / 0xd and 0xc send the running jump (0x15 / 0x16) below;
        // every other value (0, 7, 9, 10, -1 for nodes without a link, ...)
        // only sets +114, and 1 / 2 the patrol speeds.
        if(c.action_114!=type){                                              // 001521a0
            if(c.action_114==6){
                npc_request_action(c,9,10,0.0F,host);                        // stand up
                if(c.field_9c<11)c.field_9c=0;
            }else if(c.action_114==8)npc_request_action(c,0xa,0,0.0F,host);  // stop crawling
            else if(c.action_114==3)send_actor(host,actor_action(0x1e,0,0)); // +20 = 1, transport 0
            c.action_114=type;
        }
        if(type==6){npc_request_action(c,1,10,0.0F,host);return false;}      // 00152290: crouch
        if(type==5||type==0xd||type==0xc){
            // 0015232c / 0015247c (5 / 0xd): actor action 0x15 kind 4 (CAS
            // action, vtable 004e0140) with the follower's point +0 as +30,
            // xyzw as stored. 001523bc (0xc): 0x16 with w = 00121ab8(-x, y)
            // of the follower's direction +10. +20 = 1, transport 0.
            NpcAiSend s=actor_action(type==0xc?0x16:0x15,4,0);
            s.point=pf.target_00;
            if(type==0xc){
                // neg.s flushes a denormal x to a signed zero first (EE goldens).
                const float x=pf.direction_10[0];
                const float nx=std::fpclassify(x)==FP_SUBNORMAL?(std::signbit(x)?0.0F:-0.0F):-x;
                s.point[3]=game_atan2f(nx,pf.direction_10[1]);
            }
            send_actor(host,s);
            return false;
        }
        if(type==8){                                                         // 001522bc: crawl
            npc_request_action(c,4,0,0.0F,host);
            const float speed=npc_travel_speed(3);                            // virtual +148(3)
            if(c.speed_priority_64<21){
                c.travel_speed_60=speed;c.speed_priority_64=20;
                c.flags_141=static_cast<std::uint8_t>((c.flags_141&~0x40U)|0x40U);
            }
            return false;
        }
        if(type==4||type==0xb||type==3){set_npc_movement_state(c,2,20,host);return false;}
        if(!bit(pf.flags_60,0))return false;
        if(type==2||type==1){
            const float speed=npc_travel_speed(type==2?3:1);                  // virtual +148
            if(c.speed_priority_64<21){
                c.travel_speed_60=speed;c.speed_priority_64=20;
                c.flags_141=static_cast<std::uint8_t>((c.flags_141&~0x40U)|0x40U);
            }
        }
        return false;}
    case 6:{
        const auto node=m.value;
        set_npc_movement_state(c,0,20,host);
        c.action_114=0;
        if(c.speed_priority_64<51)c.speed_priority_64=0;
        if(node>=0){pf.hold_4c=node;graph.node(node);}
        NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_destination_reached;
        s.node=node>=0?node:-1;s.node2=-1;
        host.send(s);
        return false;}
    case 0:                                                                  // 00152788: the node search's result (N5c)
        if(handled)return false;
        npc_dispatch_search_result(c,m,host);
        return false;
    case 4:
        if(handled)return false;
        set_npc_movement_state(c,2,20,host);
        return false;
    default:return false;
    }
}
void update_npc_ai_base(NpcAiComponent& c,float dt,NpcAiHost& host){
    if(c.pending_50){
        c.pending_50=0;
        const auto saved=host.sender_f20;
        host.sender_f20=dt;                                                  // 0016f2ec mov.s f20, f12
        for(std::uint8_t event:{std::uint8_t{1},std::uint8_t{0}}){              // exit, then enter (transport 1)
            const auto s=component_message(NpcAiMessage::Kind::ai,event,-1,1);
            host.send(s);
            dispatch_npc_ai(c,s.message,host);
        }
        host.sender_f20=saved;
    }
    if(!(c.script_timer_58<0.0F)){
        c.script_timer_58-=dt;
        if(c.script_timer_58<0.0F){
            NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_time_elapsed;  // to the component
            host.send(s);
        }
    }
}
void ground_npc_actor(NpcAiHost& host,bool a1,bool a2,float height){
    auto& a=host.actor;
    if(!a.physical_present()||a.physical_mode_20()!=0)return;
    bool s0=a.physical_e8();                                                 // virtual +110 (0013e020)
    if(!s0&&dying_or_dead(a.life_state())){
        if(!a.dying_ground_probe)throw std::runtime_error("NPC AI: 001b0a50 for a dying or dead actor needs the ground probe (00148290)");
        s0=a.dying_ground_probe();
    }
    if(s0){
        if(!a1||!a.physical_dc()||dying_or_dead(a.life_state()))return;
        // The AI component's virtual +54 is 0: no return for a player.
    }
    bool stop=!a1;
    if(a1){
        if(a.roots_unparented()){
            const RootVector p=a.world_matrix()[3];                          // virtual +e8 (00184a80)
            if(!a2||p[2]<height-0.5F){                                       // 001b0d6c c.olt.s
                // 001b0d80 / 001b0e08: a Teleport Message (key 0055d4e0, vtable 004d9360, +24 mode 0) with rows
                // 0..2 from the identity 0055c9f0 (0014b210, 0014b160) and row 3 the +e8 translation with z =
                // the height - 0.6 (a2) or the height, w = 1 (0014b1e0), sent with transport 1 (003c9420).
                NpcAiSend s;s.kind=NpcAiSend::Kind::teleport;s.transport=1;s.mode_24=0;
                s.matrix={RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},
                          RootVector{p[0],p[1],a2?height-0.6F:height,1.0F}};
                send_actor(host,s);
            }
        }else stop=true;
    }
    if(stop){
        a.stop_physical_motion();                                            // 001b0e88
        if(bit(a.flags_3210(),10)){                                          // 001b0ec0..001b0ed8: 001a0b30(actor, 0)
            if(!a.land_reset)throw std::runtime_error("NPC AI: 001b0a50 landing 001a0b30(actor, 0) is not hosted");
            a.land_reset();
        }
    }
    a.set_flags_3038(1);
    if(a.falling_dead_1d_1e()){                                              // 001b0ef0: 0019e670(actor, 1)
        if(!a.land_dead)throw std::runtime_error("NPC AI: 0019e670 dead landing for an AI controller is not hosted");
        a.land_dead();
    }
}
std::optional<RootVector> npc_facial_facing(const NpcAiComponent& c,const RootVector& translation,
    const std::function<std::uint64_t(std::uint64_t object,std::int32_t id)>& resolve,
    const std::function<RootVector(std::uint64_t object)>& eye){
    if(c.look_entry_2.active==0)return std::nullopt;                          // 0014c724: +a8(2)
    const auto object=resolve(c.look_entry_2.object,c.look_entry_2.id);      // 0014c72c..0014c78c, 0014d350
    if(object==0)throw std::runtime_error("NPC AI: 0014bff0 with look entry 2 unresolved (0014c790 reaches 0014c990 with the facing sp+610 unwritten)");
    const RootVector e=eye(object);                                            // 0014c7b0: virtual +c0(sp+210, object, -1)
    return RootVector{e[0]-translation[0],e[1]-translation[1],e[2]-translation[2],e[3]-translation[3]};   // 0014c7cc vsub.xyzw
}

void npc_ledge_fall_sound_tail(NpcAiComponent& c,NpcAiHost& host){
    if(!c.byte_1d6||!bit(host.actor.flags_3204(),7))return;                 // 0014d1a0..0014d1bc
    c.byte_1d6=0;
    NpcAiSend s;s.kind=NpcAiSend::Kind::sound;s.transport=0;s.key=npc_ledge_fall_sound;   // 0014d1cc..0014d208
    host.send(s);
}
void run_npc_locomotion(NpcAiComponent& c,NpcAiHost& host){
    auto& a=host.actor;auto& pf=c.path;const auto& graph=graph_of(host);
    const auto m=a.world_matrix();const RootVector p=m[3];
    const std::uint8_t old_contact=c.contact_158;
    c.contact_158=1;
    // 0014c058..0014c174: actor +3204 bit 7, cleared unless the current and
    // the hold (or previous) node's association objects are eligible. The
    // checks have no side effects, so they are skipped once s2 is clear.
    bool s2=true;
    if(c.action_114!=7){
        s2=bit(a.flags_3204(),7);
        if(s2&&pf.current_50>=0&&!association_eligible(graph.node(pf.current_50),host))s2=false;
        if(const auto ref=npc_path_reference_node(pf);s2&&ref&&!association_eligible(graph.node(*ref),host))s2=false;
    }
    const float height=npc_path_height(pf,graph,old_contact!=0,s2);         // 0015fce0
    if(bit(a.flags_3210(),22)){
        // 0014c1a0..0014c2d4, a carried body (action 22): the multiplayer
        // search for its carrier's slot (5721c8) is skipped in solo, so
        // 0015fdd0(pf, -1): 00160c40(pf, -1), then +08 = +28 = the +f4 z.
        // The contact +158 stays 1 only while the actor's region
        // (0014d460 / 0014d450 / 0014d410: *(+2fe0) +50 / +54) has +2d and +2c
        // set and +2e clear.
        hold_npc_path_node(pf,graph,-1);
        set_npc_path_height(pf,p[2]);
        if(!a.region_2fe0)throw std::runtime_error("NPC AI: 0014bff0 for a carried body reads the actor's region (0014d410, NpcAiActor::region_2fe0)");
        const auto r=a.region_2fe0();
        if(!(r&&(*r)[1]!=0&&(*r)[0]!=0&&(*r)[2]==0))c.contact_158=0;
    }else if(old_contact==0||p[2]<=height){
        c.contact_158=a.physical_present()&&a.physical_e8()?1:0;              // virtual +f8 (0014d3d0)
        bool s3=false;
        if(c.state_13c==1&&!bit(pf.flags_60,3)&&!dying_or_dead(a.life_state()))s3=true;
        const bool off=s3?((a.physical_present()&&a.physical_dc())||c.contact_158==0):c.contact_158==0;
        if(off)ground_npc_actor(host,s3,true,height);
    }else if(a.life_state()==3){
        set_npc_path_height(pf,3.0F+a.world_matrix()[3][2]);                // 0015fdc0
    }
    if(old_contact!=c.contact_158)
        pf.flags_60=static_cast<std::uint8_t>((pf.flags_60&~0x4U)|(c.contact_158==0?0x4U:0U));
    if(host.network_contact)host.network_contact(c.contact_158,a.world_matrix()[3]);   // 0014c510..0014c5f8 (132ln5h13)
    const auto tail=[&]{npc_ledge_fall_sound_tail(c,host);};                 // 0014d1a0 after every branch
    const auto life=a.life_state();
    if(dying_or_dead(life)&&c.action_114!=7){tail();return;}
    auto ph=path_host(host);
    if(a.navigation_enabled_30ac())npc_path_arrival_input(pf,a.navigation_arrived_30ad());   // 00161370
    update_npc_path_follower(pf,graph,ph);                                     // 00160220
    // Virtual +d8 / +a4: the AI controller's 0014d3c0 (+60) / 0014d3b0 (+13c); the remote NPC controller's 002d3480
    // (+200) / 002d2bd0 (1 while +200 > 1e-6, 0x358637bd).
    const auto& remote=host.remote;
    float speed=remote?remote->speed_200:c.travel_speed_60;                  // virtual +d8
    std::int8_t state=remote?static_cast<std::int8_t>(remote->speed_200<=std::bit_cast<float>(0x358637bdU)?0:1):c.state_13c;   // virtual +a4
    const bool s0=bit(a.flags_3204(),5);
    if(speed==0.0F)state=0;
    if(state!=1)speed=0.0F;
    if(host.network_speed)host.network_speed(speed);                         // 0014c6e0.. (132ln5h13)
    RootVector dir{};
    // Virtual +a8(2): look entry 2 active (0014d390); the remote NPC controller's 002d2b50 answers 0.
    const auto facial=remote?std::nullopt:npc_facial_facing(c,p,
        [&host](std::uint64_t object,std::int32_t id){
            if(!host.sense)throw std::runtime_error("NPC AI: 0014bff0's look entry 2 resolves its object (0014d350) through the perception views (not hosted)");
            return npc_resolve_handle(*host.sense,object,id);
        },
        [&host](std::uint64_t object){
            if(!host.attack||!host.attack->eye_c0)throw std::runtime_error("NPC AI: 0014bff0's look entry 2 reads its object's eye (virtual +c0(-1), not hosted)");
            return host.attack->eye_c0(object,-1);
        });
    if(facial)dir=*facial;                                                     // 0014c7f0: on to 0014c990
    else if(s0||c.action_114==7)dir=pf.direction_10;
    else{
        // 0014c828..0014c98c, the aim facing: a raised weapon (both upper-body
        // states of the secondary component +3314 are firing states, 0049f790
        // +4c) faces a visible target (+b4); otherwise a held weapon whose
        // record +f0 is 1 (001437a0; 11 with nothing held) faces it while the
        // last known position is valid (+ac: perception +20 bit 4). The facing
        // is the last known position (+b0: perception +00) minus the
        // translation (vsub.xyzw), zero when its squared length is below 16.
        const auto f=c.perception_flags_20;
        // Virtual +b4 / +ac / +b0: the AI controller's perception +20 bits 0 and 2 / bit 4 / +00; the remote NPC
        // controller's 002d3ff0 / 002d4030 (the cNetHuman linked and its +0x20) / 002d2b60 (its +0x10..+0x18, w = 1).
        const bool visible_b4=remote?remote->aiming_20:(bit(f,0)&&bit(f,2));
        const bool known_ac=remote?remote->aiming_20:bit(f,4);
        bool aim=false;
        if(visible_b4){
            if(!a.secondary_3314)throw std::runtime_error("NPC AI: 0014bff0's aim facing reads the secondary component +3314 (NpcAiActor::secondary_3314)");
            const auto states=a.secondary_3314();
            aim=states&&secondary_state_allows_fire((*states)[0])&&secondary_state_allows_fire((*states)[1]);
        }
        if(!aim&&known_ac&&a.inventory_mode_84()!=6){                        // nothing held: hold class 11
            if(!a.held_class)throw std::runtime_error("NPC AI: 0014bff0's aim facing reads the held weapon's hold class (001437a0, NpcAiActor::held_class)");
            aim=a.held_class()==1;
        }
        if(aim){
            if(!remote&&!host.perception)throw std::runtime_error("NPC AI: 0014bff0's aim facing reads the perception's last known position (not hosted)");
            const auto& k=remote?remote->aim_point_10:host.perception->last_known_00;
            const RootVector d{k[0]-p[0],k[1]-p[1],k[2]-p[2],k[3]-p[3]};
            if(!(((d[0]*d[0]+d[1]*d[1])+d[2]*d[2])<16.0F))dir=d;
        }
    }
    // Otherwise the facing vector stays zero (0014c988).
    if(state==1){
        dir=zero_xyz(dir)?pf.direction_10:normalize_xyz(dir);
        if(c.action_114==9&&!(c.flags_124&2U)&&!s0)npc_link9_turn(c,pf.direction_10,a.world_matrix()[1],s0,host);   // 0014ca28..0014cb40
        send_navigation(host,pf.target_00,yaw_of(dir),speed);
        tail();return;
    }
    if(state==0){
        if(zero_xyz(dir)){
            dir=a.world_matrix()[1];                                         // +f4 matrix row 1
            // +1d0: a node object facing override when its +7c is the destination +5c.
            if(c.facing_node_1d0>=0&&graph.node(c.facing_node_1d0).parameter_7c==pf.destination_5c){
                const auto n=graph.position(c.facing_node_1d0);
                RootVector d;for(int i=0;i<4;++i)d[i]=n[i]-p[i];
                dir=normalize_xyz(d);
            }
        }else dir=normalize_xyz(dir);
        send_navigation(host,dir,yaw_of(dir),speed);
        tail();return;
    }
    // State 2: turn toward the path direction.
    const float path_yaw=yaw_of(pf.direction_10);
    const float facing_yaw=yaw_of(a.world_matrix()[1]);
    float d=path_yaw-facing_yaw;
    if(!(d<=std::bit_cast<float>(0x40490fdbU)))d-=std::bit_cast<float>(0x40c90fdbU);
    else if(d<std::bit_cast<float>(0xc0490fdbU))d+=std::bit_cast<float>(0x40c90fdbU);
    const auto t=c.action_114;
    const float threshold=(t==4||t==0xb||t==3||t==9)?25.0F:360.0F;
    const float limit=std::bit_cast<float>(0x3c8efa35U)*threshold;            // pi/180 * degrees
    if(std::fabs(d)<=limit){
        set_npc_movement_state(c,1,20,host);
        if(t==4)send_actor(host,actor_action(0x0f,0,1));
        else if(t==0xb)send_actor(host,actor_action(0x10,0,1));
        else if(t==3)send_actor(host,actor_action(0x1a,0,0));
    }
    send_navigation(host,pf.target_00,yaw_of(pf.direction_10),speed);
    tail();
}
void npc_link9_turn(NpcAiComponent& c,const RootVector& path_direction,const RootVector& facing_row,bool s0,NpcAiHost& host){
    if(c.action_114!=9||(c.flags_124&2U)||s0)return;
    float d=yaw_of(path_direction)-yaw_of(facing_row);
    if(!(d<=std::bit_cast<float>(0x40490fdbU)))d-=std::bit_cast<float>(0x40c90fdbU);
    else if(d<std::bit_cast<float>(0xc0490fdbU))d+=std::bit_cast<float>(0x40c90fdbU);
    if(!(std::fabs(d)<=std::bit_cast<float>(0x3edf66f3U)))return;
    npc_request_action(c,2,0,0.0F,host);                                     // 0014d900(c, 2, 0, 0)
    c.flags_124&=~2U;c.action_114=0;
}
NpcAiFrame update_npc_ai(NpcAiComponent& c,float dt,NpcAiSystem& system,NpcAiHost& host){
    NpcAiFrame out;
    if(!system.selected_frame||*system.selected_frame!=system.frame){
        // 004ea358 keeps its value when no candidate is found.
        float best=-1.0F;
        for(const auto& k:system.think_list){
            if(!k.eligible())continue;
            if(dying_or_dead(k.life_state())){                                // virtual +44 (00157a40)
                if(!k.alive_44)throw std::runtime_error("NPC AI: a dying or dead thinker needs 00157a40 (+164)");
                if(!k.alive_44())continue;
            }
            if(k.controller_88!=1)continue;                                   // virtual +88 (132ln5h13: the remote's is 2)
            if(!(k.component->since_think_1bc<=best)){system.thinker=k.component;best=k.component->since_think_1bc;}   // virtual +88 is 1
        }
        system.selected_frame=system.frame;
    }
    out.think=system.thinker==&c;
    if(c.stop_timer_7c>0.0F){
        c.stop_timer_7c-=dt;
        if(c.stop_timer_7c<=0.0F)resume_npc_movement(c,host);               // virtual +158
    }
    update_npc_ai_base(c,dt,host);
    if(c.timer_128>0.0F){
        c.timer_128-=dt;
        if(c.timer_128<=0.0F&&(c.flags_124&8U))npc_request_action(c,0xb,100,0.0F,host);   // 0015980c
    }
    run_npc_locomotion(c,host);
    // The attack stages (N5b) need the perception's target and flags.
    const bool attack=host.attack&&host.perception&&host.sense;
    if(attack)npc_update_aim(c,host);                                         // virtual +60 = 0022f5b0
    else out.skipped.push_back("0022f5b0 aim/weapon update (component +60)");
    if(dying_or_dead(host.actor.life_state())){
        // 0015a2f8: a removable corpse goes (actor +60(1)); otherwise the
        // firing gate runs, which can only fire for a dying NPC with
        // ForceWeaponFire pending (+130 > 0, the death burst).
        need(host.dead_removal_allowed,"dead_removal_allowed");
        if(host.dead_removal_allowed()){
            if(!host.remove_dead_actor)throw std::runtime_error("NPC AI host: remove_dead_actor");
            host.remove_dead_actor();                                           // actor virtual +60(1)
        }
        else if(attack)npc_firing_gate(c,system,dt,host);                    // 00150340
        else{
            if(c.force_count_130>0)throw std::runtime_error("NPC AI: the firing gate 00150340 with ForceWeaponFire pending (the death burst) needs the attack host (NpcAiHost::attack)");
            out.skipped.push_back("00150340 firing");
        }
        if(out.think)c.since_think_1bc=0;                                     // 0015a338
        c.since_think_1bc+=dt;
        if(!(c.timer_154<=0.0F))c.timer_154-=dt;
        return out;
    }
    if(out.think){
        if(host.perception){
            if(!host.sense)throw std::runtime_error("NPC AI host: the perception without its host view (sense)");
            std::vector<NpcPerceptionSend> sends;
            npc_perception_think(*host.perception,c,*host.sense,c.since_think_1bc,sends);   // 0016b580(+1ac, +1bc)
            npc_forward_perception_sends(sends,host);
            npc_snap_burst_check(c,host);
        }else{
            out.skipped.push_back("0016b580 perception update (think frame)");
            // Without the perception +20 bit 3 stays clear, so the snap-burst
            // check below 0016b580 does not pass its bit-3 test.
            if(bit(c.perception_flags_20,3))throw std::runtime_error("NPC AI: the think-frame snap-burst check without the perception (NpcAiHost::perception)");
        }
        npc_stuck_check(c,host);                                             // 00159cc0..0015a088
    }
    if(attack){
        npc_look_at_selection(c,dt,host);                                     // 0014e0c0
        (void)npc_weapon_posture(c,system,host);                              // 00150ca0
        npc_firing_gate(c,system,dt,host);                                    // 00150340
    }else{
        out.skipped.push_back("0014e0c0 look-at selection");
        out.skipped.push_back("00150ca0 weapon posture (actor actions 0x3a, 0x3f)");
        out.skipped.push_back("00150340 firing");
    }
    if(c.timer_e8>0){
        --c.timer_e8;
        if(c.timer_e8==0)host.send(component_message(NpcAiMessage::Kind::ai,3,-1,0));
    }
    dispatch_npc_ai_mode(c,{NpcAiMessage::Kind::ai,2,-1},host);              // the tick, straight to the mode handler
    if(out.think&&c.occupied_hotbox_1b4)throw std::runtime_error("NPC AI: the hotbox occupancy check (+1b4) is not native");
    if(out.think)c.since_think_1bc=0;
    c.since_think_1bc+=dt;
    if(!(c.timer_154<=0.0F))c.timer_154-=dt;
    return out;
}
}
namespace omega {
std::vector<std::string> activate_npc_ai(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host,const NpcAiActivationHost& activation){
    std::vector<std::string> skipped;
    if(!c.inactive_159)return skipped;
    auto& a=host.actor;
    const auto alive_44=[&]{                                                 // virtual +44 (00157a40)
        if(!dying_or_dead(a.life_state()))return true;
        if(!activation.alive_44)throw std::runtime_error("NPC AI: Activate of a dying or dead NPC needs 00157a40 (+164)");
        return activation.alive_44();
    };
    if(!a.eligible()){
        if(alive_44())activation.activate_actor();                           // actor +5c(1); Activate runs again from there
        return skipped;
    }
    if(alive_44())system.think_list.push_back({&c,a.eligible,a.life_state,activation.actor_id,activation.alive_44});   // 0015af50
    else{
        if(!activation.append_to_dead_list)throw std::runtime_error("NPC AI: Activate of a removable corpse needs the dead list 004ea370");
        activation.append_to_dead_list();                                    // 0014ab90(004ea370, &component)
    }
    npc_take_look_slot(c,system);                                            // 004ea1d0 / +1b8, 00158980
    c.byte_166=0;
    if(c.timer_e8<0)c.timer_e8=0;
    if(!c.quiet_5c){                                                          // 0016f1c0
        NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_activated;
        host.send(s);
    }
    if(activation.perception)npc_perception_reset(*activation.perception,system.far_distance,system.frame);   // 0016dfa0
    else skipped.push_back("perception reset 0016dfa0");
    auto ph=path_host(host);
    start_npc_path_follower(c.path,graph_of(host),ph,host.routes);           // 00160f60
    if(c.pending_node_138>=0){                                                // 00157f88..00157fbc
        graph_of(host).node(c.pending_node_138);                              // 0015ddb0
        npc_move_to_node(c,c.pending_node_138);                               // virtual +100 = 0014be50
        c.pending_node_138=-1;
    }
    const bool armed=a.inventory_mode_84()!=6;                               // 001b16c0
    if(armed&&(!activation.inventory||!activation.random))
        throw std::runtime_error("NPC AI: Activate of an armed NPC needs its inventory and the game RNG");
    WeaponInventoryOps* inv=activation.inventory;
    // The burst length (00157fd8, 0014ba3c, 0014bb2c): 3 + (id & 7) + (r & 1),
    // capped by the current weapon's clip size (001438e0(inv, 6)).
    auto burst=[&]{
        if(!(inv&&inv->inventory().current_84!=weapon_no_slot)){c.ammo_a4=0;return;}
        if(!activation.random)throw std::runtime_error("NPC AI: Activate of an armed NPC needs the game RNG");
        c.ammo_a4=static_cast<std::int32_t>(3U+(activation.actor_id&7U));
        if((activation.random()&1U)!=0)c.ammo_a4=c.ammo_a4+1;
        const std::int32_t size=inv->clip_size(weapon_no_slot);
        if(size<c.ammo_a4){const std::int32_t half=size/2;c.ammo_a4=half>0?half:1;}   // sra toward zero for negatives
    };
    if(armed)burst();else c.ammo_a4=0;
    c.field_a8=0;c.contact_158=1;c.since_think_1bc=0;c.flags_68=0;c.field_84=0;c.field_88=0;
    c.stuck_1c0=999999.0F;c.stuck_1c4=0;c.facing_node_1d0=-1;
    // +8c = 0 for a full-auto weapon (00143820), else 0.2 + 0.01 * (id & 15).
    if(armed&&inv->full_auto())c.fire_delay_8c=0.0F;
    else c.fire_delay_8c=std::bit_cast<float>(0x3e4ccccdU)+std::bit_cast<float>(0x3c23d70aU)*static_cast<float>(activation.actor_id&15U);
    if(c.flags_141&0x20U){
        // 00158150..001581dc, the first activation: bit 5 cleared, then the
        // held item through +1a4 (0014bcb0), +1b0 (0014b9e0), +1a4 and
        // +1ac(+b0) (0014bae0). For an unarmed NPC (mode 6, no item) the
        // inventory calls change nothing (interpreter goldens).
        c.flags_141&=static_cast<std::uint8_t>(~0x20U);
        auto held=[&]()->std::int32_t{
            if(!inv)return -1;
            const auto cur=inv->inventory().current_84;
            return cur==weapon_no_slot?-1:inv->inventory().slots[cur].item;
        };
        c.held_item_b0=held();                                               // +1a4 = 0014bcb0
        if(inv){                                                             // +1b0 = 0014b9e0
            inv->cycle(true);
            if(inv->inventory().current_84==weapon_no_slot)inv->cycle(true);
        }
        burst();c.field_a8=0;
        c.held_item_b4=held();                                               // +1a4
        if(inv){                                                             // +1ac(+b0) = 0014bae0
            if(c.held_item_b0>=0)inv->equip_item(c.held_item_b0);
            else inv->equip_first();
        }
        burst();c.field_a8=0;
    }
    c.inactive_159=0;
    return skipped;
}
std::uint8_t npc_ai_constructor_flags_141(std::uint8_t previous){
    return static_cast<std::uint8_t>((previous&0x0bU)|0x34U);                  // 00158f04..00158fd8
}
std::vector<std::string> deactivate_npc_ai(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host,const NpcAiDeactivationHost& deactivation){
    std::vector<std::string> skipped;
    if(c.inactive_159)return skipped;
    auto& a=host.actor;
    if(a.eligible()){
        deactivation.deactivate_actor();                                     // actor +60(1); 003ce790 calls +4c again
        return skipped;
    }
    // 00163c50 on the manager 004ed9b0: the actor's queued requests (only
    // made through NpcAiHost::routes).
    if(host.routes&&host.route_client)npc_route_release(*host.routes,host.route_client->id);
    release_npc_route_buffer(c.path);                                        // 00160e60
    if(!c.quiet_5d){                                                         // 0016f0e0
        NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_deactivated;
        host.send(s);
    }
    // 00138ab0 on 004ea370: the component leaves the dead list.
    for(auto i=system.dead_list.begin();i!=system.dead_list.end();){
        if(i->component==&c)i=system.dead_list.erase(i);
        else ++i;
    }
    auto& list=system.think_list;                                            // 0015ad70
    for(auto i=list.begin();i!=list.end();){
        if(i->component==&c)i=list.erase(i);
        else ++i;
    }
    npc_release_look_slot(c,system);                                         // +1b8: 0015b860, +14 = 0
    if(deactivation.generator_release)deactivation.generator_release();      // 00157c3c (132ln5h13)
    // 00157cb0: a dying or dead actor's 003ce850(actor, &d) (the closest player
    // and its eye distance into the caller's frame, which nothing reads).
    if(dying_or_dead(a.life_state()))skipped.push_back("003ce850 (answer unused)");
    c.inactive_159=1;
    return skipped;
}
std::vector<std::string> deactivate_npc_ai_anonymous(NpcAiHost& host,const NpcAiDeactivationHost& deactivation){
    std::vector<std::string> skipped{"per-player activation record 004ea1b0 (0015b220)"};
    if(host.actor.eligible())deactivation.deactivate_actor();                // actor +60(1)
    skipped.push_back("NetObjectMgr 0042d910");
    return skipped;
}
void check_npc_activation_radius(NpcAiComponent& c,const NpcAiSystem& system,NpcAiHost& host,const NpcAiRadiusHost& radius){
    if(!(c.flags_141&0x80U))return;
    if((((radius.actor_id&0xffffU)+system.frame)&7U)!=0)return;
    auto range=[&]{
        float r2=c.radius_squared_98;
        if(r2<std::bit_cast<float>(0x3727c5acU))r2=system.far_distance*system.far_distance;   // 0053834c squared
        return r2;
    };
    if(!host.actor.eligible()){
        if(!c.latch_1d5){
            if(const auto d2=radius.closest_player_distance_squared()){
                if(*d2<range()){radius.activate_actor();c.latch_1d5=1;}
            }
        }
        c.latch_1d4=0;
        return;
    }
    if(c.latch_1d4)return;
    const auto d2=radius.closest_player_distance_squared();
    if(!d2)throw std::runtime_error("NPC AI: 0015a440 without a player reads an unset distance");
    if(!(*d2<=std::bit_cast<float>(0x441c4000U)+range())){radius.deactivate_actor();c.latch_1d4=1;}   // r^2 + 625
    c.latch_1d5=0;
}
void activate_npc_by_radius(NpcAiComponent& c,float radius){
    if(radius<std::bit_cast<float>(0x3727c5acU))c.radius_squared_98=0;
    else c.radius_squared_98=radius*radius;
    c.flags_141|=0x80U;
}
void deactivate_npc_by_radius(NpcAiComponent& c){
    c.radius_squared_98=0;                                                   // 0014ae88: sw zero, +98
    c.flags_141&=static_cast<std::uint8_t>(~0x80U);
}
}
namespace omega {
void set_npc_team(NpcAiComponent& c,std::int32_t team){c.team_54=team;}                        // 0016f490
std::int32_t npc_team(const NpcAiComponent& c){return c.team_54;}                               // 0015b800
void set_npc_skill(NpcAiComponent& c,float skill){c.skill_38=skill;}                            // 0022f2d0
float set_npc_firing_threshold(NpcAiComponent& c,float threshold){                              // 0014bce0
    const float previous=c.firing_threshold_78;
    c.firing_threshold_78=threshold;
    return previous;
}
void set_npc_max_active_count(NpcAiSystem& system,std::int32_t count){system.max_active_npc_count=count;}   // 0014b340
float npc_closest_player_eye_distance_squared(const RootVector& eye,std::span<const std::optional<RootVector>> players){
    if(players.size()!=4)throw std::runtime_error("NPC AI: 003ce850 scans four player slots");
    float best=std::bit_cast<float>(0x4cbebc20U);                                                 // 1e8
    bool found=false;
    for(const auto& player:players){
        if(!player)continue;
        const float dx=eye[0]-(*player)[0],dy=eye[1]-(*player)[1],dz=eye[2]-(*player)[2];      // vsub.xyzw
        const float x2=dx*dx,y2=dy*dy,z2=dz*dz;                                                  // vmul.xyz
        const float xy=x2+y2;                                                                     // vmulax.x / vmadday.x
        const float d2=xy+z2;                                                                     // vmaddz.x
        if(d2<best){best=d2;found=true;}                                                          // c.olt.s
    }
    if(!found)throw std::runtime_error("NPC AI: no player closer than 1e8 (003ce850 then returns 0040bf10's cached player, not hosted)");
    return best;
}
void npc_send_to_actor(NpcAiHost& host,const NpcAiSend& s){send_actor(host,s);}
NpcPathHost npc_path_host(NpcAiHost& host){return path_host(host);}
bool npc_node_association_eligible(const NpcNode& n,const NpcAiHost& host){return association_eligible(n,host);}
}
