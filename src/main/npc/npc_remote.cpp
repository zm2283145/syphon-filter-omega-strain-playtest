// 132ln5h13: the remote NPC controller 0x004de2a0 (npc_remote.hpp; research/NET_INLEVEL.md 4.23).
#include "npc_remote.hpp"
#include "npc_world.hpp"
#include "actor_fire.hpp"
#include "actor_object_action.hpp"
#include "npc_attack.hpp"
#include "npc_death_notice.hpp"
#include "npc_follow.hpp"
#include "npc_script_control.hpp"
#include "npc_route.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace omega {
namespace {
bool dying_or_dead(std::uint8_t life){return life==2||life==3;}
// PORT-ONLY diagnostic (134lt15): OMEGA_NPC_REMOTE_TRACE=N prints the remote controller's 002d5160 inputs every N
// updates per controller (0 / unset: off).
unsigned remote_trace_every(){
    static const unsigned n=[]{const char* e=std::getenv("OMEGA_NPC_REMOTE_TRACE");return e?static_cast<unsigned>(std::strtoul(e,nullptr,10)):0U;}();
    return n;
}
bool remote_trace_due(const void* who){
    const unsigned n=remote_trace_every();
    if(!n)return false;
    static std::map<const void*,unsigned> ticks;
    return (ticks[who]++%n)==0;
}
// 0015a430 is false for mode 0 and 2 (it tests +24 == 3), so 0014b1e0 / 0014b160 build the rows: the given three
// rows and row 3 = the position with w = 1.
NpcAiSend teleport_send(std::uint8_t mode,const std::array<RootVector,3>& rows,const RootVector& position){
    NpcAiSend s;s.kind=NpcAiSend::Kind::teleport;s.transport=1;s.mode_24=mode;   // key 0055d4e0, vtable 004d9360, +20 = 1, a2 = 1
    s.matrix={rows[0],rows[1],rows[2],RootVector{position[0],position[1],position[2],1.0F}};
    return s;
}
// 00132130 with the axis (0, 0, 1) and the block's heading +40: the yaw rows (w 0).
std::array<RootVector,3> heading_rows(float heading){
    return actor_object_node_rows(heading);
}
// 0014b210: the identity rows.
std::array<RootVector,3> identity_rows(){
    return {RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F}};
}
}

NpcRemoteInstall npc_remote_install(const NpcAiComponent* old,bool old_removable_164,std::int32_t start_node_3548,
    std::uint8_t new_flags_141){
    NpcRemoteInstall i;i.flags_141=new_flags_141;
    std::int32_t s1=start_node_3548;                                          // the base's +3548 (00158740)
    if(old){                                                                   // 002d60d0..: the old controller's +88 == 1
        i.flags_141=static_cast<std::uint8_t>((i.flags_141&0x7fU)|(old->flags_141&0x80U));
        i.radius_squared_98=old->radius_squared_98;
        i.state.node_210=-1;
        const std::int32_t v=old->path.hold_4c>=0?old->path.hold_4c:old->path.previous_54;
        if(v>=0)s1=v;
        else if(old->path.current_50>=0){s1=old->path.current_50;i.state.node_210=old->path.current_50;}
        i.flags_141=static_cast<std::uint8_t>((i.flags_141&~0x04U)|(old_removable_164?0x04U:0U));   // virtual +164 & 1
    }
    i.state.node_1e8=s1>=0?s1:-1;                                              // 0015c120 (identity; -1: null)
    i.state.node_208=s1;
    i.state.flags_214=static_cast<std::uint8_t>(i.state.flags_214&~0x07U);
    i.state.speed_200=0.0F;
    return i;
}
float npc_remote_ramp(float speed_200,std::uint8_t mode_1ee,float f1){
    const float step=std::bit_cast<float>(0x3c75c28fU);                       // 0.015
    if(mode_1ee==1){
        if(speed_200<f1)speed_200=f1;
        speed_200+=step;
        if(!(speed_200<=1.0F))speed_200=1.0F;
    }else if(mode_1ee==2){
        if(!(speed_200<f1))speed_200=f1;
        speed_200-=step;
        if(speed_200<0.0F)speed_200=0.0F;
    }else speed_200=f1;
    return speed_200;
}
float npc_remote_distance(const RootVector& a,const RootVector& b){
    const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];                          // vsub.xyzw
    const float x2=x*x,y2=y*y,z2=z*z;                                          // vmul.xyz
    return vu::sqrt((x2+y2)+z2);                                               // vmulax.w / vmadday.w / vmaddz.w, vsqrt
}
std::uint8_t npc_remote_route_mode(std::uint8_t order,std::uint8_t mode,const NpcPathFollower& pf,const NpcNodeGraph& graph,
    const RootVector& position_44){
    if(order==0)return mode;
    if(order==3)return 2;
    if(order==2)return 1;
    if(pf.current_50<0)return mode;
    const float d=npc_remote_distance(position_44,graph.position(pf.current_50));   // 0015ddb0 / 0015c2c0
    return d<=pf.remaining_40?1:2;                                             // c.ole.s
}
bool npc_remote_far_teleport(float distance,std::uint32_t mode_320c,std::uint32_t word_38,bool physical_present,float physical_2c8){
    if(distance<=4.0F)return false;                                            // c.ole.s 4.0
    if(mode_320c==0x10U)return false;
    if(word_38==0x80U)return true;
    if(!physical_present)return false;                                         // actor +3310
    return !(physical_2c8<0.5F);
}
NpcRemoteBurstStep npc_remote_burst_step(std::uint16_t count,float pause,float cooldown,float fire_delay,
    std::uint32_t actor_id,bool can_attack){
    NpcRemoteBurstStep out{count,pause,cooldown,false};
    if(count!=0){
        if(can_attack&&cooldown<=0.0F){
            out.count=static_cast<std::uint16_t>(count-1);
            out.cooldown=fire_delay;
            out.shoot=true;
        }
    }else if(pause<=0.0F){
        out.count=static_cast<std::uint16_t>((actor_id&7U)+3U);
        out.pause=4.0F*(std::bit_cast<float>(0x3e4ccccdU)
            +std::bit_cast<float>(0x3c23d70aU)*static_cast<float>(actor_id&15U));
    }
    return out;
}
RootVector npc_remote_no_fire_point(const NpcRemoteAim& aim,const std::array<RootVector,4>& actor_matrix,
    const RootVector& target_eye){
    if(!aim.point_valid_0)return target_eye;
    RootVector point=aim.point_10;
    const float x=point[0]-actor_matrix[3][0],y=point[1]-actor_matrix[3][1],z=point[2]-actor_matrix[3][2];
    if((x*x+y*y+z*z)<9.0F)
        for(std::size_t k=0;k<4;++k)point[k]+=actor_matrix[1][k]*20.0F;
    return point;
}
bool npc_remote_fire_gate(std::uint16_t maximum,std::uint16_t& current,bool& reset){
    if(reset){current=0;reset=false;}
    if(current>=maximum)return false;
    ++current;
    return true;
}

