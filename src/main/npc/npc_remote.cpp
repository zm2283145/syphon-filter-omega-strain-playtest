// 132ln5h13: the remote NPC controller 0x004de2a0 (npc_remote.hpp; research/NET_INLEVEL.md 4.23).
#include "npc_remote.hpp"
#include "npc_world.hpp"
#include "actor_fire.hpp"
#include "actor_object_action.hpp"
#include "npc_death_notice.hpp"
#include "npc_route.hpp"
#include "vu_float.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
namespace {
bool dying_or_dead(std::uint8_t life){return life==2||life==3;}
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
    if(actor.eligible())remote_activate(actor);                                // virtual +48 = 002d40a0
}
void NpcAiController::make_fresh_ai(SoloActorSimulation& actor){
    if(!remote_)return;                                                        // 001b3d98: virtual +88 == 1
    route_actor_=&actor;
    // 00158800 / 001588d0 -> 00158bc0: the constructor's draw, then 001ae450 destroys the remote (002d5fa0).
    const float r=game_int_to_float(static_cast<std::int32_t>(combat_.random?combat_.random():next_game_random(*random_state_)));
    destroy_component(actor);
    remote_.reset();
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
    }else if(alive_or_eligible(actor))
        throw std::runtime_error("NPC AI: the remote NPC's deactivation of an alive actor without +150 (the NPC info release 00436c00 / 00434440, "
            "0042d910 with +141 bit 7) is not ported");
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
    if(r.flags_214&0x02U)
        throw std::runtime_error("NPC AI: the remote NPC's Activate with +214 bit 1 (the route request 0015f9b0 to +208) is not ported");
    r.timer_204=0.0F;
    c.inactive_159=0;
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
    bool fire=false;
    if(target!=0&&link&&link->firing_21){
        const auto v=present(target);
        fire=(v.root[1]&&v.root[0]&&!v.root[2])||v.class_4ea770;
    }
    if(fire)throw std::runtime_error("NPC AI: the remote NPC's fire (002c4f20, then 002d48f0 / 002d4b30) is not ported");
    for(const std::int32_t a:{0x3a,0x3f}){
        NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=0;s.action=a;s.action_kind=0;   // key 0055d480, vtable 004e0840
        h.send(s);
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
            throw std::runtime_error("NPC AI: the remote NPC's follow (+214 bit 0: 00153870 with the cNetHuman's target) is not ported");
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
        if(!(r.timer_204<=1.0F))
            throw std::runtime_error("NPC AI: the remote NPC's lost-contact deactivation (actor +60(1), then 0042d790) is not ported");
        r.timer_204+=dt;
    }
    h.remote->speed_200=r.speed_200;
    run_npc_locomotion(c,h);                                                   // 0014bff0
}
}