std::function<void()> NpcAiController::generator_release(){
    if(!generator_spawned()||!remote_link_.word_150||!remote_link_.object_live)return {};
    return [this]{                                                             // 00157c3c..00157ca8
        const std::uint32_t w=remote_link_.word_150();
        if(w!=0&&remote_link_.object_live(w)){
            if(!remote_link_.generator_message)
                throw std::runtime_error("NPC AI: Deactivate's generator release (the message 0x004a4a68 to the object of +150, 00157c3c) has no network link");
            remote_link_.generator_message(w);
        }
    };
}
void NpcAiController::fresh_component(){
    component_=NpcAiComponent{};
    construct_npc_path_follower(component_.path,100,start_node_);             // 00161840(pf, actor, 100, +3548)
    component_.flags_141=npc_ai_constructor_flags_141(0);
    queue_.clear();pending_.clear();
    perception_=NpcPerception{};
    port_route_event_.reset();
}
void NpcAiController::destroy_component(SoloActorSimulation& actor){
    // 00158550 with the actor's +2d cleared by 001ae450: 00157ad0's body for an active AI, else 00163c50; then
    // 0042df20 (PORT-ONLY: nothing to release) and the path follower's route buffer.
    if(!component_.inactive_159){
        if(remote_)remote_deactivate(actor,true);                              // 002d5fa0: +150 = -1, virtual +4c
        else{
            auto h=host(actor);
            h.actor.eligible=[]{return false;};
            NpcAiDeactivationHost d;
            d.deactivate_actor=[]{throw std::runtime_error("NPC AI: 00158550 re-entered the actor's deactivation");};
            d.generator_release=generator_release();
            const auto skipped=deactivate_npc_ai(component_,system_,h,d);
            frame_.skipped.insert(frame_.skipped.end(),skipped.begin(),skipped.end());
        }
    }
    if(system_.routes)npc_route_release(*system_.routes,route_client_->id);   // 00163c50
    release_npc_route_buffer(component_.path);
}
void NpcAiController::set_remote(SoloActorSimulation& actor){
    if(remote_)return;                                                         // 001b3d98: virtual +88 == 2
    if(!remote_link_.link||!remote_link_.motion||!remote_link_.write_position||!remote_link_.word_150||!remote_link_.object_live
       ||!remote_link_.object_by_key||!remote_link_.generator_message)
        throw std::runtime_error("NPC AI: the remote NPC controller 0x004de2a0 needs its network link (NpcRemoteLink)");
    route_actor_=&actor;
    // 002d6030 -> 00158740: the constructor's draw (00127dd0) for +6c, then the copies from the AI.
    const float r=game_int_to_float(static_cast<std::int32_t>(combat_.random?combat_.random():next_game_random(*random_state_)));
    const NpcRemoteInstall install=npc_remote_install(&component_,npc_corpse_removable_164(corpse_view(actor)),start_node_,
        npc_ai_constructor_flags_141(0));
    destroy_component(actor);                                                  // 001ae450(actor, 4, 0, new)
    fresh_component();
    aggressiveness_6c_=0.25F+0.5F*(r/2147483648.0F);
    component_.aggressiveness_6c=aggressiveness_6c_;
    component_.flags_141=install.flags_141;
    component_.radius_squared_98=install.radius_squared_98;
    remote_=install.state;
    actor.set_byte_14(0);                                                      // 132ln5h14c: another console owns it (+14 0)
    if(actor.eligible())remote_activate(actor);                                // virtual +48 = 002d40a0
}
void NpcAiController::make_fresh_ai(SoloActorSimulation& actor){
    if(!remote_)return;                                                        // 001b3d98: virtual +88 == 1
    route_actor_=&actor;
    // 00158800 / 001588d0 -> 00158bc0: the constructor's draw, then 001ae450 destroys the remote (002d5fa0).
    const float r=game_int_to_float(static_cast<std::int32_t>(combat_.random?combat_.random():next_game_random(*random_state_)));
    destroy_component(actor);
    remote_.reset();
    actor.set_byte_14(1);                                                      // 132ln5h14c: this console's again (+14 1)
    fresh_component();
    aggressiveness_6c_=0.25F+0.5F*(r/2147483648.0F);
    component_.aggressiveness_6c=aggressiveness_6c_;
    if(actor.eligible())activated(actor);                                      // virtual +48 = 00157df0
}
void NpcAiController::remote_deactivate(SoloActorSimulation& actor,bool destructor){
    auto& c=component_;
    if(c.inactive_159)return;
    if(!destructor&&actor.eligible()){actor.deactivate();return;}             // actor +60(1); 003ce790 calls +4c again
    auto& think=system_.think_list;                                            // 0015ad70
    for(auto i=think.begin();i!=think.end();){if(i->component==&c)i=think.erase(i);else ++i;}
    for(auto i=system_.dead_list.begin();i!=system_.dead_list.end();){        // 00138ab0(004ea370)
        if(i->component==&c)i=system_.dead_list.erase(i);else ++i;
    }
    // lhu +150. The destructor 002d5fa0 wrote -1 first; 0040c540 of that key finds no object (PORT-ONLY: taken).
    const std::uint32_t word=destructor?0xffffffffU:remote_link_.word_150();
    if((word&0xffffU)!=0){
        if(!destructor&&remote_link_.object_live(word))remote_link_.generator_message(word);   // 002d4554..
    }else if(alive_or_eligible(actor)){                                       // 002d4554..: virtual +44
        if(!remote_link_.local_release)
            throw std::runtime_error("NPC AI: the remote NPC's deactivation without +150 needs NpcRemoteLink::local_release (00434440 / 0042d910)");
        remote_link_.local_release((c.flags_141&0x80U)!=0);
    }
    c.inactive_159=1;
}
void NpcAiController::remote_activate(SoloActorSimulation& actor){
    auto& c=component_;auto& r=*remote_;
    if(!c.inactive_159)return;
    route_actor_=&actor;
    SoloActorSimulation* p=&actor;
    if(!actor.eligible()){
        if(alive_or_eligible(actor))actor.activate();                          // actor +5c(1)
        c.inactive_159=0;
        return;
    }
    auto h=host(actor);
    if(alive_or_eligible(actor))                                               // 0015af50 (virtual +88 answers 2)
        system_.think_list.push_back({&c,h.actor.eligible,h.actor.life_state,actor_id_,[this,p]{return alive_or_eligible(*p);},2});
    else system_.dead_list.push_back({&c,[this,p]{return corpse_view(*p);}});   // 0014ab90(004ea370)
    if(const auto m=remote_link_.motion()){                                    // 002d41a8..002d42f8
        npc_send_to_actor(h,teleport_send(2,heading_rows(m->heading_40),m->position_44));
        r.mode_1ee=0;
    }
    // 00143820(+3524): +8c = 0 for a full-auto weapon, else 0.2 + 0.01 (ID & 15).
    if(!combat_.inventory||!combat_.database)
        throw std::runtime_error("NPC AI: the remote NPC's Activate 002d40a0 reads the inventory +3524 (NpcCombatLink::inventory)");
    WeaponInventory* inv=combat_.inventory();
    if(!inv)throw std::runtime_error("NPC AI: the remote NPC's Activate 002d40a0 for an actor without a combat inventory");
    WeaponInventoryHost ih;ih.player=false;
    std::vector<WeaponInventoryEvent> events;
    WeaponInventoryOps ops(*inv,*combat_.database,ih,events);
    if(ops.full_auto())c.fire_delay_8c=0.0F;
    else c.fire_delay_8c=std::bit_cast<float>(0x3e4ccccdU)+std::bit_cast<float>(0x3c23d70aU)*static_cast<float>(actor_id_&15U);
    c.field_84=0;
    if(perception_link_.sense)npc_perception_reset(perception_,system_.far_distance,system_.frame);   // 0016dfa0
    else frame_.skipped.push_back("perception reset 0016dfa0");
    start_npc_path_follower(c.path,graph_,npc_path_host(h),system_.routes);  // 00160f60
    npc_reroll_burst(c,ops,actor_id_,h.random);                               // 0014fc60
    if(!events.empty())throw std::runtime_error("NPC AI: the remote NPC's Activate made inventory events");
    if(r.flags_214&0x02U){                                                     // 002d43f0..: 0015f9b0(+1b0, +208, ...)
        if(!system_.routes)throw std::runtime_error("NPC AI: the remote NPC's Activate route request 0015f9b0 needs the route manager");
        if(r.node_20c>=0)npc_route_request_15f9b0(*system_.routes,c.path,route_client_,r.node_208,r.node_20c,nullptr);
        else npc_route_request_15f9b0(*system_.routes,c.path,route_client_,r.node_208,-1,&r.point_1f0);
        r.flags_214=static_cast<std::uint8_t>(r.flags_214&~0x02U);
    }
    r.timer_204=0.0F;
    c.inactive_159=0;
}
void NpcAiController::remote_follow_002d2ca0(SoloActorSimulation& actor,std::optional<std::int32_t> target_id,float a,float b){
    if(!remote_)throw std::runtime_error("NPC AI: cNetNPC_NetFollow 002d2ca0 outside the remote controller");
    if(!target_id){remote_->flags_214=static_cast<std::uint8_t>(remote_->flags_214&~0x01U);return;}
    route_actor_=&actor;
    npc_follow(component_,target_id,a,b);                                      // 001488f0 (+88 = 2: no 0x32 send)
    auto h=host(actor);
    NpcAiMessage m;m.kind=NpcAiMessage::Kind::ai;m.event=0;m.value=-1;         // 004ee550, +24 0, +28 -1
    npc_follow_mode(component_,m,h);                                           // 00153870
    remote_->flags_214=static_cast<std::uint8_t>(remote_->flags_214|0x01U);
}
bool NpcAiController::remote_net_path_002d4ff0(SoloActorSimulation& actor,std::int32_t last_2c,std::int32_t first_30,bool point_34,
    const RootVector& point){
    if(!remote_)return false;
    auto& c=component_;auto& r=*remote_;
    route_actor_=&actor;
    r.node_20c=first_30;                                                       // +20c = +30
    if(!system_.routes)throw std::runtime_error("NPC AI: 002d4ff0's route request 0015f9b0 needs the route manager");
    if(!point_34){
        npc_route_request_15f9b0(*system_.routes,c.path,route_client_,last_2c,r.node_20c,nullptr);
    }else{
        r.point_1f0={point[0],point[1],point[2],1.0F};                         // 002d79a0: +38..+40, w = 1
        if(r.node_20c>=0)npc_route_request_15f9b0(*system_.routes,c.path,route_client_,last_2c,r.node_20c,&r.point_1f0);
        else{
            auto h=host(actor);
            npc_follow_point(c.path,graph_,r.point_1f0,npc_path_host(h));      // 0015fbd0(+1b0, +1f0, +5c)
        }
    }
    if(!actor.eligible())r.flags_214=static_cast<std::uint8_t>(r.flags_214|0x02U);   // b2D && b2C && !b2E
    return true;
}
bool NpcAiController::remote_node_in_active_terrain(SoloActorSimulation& actor){
    if(!remote_||remote_->node_1e8<0)return false;                             // b = +1e8 null: 0
    auto h=host(actor);
    return npc_node_association_eligible(graph_.node(remote_->node_1e8),h);  // c = node +60: +2d && +2c && !+2e
}
void NpcAiController::remote_relocate(NpcAiHost& h){
    relocate_npc_path_follower_15ffd0(component_.path,graph_,remote_->node_208,npc_path_host(h));   // 0015ffd0(+1b0, +208)
}
void NpcAiController::remote_dead(SoloActorSimulation& actor,NpcAiHost& h){
    auto& c=component_;auto& r=*remote_;
    if(!alive_or_eligible(actor)){                                            // virtual +44
        bool listed=false;                                                     // 0014ac.. over 004ea370
        for(const auto& d:system_.dead_list)if(d.component==&c){listed=true;break;}
        if(!listed){
            auto& think=system_.think_list;                                    // 0015ad70
            for(auto i=think.begin();i!=think.end();){if(i->component==&c)i=think.erase(i);else ++i;}
            SoloActorSimulation* p=&actor;
            system_.dead_list.push_back({&c,[this,p]{return corpse_view(*p);}});   // 0014ab90
        }
    }else if(const auto m=remote_link_.motion()){
        const float d=npc_remote_distance(m->position_44,h.actor.world_matrix()[3]);
        const std::uint32_t w38=combat_.word_38?combat_.word_38():0x80U;
        if(!(d<=4.0F)&&w38==0x80U&&((h.actor.flags_3210()>>22)&1U)==0){
            npc_send_to_actor(h,teleport_send(0,identity_rows(),m->position_44));
            remote_relocate(h);
            r.mode_1ee=0;
        }
    }
    bool removed=false;
    if(h.dead_removal_allowed())                                               // 00159020
        {h.remove_dead_actor();removed=true;}                                  // actor +60(1)
    if(!removed)run_npc_locomotion(c,h);                                       // 0014bff0
}
void NpcAiController::remote_update(SoloActorSimulation& actor,float dt){
    auto& c=component_;
    auto h=host(actor);
    const auto link=remote_link_.link();
    const std::uint8_t life=actor.life_state();
    auto present=[&](std::uint64_t o){
        if(!h.sense||!h.sense->object)throw std::runtime_error("NPC AI: the remote NPC's update reads its target's +2c..+2e (NpcPerceptionLink)");
        const auto v=h.sense->object(o);
        if(!v)throw std::runtime_error("NPC AI: the remote NPC's target is not an object the perception knows");
        return *v;
    };
    std::uint64_t target=0;                                                    // s2
    if(link){                                                                  // +1e0 && +1e4
        target=remote_link_.object_by_key(link->target_8);                     // 0040c540(+8)
        std::uint64_t aim=target;
        if(!link->aiming_20)aim=0;
        else if(target!=0){
            const auto v=present(target);
            if(!(v.root[1]&&v.root[0]&&!v.root[2]))aim=0;
        }
        if(dying_or_dead(life))aim=0;
        if(!combat_.aim)throw std::runtime_error("NPC AI: the remote NPC's aim 0022f5b0 needs the combat link (NpcCombatLink::aim)");
        combat_.aim(aim,script_active_weapon(),link->subtarget_c,c.skill_38);
    }
    h.remote=NpcAiHost::RemoteVirtuals{remote_->speed_200,link?link->aiming_20:false,
        link?RootVector{link->point_10[0],link->point_10[1],link->point_10[2],1.0F}:RootVector{0.0F,0.0F,0.0F,1.0F}};
    if(dying_or_dead(life)){remote_dead(actor,h);return;}                     // 002d5b58
    if(!combat_.inventory||!combat_.database)
        throw std::runtime_error("NPC AI: the remote NPC's update 002d5160 reads the inventory +3524 (NpcCombatLink::inventory)");
    if(const WeaponInventory* inv=combat_.inventory();inv&&weapon_can_attack(*inv,*combat_.database)){
        auto tick=[dt](std::int32_t& w){
            float f=std::bit_cast<float>(w);
            if(f>0.0F){f-=dt;w=std::bit_cast<std::int32_t>(f);}
        };
        tick(c.field_88);tick(c.field_84);
    }else if(!inv)throw std::runtime_error("NPC AI: the remote NPC's update 002d5160 for an actor without a combat inventory");
    // 002d4e70(s2)
    bool valid_target=false;
    NpcSenseObject target_view{};
    if(target!=0&&link){
        const auto v=present(target);
        valid_target=(v.root[1]&&v.root[0]&&!v.root[2])||v.class_4ea770;
        target_view=v;
    }
    auto action=[&](std::int32_t code,std::uint8_t kind,std::uint8_t transport){
        NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=transport;s.action=code;s.action_kind=kind;
        return s;
    };
    auto send_action=[&](const NpcAiSend& s){
        h.send(s);
        if(s.transport==1){
            if(!combat_.actor_action)throw std::runtime_error("NPC AI: an immediate remote combat action without NpcCombatLink::actor_action");
            combat_.actor_action(s);
        }
    };
    if(valid_target){
        if(!combat_.eye_c0_of)
            throw std::runtime_error("NPC AI: 002d48f0 / 002d4b30 needs the target's virtual +c0 (NpcCombatLink::eye_c0_of)");
        const RootVector eye=combat_.eye_c0_of(target,-1);
        if(link->firing_21){
            if(!remote_link_.fire_gate)
                throw std::runtime_error("NPC AI: 002c4f20 needs the cNetHuman fire-counter link");
            if(remote_link_.fire_gate()){                                      // 002d48f0
                auto aim=action(0x39,4,1);aim.point=eye;send_action(aim);
                WeaponInventory* inv=combat_.inventory();
                const auto burst=npc_remote_burst_step(remote_->burst_1ec,npc_burst_pause_84(c),npc_shot_cooldown_88(c),
                    c.fire_delay_8c,actor_id_,weapon_can_attack(*inv,*combat_.database));
                remote_->burst_1ec=burst.count;
                c.field_84=static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(burst.pause));
                c.field_88=static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(burst.cooldown));
                if(burst.shoot){
                    auto shot=action(0x3b,0xb,0);
                    shot.word_30=target_view.id;shot.word_34=-1;
                    send_action(shot);
                    WeaponInventoryHost ih;ih.player=false;
                    std::vector<WeaponInventoryEvent> events;
                    WeaponInventoryOps ops(*inv,*combat_.database,ih,events);
                    if(ops.clip(weapon_no_slot)==0)send_action(action(0x3c,0,0));
                }
                auto look=action(0x3e,5,1);look.word_2c=target_view.id;send_action(look);
            }else{                                                            // 002d4b30
                auto aim=action(0x39,4,1);
                aim.point=npc_remote_no_fire_point(*link,h.actor.world_matrix(),eye);
                send_action(aim);
                send_action(action(0x3f,0,0));
            }
        }else{
            for(const std::int32_t a:{0x3a,0x3f})send_action(action(a,0,0));
        }
    }else{
        for(const std::int32_t a:{0x3a,0x3f})send_action(action(a,0,0));
    }
    auto& r=*remote_;
    const auto motion=remote_link_.motion();
    if(motion){
        const float dist=npc_remote_distance(motion->position_44,h.actor.world_matrix()[3]);
        if(r.flags_214&0x04U){
            if(r.node_1e8>=0){
                npc_send_to_actor(h,teleport_send(2,heading_rows(motion->heading_40),graph_.position(r.node_1e8)));
            }
            remote_relocate(h);
            if(r.node_1e8<0)throw std::runtime_error("NPC AI: the remote NPC's node teleport without a node (002d55cc reads 0015c2c0 of +1e8 = 0)");
            remote_link_.write_position(graph_.position(r.node_1e8));          // the cNetHuman's +44..+4c
            r.mode_1ee=0;
            r.flags_214=static_cast<std::uint8_t>(r.flags_214&~0x04U);
        }else if(r.flags_214&0x01U){
            r.mode_1ee=1;
            // 002d53f8..: 0040c540(&+f4) (its answer is not read), then 00153870 with the AI message 004ee550 event 2
            // (the tick; +28 -1) on this controller (134lt7).
            NpcAiMessage fm;fm.kind=NpcAiMessage::Kind::ai;fm.event=2;fm.value=-1;
            npc_follow_mode(c,fm,h);
        }else{
            r.mode_1ee=0;
            const std::uint32_t w38=combat_.word_38?combat_.word_38():0x80U;
            // Read lazily, in the original's order: the distance, +320c, +38, then +3310 and its +2c8.
            bool far=false;
            if(!(dist<=4.0F)){
                const std::uint32_t m320c=h.actor.mode_320c();
                const bool physical=m320c!=0x10U&&w38!=0x80U&&h.actor.physical_present();
                far=npc_remote_far_teleport(dist,m320c,w38,physical,physical?h.actor.physical_2c8():0.0F);
            }
            if(far){
                npc_send_to_actor(h,teleport_send(0,identity_rows(),motion->position_44));
                remote_relocate(h);
                r.mode_1ee=0;
            }else if((c.path.flags_60&0x08U)==0){
                r.mode_1ee=npc_remote_route_mode(npc_route_order_1600a0(c.path,r.node_208),r.mode_1ee,c.path,graph_,motion->position_44);
            }
        }
    }
    // 002d5948: the speed ramp.
    const float f1=motion?motion->speed_3c:actor.navigation_target().speed_30e0;
    r.speed_200=npc_remote_ramp(r.speed_200,r.mode_1ee,f1);
    // 002d5a40: the time without contact.
    const std::uint32_t w150=remote_link_.word_150()&0xffffU;
    if(w150!=0||((h.actor.flags_3210()>>22)&1U)!=0){}
    else if(c.contact_158)r.timer_204=0.0F;
    else if(r.node_1e8>=0&&npc_node_association_eligible(graph_.node(r.node_1e8),h))r.timer_204=0.0F;
    else{
        bool lost=false;
        if(!(r.timer_204<=1.0F)){                                              // 002d5bxx: +204 > 1.0
            actor.deactivate();                                                // self->obj->f60(1)
            lost=true;                                                         // done = 1: no cNPC_UpdateMovement
            if(!remote_link_.terrain_activation)
                throw std::runtime_error("NPC AI: the remote NPC's lost contact needs NpcRemoteLink::terrain_activation (0042d790)");
            remote_link_.terrain_activation();
        }
        r.timer_204+=dt;
        if(lost){h.remote->speed_200=r.speed_200;return;}
    }
    h.remote->speed_200=r.speed_200;
    if(remote_trace_due(this)){
        const auto p=h.actor.world_matrix()[3];
        const std::uint32_t w38=combat_.word_38?combat_.word_38():0x80U;
        const bool phys=h.actor.physical_present();
        std::printf("NPC REMOTE TRACE id 0x%x pos (%.2f, %.2f, %.2f) +44 (%.2f, %.2f, %.2f) dist %.2f +38 0x%x +320c 0x%x"
            " +2c8 %s%.3f +1ee %u +214 0x%x +60 0x%x +208 %d +50 %d +40 %.2f tgt (%.2f, %.2f, %.2f) order %u +200 %.3f +3c %.3f"
            " +150 0x%x +158 %u +204 %.2f arrive %u\n",
            actor_id_,p[0],p[1],p[2],motion?motion->position_44[0]:0.0F,motion?motion->position_44[1]:0.0F,
            motion?motion->position_44[2]:0.0F,motion?npc_remote_distance(motion->position_44,p):-1.0F,w38,h.actor.mode_320c(),
            phys?"":"none ",phys?h.actor.physical_2c8():0.0F,unsigned(r.mode_1ee),unsigned(r.flags_214),unsigned(c.path.flags_60),
            r.node_208,c.path.current_50,c.path.remaining_40,c.path.target_00[0],c.path.target_00[1],c.path.target_00[2],
            unsigned(npc_route_order_1600a0(c.path,r.node_208)),r.speed_200,motion?motion->speed_3c:-1.0F,
            remote_link_.word_150(),unsigned(c.contact_158),r.timer_204,unsigned(actor.navigation_target().arrived_30ad));
        std::fflush(stdout);
    }
    run_npc_locomotion(c,h);                                                   // 0014bff0
}

// 134lt11: cNetworkTransferNpc (npc_remote.hpp).
namespace {
template<class T> void transfer_store(NpcTransferBytes& b,std::size_t off,const T& v){
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(b.data()+off,&v,sizeof v);
}
template<class T> void transfer_load(const NpcTransferBytes& b,std::size_t off,T& v){
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(&v,b.data()+off,sizeof v);
}
void transfer_zero(NpcTransferBytes& b,std::size_t off,std::size_t n){std::memset(b.data()+off,0,n);}
// The component's fields at their cNetworkTransferNpc offsets: cNpcTransfer (controller +60 at +000), cAiTransfer
// (controller +44 at +0f0), the cAITarget's flag byte +20 (component perception_flags_20 at +130) and
// cPathNavigatorTransfer (the follower +1b0 at +140).
template<class F> void transfer_component_fields(NpcAiComponent& c,F&& f){
    f(0x000,c.travel_speed_60);f(0x004,c.speed_priority_64);f(0x008,c.flags_68);f(0x00c,c.aggressiveness_6c);
    f(0x010,c.query_70);f(0x014,c.query_74);f(0x018,c.firing_threshold_78);f(0x01c,c.stop_timer_7c);
    f(0x020,c.look_timer_80);f(0x024,c.field_84);f(0x028,c.field_88);f(0x02c,c.fire_delay_8c);
    f(0x030,c.throw_scatter_90);f(0x034,c.throw_angle_94);f(0x038,c.radius_squared_98);f(0x03c,c.field_9c);
    f(0x040,c.movement_priority_a0);f(0x044,c.ammo_a4);f(0x048,c.field_a8);f(0x04c,c.query_priority_ac);
    f(0x050,c.held_item_b0);f(0x054,c.held_item_b4);f(0x058,c.switch_distance_b8);
    for(std::size_t i=0;i<c.allowed_regions_bc.size();++i)f(0x05c+4*i,c.allowed_regions_bc[i]);   // +bc..+df
    f(0x080,c.firing_node_e0);f(0x084,c.cover_node_e4);f(0x088,c.timer_e8);f(0x08c,c.mode_counter_ec);
    f(0x090,c.carry_node_f0);f(0x094,c.object_f4);
    f(0x0a0,c.follow_100);f(0x0a4,c.follow_104);f(0x0a8,c.follow_108);f(0x0ac,c.follow_10c);
    f(0x0b0,c.pending_tag_110);f(0x0b2,c.pending_mode_112);f(0x0b4,c.action_114);f(0x0b8,c.fire_priority_118);
    f(0x0bc,c.subtarget_11c);f(0x0c0,c.draw_priority_120);f(0x0c4,c.flags_124);f(0x0c8,c.timer_128);
    f(0x0cc,c.force_item_12c);f(0x0d0,c.force_count_130);f(0x0d4,c.force_delay_134);f(0x0d8,c.pending_node_138);
    f(0x0dc,c.state_13c);f(0x0dd,c.draw_13d);f(0x0de,c.byte_13e);f(0x0df,c.byte_13f);f(0x0e0,c.byte_140);
    f(0x0e1,c.flags_141);
    f(0x0f0,c.mode_44);f(0x0f4,c.requested_48);f(0x0f8,c.priority_4c);f(0x0fc,c.pending_50);f(0x100,c.team_54);
    f(0x130,c.perception_flags_20);
    auto& pf=c.path;
    for(std::size_t i=0;i<4;++i){f(0x140+4*i,pf.target_00[i]);f(0x150+4*i,pf.direction_10[i]);f(0x160+4*i,pf.saved_20[i]);}
    f(0x180,pf.remaining_40);f(0x184,pf.segment_44);f(0x188,pf.index_48);f(0x18c,pf.hold_4c);f(0x190,pf.current_50);
    f(0x194,pf.previous_54);f(0x198,pf.field_58);f(0x19c,pf.destination_5c);f(0x1a0,pf.flags_60);
}
// The cAITarget's first 0x30 bytes (but +20, the component's) at +110.
template<class P,class F> void transfer_perception_fields(P& p,F&& f){
    for(std::size_t i=0;i<4;++i)f(0x110+4*i,p.last_known_00[i]);
    f(0x120,p.radius_10);f(0x124,p.half_angle_14);f(0x128,p.region_18);f(0x12c,p.priority_1c);f(0x131,p.flags_21);
}
// A cTargetAwareness (0x40; +0 the handle's object, +4 its ID) relative to its slot.
template<class R,class F> void transfer_record_fields(R& r,F&& f){
    f(0x04,r.id_4);f(0x08,r.id_8);
    for(std::size_t i=0;i<4;++i)f(0x10+4*i,r.position_10[i]);
    f(0x20,r.cause_20);f(0x24,r.level_24);f(0x28,r.time_28);f(0x2c,r.score_2c);f(0x30,r.score_30);f(0x34,r.score_34);
    f(0x38,r.byte_38);f(0x39,r.byte_39);f(0x3a,r.byte_3a);f(0x3b,r.dead_notice_3b);f(0x3c,r.byte_3c);
}
// 00148b50 / 001489d0's `new (0x30) cPath(+78)` (cPath_ctor 00161930: +20 / +28 0, +14 / +10 -1, +24 the capacity).
void transfer_construct_path(NpcPathFollower& pf){
    auto b=std::make_shared<NpcRouteBuffer>();
    b->capacity_24=pf.capacity_78;b->start_14=-1;b->destination_10=-1;
    pf.route_74=std::move(b);
}
}

void npc_transfer_get_00148b50(NpcTransferBytes& out,NpcAiComponent& c,const NpcPerception& p,const NpcTransferLink& link){
    // The quadword copies (controller +60 0xf0, +44 0x14, cAITarget 0x30, the follower 0x70) also move the padding
    // the port does not model; it is packed as zero.
    transfer_zero(out,0x000,0xf0);transfer_zero(out,0x0f0,0x14);transfer_zero(out,0x110,0x30);transfer_zero(out,0x140,0x70);
    transfer_component_fields(c,[&](std::size_t off,const auto& v){transfer_store(out,off,v);});
    transfer_perception_fields(p,[&](std::size_t off,const auto& v){transfer_store(out,off,v);});
    // 0016ecc0: the target's ID (-1 when the handle is null or stale), then the first four awareness records.
    std::int32_t id=-1;
    if(p.target_40){
        if(!link.handle_live)throw std::runtime_error("NPC transfer: 0016ecc0 reads the target handle +40 (NpcTransferLink::handle_live)");
        if(link.handle_live(p.target_40,p.target_id_44))id=p.target_id_44;
    }
    transfer_store(out,0x1d0,id);
    for(std::size_t i=0;i<4;++i){
        const std::size_t base=0x1e0+0x40*i;
        if(i<p.records.size()){
            const auto& r=p.records[i];
            transfer_zero(out,base,0x40);
            transfer_store(out,base,static_cast<std::uint32_t>(r.object));   // PORT-ONLY: the handle's low word
            transfer_record_fields(r,[&](std::size_t off,const auto& v){transfer_store(out,base+off,v);});
        }else{
            transfer_store(out,base+8,std::int32_t{-1});                     // 0049c588
        }
    }
    auto& pf=c.path;
    if(!pf.route_74)transfer_construct_path(pf);
    // 00161610 cPathTransfer::Get.
    const auto& b=*pf.route_74;
    if(b.point_00)for(std::size_t i=0;i<4;++i)transfer_store(out,0x1b0+4*i,(*b.point_00)[i]);
    else transfer_zero(out,0x1b0,0x10);
    transfer_store(out,0x1c0,b.destination_10);
    if(!b.array_20){
        transfer_store(out,0x1c4,b.start_14);
        transfer_store(out,0x1c8,b.end_18);
    }else if(!b.nodes.empty()){
        transfer_store(out,0x1c4,b.nodes.back());
        transfer_store(out,0x1c8,b.nodes.front());
    }else{
        transfer_store(out,0x1c4,std::int32_t{-1});
    }
}

void npc_transfer_put_001489d0(const NpcTransferBytes& in,NpcAiComponent& c,NpcPerception& p,const NpcTransferLink& link,
    NpcRouteManager* routes){
    transfer_component_fields(c,[&](std::size_t off,auto& v){transfer_load(in,off,v);});
    transfer_perception_fields(p,[&](std::size_t off,auto& v){transfer_load(in,off,v);});
    const auto need_lookup=[&]{
        if(!link.lookup)throw std::runtime_error("NPC transfer: 0016e890 looks objects up by ID (0040c540; NpcTransferLink::lookup)");
    };
    // 0016e890: the target from its ID, registered for visibility when it has no controller.
    std::int32_t id{};transfer_load(in,0x1d0,id);
    if(id!=-1){
        need_lookup();
        const std::uint64_t o=link.lookup(id);
        p.target_40=o;p.target_id_44=o?id:-1;
        if(o){
            if(!link.has_controller||!link.add_target)
                throw std::runtime_error("NPC transfer: 0016e890 reads the target's controller +58 and cTargetManager_AddGobj (NpcTransferLink)");
            if(!link.has_controller(o))link.add_target(o);
        }
    }else{
        p.target_40=0;p.target_id_44=-1;
    }
    for(std::size_t i=0;i<4;++i){
        const std::size_t base=0x1e0+0x40*i;
        std::int32_t rid{};transfer_load(in,base+8,rid);
        if(rid==-1)continue;
        need_lookup();
        const std::uint64_t o=link.lookup(rid);
        if(!o)continue;
        AwarenessRecord r;                                                     // new (0x40), then *e = recs[i]
        transfer_record_fields(r,[&](std::size_t off,auto& v){transfer_load(in,base+off,v);});
        r.object=o;r.id_4=rid;                                                 // Set(&e->h, o)
        p.records.insert(p.records.begin(),r);                                 // push_front
    }
    // 001613e0 is in transfer_component_fields; then the cPath and 001614c0.
    auto& pf=c.path;
    if(!pf.route_74)transfer_construct_path(pf);
    auto& b=*pf.route_74;
    RootVector point{};
    for(std::size_t i=0;i<4;++i)transfer_load(in,0x1b0+4*i,point[i]);
    b.point_00=point;
    transfer_load(in,0x1c0,b.destination_10);
    transfer_load(in,0x1c4,b.start_14);
    transfer_load(in,0x1c8,b.end_18);
    if(!b.array_20){b.array_20=true;b.nodes.clear();}                          // cMemMan_Allocate(+24 * 4)
    if(b.start_14>=0){
        if(!routes)throw std::runtime_error("NPC transfer: 001614c0 restores the route (0015f3d0) without the route manager");
        npc_route_rebuild(*routes,b.start_14,b.end_18,pf.route_74);
    }
}

void NpcAiController::pack_transfer_00148b50(NpcTransferBytes& out){
    npc_transfer_get_00148b50(out,component_,perception_,transfer_link_);
}
void NpcAiController::apply_copy_00148cd0(SoloActorSimulation& actor,const NpcTransferBytes& in){
    if(remote_)throw std::runtime_error("NPC AI: 00148cd0 on the remote NPC controller (002d5ee0 makes the AI controller first)");
    route_actor_=&actor;
    npc_transfer_put_001489d0(in,component_,perception_,transfer_link_,system_.routes);
    aggressiveness_6c_=component_.aggressiveness_6c;
    component_.quiet_5c=actor.eligible()?1:0;                                  // +5c = the owner present
    activated(actor);                                                          // virtual +48 (00157df0)
    component_.quiet_5c=0;
}
void NpcAiController::convert_to_npc_002d5ee0(SoloActorSimulation& actor,const NpcTransferBytes& in){
    if(!remote_)throw std::runtime_error("NPC AI: cNetNPC_ConvertToNpc 002d5ee0 outside the remote NPC controller");
    auto& think=system_.think_list;                                            // 0015ad70(actor)
    for(auto i=think.begin();i!=think.end();){if(i->actor_id==actor_id_)i=think.erase(i);else ++i;}
    make_fresh_ai(actor);                                                      // cHuman_CreateGobjController(a, 2, ..)
    actor.set_byte_14(1);                                                      // +30 -> +14 = 1
    apply_copy_00148cd0(actor,in);
}
}
