#include "npc_attack.hpp"
#include "actor_fire.hpp"
#include "npc_alert.hpp"
#include "npc_cover.hpp"
#include "npc_route.hpp"
#include "npc_search.hpp"
#include "vu_float.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
bool dying_or_dead(std::uint8_t life){return life==2||life==3;}
NpcAttackHost& attack(NpcAiHost& h){
    if(!h.attack)throw std::runtime_error("NPC AI: the attack stages need their host (NpcAiHost::attack)");
    return *h.attack;
}
NpcPerception& perception(NpcAiHost& h){
    if(!h.perception||!h.sense)throw std::runtime_error("NPC AI: the attack stages read the perception (+1ac), which is not hosted");
    return *h.perception;
}
const NpcPerceptionHost& sense(NpcAiHost& h){perception(h);return *h.sense;}
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("NPC AI attack host: ")+what);
    return f;
}
NpcSenseObject view(const NpcPerceptionHost& h,std::uint64_t object){
    if(!h.object)throw std::runtime_error("NPC AI: no object views (NpcPerceptionHost::object)");
    const auto v=h.object(object);
    if(!v)throw std::runtime_error("NPC AI: an object the attack stages read is not known to the host");
    return *v;
}
std::uint64_t target_of(NpcAiHost& h){
    const auto& p=perception(h);
    return npc_resolve_handle(sense(h),p.target_40,p.target_id_44);
}
// Virtual +b4 (0014d310): perception +20 bits 0 and 2.
bool visible(const NpcAiComponent& c){return bit(c.perception_flags_20,0)&&bit(c.perception_flags_20,2);}
void set_pause_84(NpcAiComponent& c,float v){c.field_84=static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(v));}
void set_cooldown_88(NpcAiComponent& c,float v){c.field_88=static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(v));}
NpcLookSlot& look_slot(const NpcAiComponent& c,NpcAiSystem& s){
    if(c.look_slot_1b8<0||c.look_slot_1b8>=static_cast<std::int32_t>(s.look_slots.size()))
        throw std::runtime_error("NPC AI: the look slot +1b8 is read before Activate assigned one");
    return s.look_slots[static_cast<std::size_t>(c.look_slot_1b8)];
}
// +78, or WorthShootingThreshold 004ea388 when +78 is negative.
float firing_threshold(const NpcAiComponent& c,const NpcAiSystem& s){
    return c.firing_threshold_78<0.0F?s.worth_shooting_388:c.firing_threshold_78;
}
// An actor action message 0055d480 (+20 = 1), transport 0.
NpcAiSend action(std::int32_t id,std::uint8_t kind){
    NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=0;s.action=id;s.action_kind=kind;return s;
}
void send(NpcAiHost& h,const NpcAiSend& s){
    if(!h.send)throw std::runtime_error("NPC AI host: send");
    h.send(s);
}
// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w (vf0.w = 1), vsqrt.
float vu_length(const RootVector& v){
    const float x2=v[0]*v[0],y2=v[1]*v[1],z2=v[2]*v[2];
    return vu::sqrt((x2+y2)+z2);
}
RootVector sub4(const RootVector& a,const RootVector& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};}
RootVector add4(const RootVector& a,const RootVector& b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2],a[3]+b[3]};}
RootVector scale4(const RootVector& a,float s){return {a[0]*s,a[1]*s,a[2]*s,a[3]*s};}   // vmulx.xyzw
std::array<RootVector,4> self_matrix(NpcAiHost& h){
    if(!h.actor.world_matrix)throw std::runtime_error("NPC AI host: world_matrix");
    return h.actor.world_matrix();                                            // virtual +f4 (= +e8 natively)
}
// The held item (virtual +1a4 = 0014bcb0): -1 while +84 is 6.
std::int32_t held_item(NpcAttackHost& a){
    const auto slot=need(a.inventory_84,"inventory_84")();
    return slot==weapon_no_slot?-1:need(a.slot_item,"slot_item")(slot);
}
// Inventory +a0 set, or the held clip (00143940(inv, 6)) below the burst length +a4.
bool reload_due(const NpcAiComponent& c,NpcAttackHost& a){
    if(need(a.reloading_a0,"reloading_a0")())return true;
    return need(a.clip,"clip")()<c.ammo_a4;
}
bool region_allowed(const NpcAiComponent& c,std::int32_t region){
    if(region<0)return true;
    const auto word=static_cast<std::size_t>(region>>5);
    if(word>=c.allowed_regions_bc.size())throw std::runtime_error("NPC AI: a region index beyond the allowed-region bits +bc..+dc");
    return ((c.allowed_regions_bc[word]>>(static_cast<unsigned>(region)&31U))&1U)!=0;
}
bool player_controlled(const NpcSenseObject& o){return o.controller==NpcSenseObject::Controller::player;}   // +58 and its +54
void apply_notices(NpcAiComponent& c,NpcAiSystem& system,const std::vector<NpcFireNoticeCall>& notices,NpcAiHost& host){
    for(const auto& n:notices)npc_fire_notice(c,system,n.target,n.continuation,host);
}
}

float npc_burst_pause_84(const NpcAiComponent& c){return std::bit_cast<float>(static_cast<std::uint32_t>(c.field_84));}
float npc_shot_cooldown_88(const NpcAiComponent& c){return std::bit_cast<float>(static_cast<std::uint32_t>(c.field_88));}
void set_npc_aggressiveness(NpcAiComponent& c,float value){c.aggressiveness_6c=value;}

void npc_take_look_slot(NpcAiComponent& c,NpcAiSystem& system){
    std::size_t slot=0;
    for(std::size_t k=0;k<system.look_slots.size();++k){
        if(system.look_slots[k].in_use_14)continue;
        system.look_slots[k].in_use_14=1;slot=k;break;
    }
    c.look_slot_1b8=static_cast<std::int32_t>(slot);
    system.look_slots[slot].points.clear();                                   // 00158980
    system.look_slots[slot].count_c=0;system.look_slots[slot].frame_10=0;
}
void npc_release_look_slot(NpcAiComponent& c,NpcAiSystem& system){
    if(c.look_slot_1b8<0)return;                                               // +1b8 null
    auto& s=look_slot(c,system);
    s.points.clear();s.in_use_14=0;
}

void npc_update_aim(NpcAiComponent& c,NpcAiHost& host){
    auto& a=attack(host);
    std::uint64_t target=target_of(host);
    if(!visible(c))target=0;
    else{
        if(target!=0&&!npc_sense_present(view(sense(host),target)))target=0;
        if(dying_or_dead(host.actor.life_state()))target=0;
    }
    need(a.aim,"aim")(target,held_item(a),c.subtarget_11c);                   // virtual +60 = 0022f5b0
}

void npc_age_look_entries(NpcAiComponent& c,float dt,const NpcPerceptionHost& h){
    for(unsigned i=0;i<3;++i){                                                // 0014e5e8..0014e828, entries 0..2
        auto& e=i<2?c.look_entries[i]:c.look_entry_2;
        if(!e.active)continue;                                                // 0014e610
        bool keep=false;
        if(!(e.duration<=0.0F)){                                              // 0014e620 c.ole.s
            if(const auto o=npc_resolve_handle(h,e.object,e.id);o!=0){
                const auto v=view(h,o);
                keep=!v.actor_class||npc_sense_present(v);                   // 0014e6f8 (+4c), +2d, +2c, !+2e
            }
        }
        if(keep)e.duration-=dt;                                              // 0014e7cc
        else{e.active=0;e.priority=0;}                                        // 0014e804 compares the entry with itself
    }
}

void npc_look_at_selection(NpcAiComponent& c,float dt,NpcAiHost& host){
    c.look_167=0;
    if(c.byte_166)npc_look_around_select(c,dt,host);                          // 0014e0f8..0014e5e4 (npc_search.hpp)
    const auto& h=sense(host);
    npc_age_look_entries(c,dt,h);
    auto& e0=c.look_entries[0];
    const std::uint64_t o=e0.active?npc_resolve_handle(h,e0.object,e0.id):0;
    if(o!=0){
        if(e0.flag){
            const auto v=view(h,o);
            if(v.class_4ea770){
                RootVector p=need(attack(host).eye_c0,"eye_c0")(o,-1);
                p[2]+=4.5F;
                auto s=action(0x3d,4);s.point=p;send(host,s);               // vtable 004e0140
            }else{
                auto s=action(0x3d,5);s.word_2c=v.id;send(host,s);           // vtable 004e01a0
            }
            c.look_sent_1a8=1;e0.flag=0;
        }
    }else if(c.look_sent_1a8){
        auto s=action(0x3d,1);s.word_2c=0;send(host,s);                       // vtable 004d9320, byte 00489de0
        c.look_sent_1a8=0;
    }else{
        auto& a=attack(host);
        if(need(a.secondary,"secondary")()&&need(a.secondary_slot1_415,"secondary_slot1_415")()){
            auto s=action(0x3d,1);s.word_2c=0;send(host,s);                   // byte 00489de8
        }
    }
    c.look_timer_80+=dt;
    c.look_timer_80_reset=false;                                               // +80 is advanced natively from here on
}

bool npc_weapon_posture(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host){
    auto& a=attack(host);
    const auto& h=sense(host);
    const auto& p=perception(host);
    const std::uint64_t target=target_of(host);
    const auto f=c.perception_flags_20;
    bool raise=true,lower=true;
    if(c.draw_13d!=0){
        // 00150d28..00150eb4: the raise decision.
        if(need(a.inventory_84,"inventory_84")()==weapon_no_slot)raise=false;
        else if(c.state_13c!=0&&(c.action_114==4||c.action_114==0xb||c.action_114==3||c.action_114==9))raise=false;
        else if(c.force_count_130>0)raise=true;
        else if(c.draw_13d==1){
            if(!bit(f,3)&&!bit(f,2))raise=false;
            else if(visible(c)||(bit(f,3)&&bit(f,1)))raise=true;
            else if(firing_threshold(c,system)<std::bit_cast<float>(0x3c23d70aU))raise=true;   // 0.01
            else{
                const AwarenessRecord* r=target!=0?npc_perception_sensed_record(p,h,target):nullptr;   // 0016c6a0
                raise=r&&r->level_24<5.0F;
            }
        }
        // 00150eb8..: the aim point.
        if(c.look_entries[1].active){                                          // virtual +a8(1)
            const auto o=npc_resolve_handle(h,c.look_entries[1].object,c.look_entries[1].id);
            if(o==0)throw std::runtime_error("NPC AI: 00150ca0 reads the eye +c0 of look entry 1's object, which no longer resolves");
            RootVector q=need(a.eye_c0,"eye_c0")(o,-1);
            if(view(h,o).class_4ea770)q[2]+=4.5F;
            auto s=action(0x39,4);s.point=q;send(host,s);                    // vtable 004e0140
            send(host,action(0x3f,0));
            lower=false;
        }else if(raise){
            lower=false;
            if(target!=0&&visible(c)){
                const RootVector q=need(a.eye_c0,"eye_c0")(target,c.subtarget_11c);
                auto s=action(0x39,4);s.point=q;send(host,s);
                auto t=action(0x3e,5);t.word_2c=view(h,target).id;send(host,t);   // vtable 004e01a0
            }else{
                const auto m=self_matrix(host);
                RootVector q;
                if(bit(f,4)){
                    q=p.last_known_00;
                    const float d=vu_length(sub4(q,m[3]));                       // virtual +e8 row 3
                    if(!(d<=3.0F)){
                        if(target!=0){
                            const auto v=view(h,target);
                            if(v.actor_class||v.class_4ea770)q[2]+=4.5F;
                        }
                    }else q=add4(q,scale4(m[1],20.0F));                          // +f4 row 1 x 20
                }else{
                    q=add4(m[3],scale4(m[1],20.0F));                             // 00151780(+f4, 1)
                    q[2]+=4.5F;
                }
                auto s=action(0x39,4);s.point=q;send(host,s);
                send(host,action(0x3f,0));
            }
        }
    }
    if(lower){
        send(host,action(0x3a,0));
        send(host,action(0x3f,0));
    }
    return raise;
}

void npc_firing_gate(NpcAiComponent& c,NpcAiSystem& system,float dt,NpcAiHost& host){
    auto& a=attack(host);
    const auto& h=sense(host);
    const std::uint64_t target=target_of(host);
    if(!host.actor.mode_320c||!host.actor.life_state)throw std::runtime_error("NPC AI host: mode_320c / life_state");
    if(c.field_a8<=0&&npc_burst_pause_84(c)<=0.0F&&host.actor.mode_320c()!=0x1a)c.field_a8=c.ammo_a4;   // burst refill
    bool fire=false;
    if(c.force_count_130>0)fire=true;
    else if(c.byte_13e==0||c.field_a8<=0||dying_or_dead(host.actor.life_state())){}
    else if(c.byte_13e==2)fire=true;
    else if(look_slot(c,system).count_c>0&&host.actor.mode_320c()==0x1a&&need(a.full_auto,"full_auto")())fire=true;
    else if(c.look_entries[1].active){}                                       // virtual +a8(1)
    else{
        bool go=true;
        if(!visible(c)){
            if(!(firing_threshold(c,system)<std::bit_cast<float>(0x3c23d70aU)))go=false;   // 0.01
            else if(target==0)go=false;
        }
        if(go){
            if(need(a.held_class,"held_class")()==1)fire=true;                 // 001437a0
            else{
                float threshold=firing_threshold(c,system);
                if(target!=0&&!player_controlled(view(h,target)))threshold*=0.5F;   // 00150c60, +58 virtual +54
                fire=!(need(a.aim_block,"aim_block")()[0]<threshold);             // +20
            }
        }
    }
    if(fire&&need(a.can_attack,"can_attack")()&&npc_shot_cooldown_88(c)<=0.0F&&!(a.port_hold_fire&&a.port_hold_fire())){   // 00141240
        if(need(a.held_class,"held_class")()==1){
            if(!need(a.byte_33e4,"byte_33e4")()){
                std::uint64_t to=target;                                           // the throw target +1c8, else the perception target
                if(const auto t=npc_resolve_handle(h,c.throw_target_1c8,c.throw_target_id_1cc);t!=0)to=t;
                if(to!=0){
                    auto& rnd=need(a.random,"random");
                    const float y=static_cast<float>(static_cast<std::int32_t>(rnd()))/std::bit_cast<float>(0x4f000000U);
                    const float x=static_cast<float>(static_cast<std::int32_t>(rnd()))/std::bit_cast<float>(0x4f000000U);
                    const RootVector off=scale4(RootVector{x,y,0.0F,0.0F},c.throw_scatter_90);
                    RootVector q=need(a.position_f4,"position_f4")(to);
                    if(c.flags_141&0x10U){
                        const RootVector self=self_matrix(host)[3];
                        q=add4(scale4(sub4(q,self),std::bit_cast<float>(0x3f4ccccdU)),self_matrix(host)[3]);   // 0.8
                    }
                    q=add4(q,off);
                    auto s=action(0x3b,0xb);
                    s.word_30=-1;s.word_34=c.subtarget_11c;s.byte_38=0;s.point=q;   // the none word *004bdd00
                    s.power_50=std::bit_cast<float>(0x3c8efa35U)*c.throw_angle_94;
                    send(host,s);
                }
            }
        }else if(need(a.ready,"ready")()){                                      // 00143400(inv, 0)
            const auto states=need(a.secondary,"secondary")();
            if(states&&secondary_state_allows_fire((*states)[0])&&secondary_state_allows_fire((*states)[1])){
                auto s=action(0x3b,0xb);
                s.word_30=target!=0?view(h,target).id:-1;s.word_34=c.subtarget_11c;s.byte_38=0;s.point=RootVector{};s.power_50=0.0F;
                send(host,s);
            }else apply_notices(c,system,need(a.discharge,"discharge")(),host);   // 0013ff40(inv, 0, -1, 0, 0)
        }
    }
    if(need(a.can_attack,"can_attack")()){
        if(!(npc_shot_cooldown_88(c)<=0.0F))set_cooldown_88(c,npc_shot_cooldown_88(c)-dt);
        if(!(npc_burst_pause_84(c)<=0.0F))set_pause_84(c,npc_burst_pause_84(c)-dt);
    }
}

void npc_fire_notice(NpcAiComponent& c,NpcAiSystem& system,std::uint64_t target,bool continuation,NpcAiHost& host){
    auto& a=attack(host);
    if(!continuation){
        auto& slot=look_slot(c,system);
        slot.count_c+=1;
        std::uint32_t d=system.frame-static_cast<std::uint32_t>(slot.frame_10);   // subu
        if(static_cast<std::int32_t>(d)<=0)d=0U-d;                                // negu
        const auto gap=static_cast<std::int32_t>(d);
        if(!(gap<0x5b)){                                                       // 91 frames
            NpcAiSend s;s.kind=NpcAiSend::Kind::script;s.transport=0;s.key=npc_script_weapon_fired;   // 0026f110, to the component
            s.object=target;
            s.word_28=need(a.slot_item,"slot_item")(need(a.inventory_84,"inventory_84")());
            s.word_2c=slot.count_c;
            s.byte_38=need(a.held_class,"held_class")()==1?1:0;                  // +30
            send(host,s);
            slot.count_c=0;slot.frame_10=static_cast<std::int32_t>(system.frame);
        }
    }
    if(c.force_count_130>0){
        c.force_count_130-=1;
        set_cooldown_88(c,static_cast<float>(c.force_delay_134));
        if(c.force_count_130<=0&&c.force_item_12c>=0){                          // 0014f534..0014f560
            need(a.select_item,"select_item (0014f460 restores +12c through virtual +1ac = 0014bae0)")(c.force_item_12c);
            c.force_item_12c=-1;
        }
    }else{
        c.field_a8-=1;
        if(c.field_a8==0)
            set_pause_84(c,4.0F*(std::bit_cast<float>(0x3e4ccccdU)+std::bit_cast<float>(0x3c23d70aU)*static_cast<float>(a.actor_id&15U)));
        set_cooldown_88(c,c.fire_delay_8c);
    }
}

void npc_force_weapon_fire(NpcAiComponent& c,std::int32_t shots,std::int32_t item,float delay,NpcAiHost& host){
    if(item>=0){
        auto& a=attack(host);
        const std::int32_t held=held_item(a);                                    // virtual +1a4 = 0014bcb0
        if(held!=item){                                                        // 0014b4ac
            npc_force_weapon_fire_stores(c,shots,delay);
            c.force_item_12c=held;                                             // 0014b4c8..0014b4d8: +1a4 again
            need(a.select_item,"select_item (0014b470 -> virtual +1ac = 0014bae0)")(item);
            return;
        }
    }
    npc_force_weapon_fire_stores(c,shots,delay);
}

void npc_force_weapon_fire_stores(NpcAiComponent& c,std::int32_t shots,float delay){
    c.force_item_12c=-1;                                                       // 0014b4b8
    c.force_count_130=shots;                                                   // 0014b500
    c.force_delay_134=npc_cvt_w_s(delay==0.0F?c.fire_delay_8c:delay);          // 0014b4f8..0014b524
}

std::uint8_t npc_combat_posture(NpcAiComponent& c,NpcAiHost& host){
    auto& a=attack(host);
    const auto& p=perception(host);
    float threat=0.0F,ability=0.0F,switch_due=0.0F;
    if(const auto t=target_of(host);t!=0){
        threat=std::bit_cast<float>(0x3dcccccdU)+npc_perception_threat(p,sense(host),t);   // 0.1 + 0016c730
        const auto block=need(a.aim_block,"aim_block")();
        ability=std::bit_cast<float>(0x3dcccccdU)+(block[0]*block[1])*15.0F;
    }
    bool due=reload_due(c,a);
    if(!due){
        const std::int32_t primary=c.held_item_b0;                              // virtual +1c0(c, 0)
        if(!(c.switch_distance_b8<=0.0F)&&bit(c.perception_flags_20,4)&&primary>=0){
            const float d=vu_length(sub4(self_matrix(host)[3],p.last_known_00));
            if(primary==held_item(a))due=!(10.0F+d<=c.switch_distance_b8);
            else due=d<c.switch_distance_b8;
        }
    }
    if(due)switch_due=1.0F;
    const float score=((ability*c.aggressiveness_6c)-threat)-switch_due;
    if(score<std::bit_cast<float>(0xbe4ccccdU))return 2;                      // -0.2
    if(score<=std::bit_cast<float>(0x3ee66666U))return 3;                     // 0.45
    return 4;
}

bool npc_attack_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    auto& p=perception(host);
    const auto f=c.perception_flags_20;
    if(m.kind==NpcAiMessage::Kind::ai){
        switch(m.event){
        case 2:{                                                                // the tick
            if(bit(f,3)&&visible(c)){
                const float d=vu_length(sub4(self_matrix(host)[3],p.last_known_00));
                if(c.state_13c!=0&&d<30.0F&&c.path.destination_5c==p.region_18)set_npc_movement_state(c,0,20,host);
            }
            return true;}
        case 3:{                                                                // the timer
            const float d=vu_length(sub4(self_matrix(host)[3],p.last_known_00));
            bool relocate=false;
            if(visible(c)){
                if(c.state_13c==0&&!(d<=40.0F)){
                    if(!region_allowed(c,p.region_18))(void)npc_approach_search(c,host);   // 001555f0, then no relocation
                    else if(c.path.destination_5c!=p.region_18)relocate=true;
                    else{
                        const auto t=target_of(host);
                        if(t==0)throw std::runtime_error("NPC AI: Attack reads the controller +58 of a target that no longer resolves");
                        relocate=player_controlled(view(sense(host),t));         // +58 and its virtual +54
                    }
                }
            }else{
                relocate=c.state_13c==0;
                if(reload_due(c,attack(host)))send(host,action(0x3c,0));        // the reload action, transport 0
            }
            if(relocate){
                c.byte_13f=4;
                const auto region=p.region_18;
                if(!region_allowed(c,region))(void)npc_approach_search(c,host);   // 00155850
                else if(c.contact_158!=0&&region>=0){
                    auto& a=attack(host);
                    if(!need(a.route_head_pending,"route_head_pending")())         // 00163fd0
                        need(a.route_to_point,"route_to_point")(p.last_known_00,region);   // 0015fa70(+1b0, +1ac, region)
                }
            }
            c.timer_e8=40;c.mode_counter_ec+=1;
            return true;}
        case 4:
            c.timer_e8=1;
            if(visible(c))c.mode_counter_ec=0;
            return true;
        case 8:
            if(npc_combat_posture(c,host)==2){
                (void)npc_cover_search(c,host);                                 // 001558f4
                c.pending_tag_110=4;c.pending_mode_112=2;                       // the word {4, 2} at +110
            }
            return false;
        case 7:
            c.timer_e8=1;
            return true;
        case 0:
            c.timer_e8=bit(f,4)?1:-1;
            c.mode_counter_ec=0;
            request_npc_weapon_draw(c,2,20);                                    // virtual +bc(2, 20)
            c.firing_node_e0=-1;c.cover_node_e4=-1;
            return true;
        default:return false;                                                   // the exit's +bc(1, 20) at 00155998 is unreachable
        }
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;
    switch(m.event){
    case 5:{
        const RootVector v=sub4(self_matrix(host)[3],p.last_known_00);
        const float d2=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];                         // vmulax.x / vmadday.x / vmaddz.x by 1
        if(visible(c)&&c.state_13c!=0&&d2<900.0F)set_npc_movement_state(c,0,20,host);
        return false;}
    case 2:
        if(c.pending_tag_110==4&&c.pending_mode_112!=4)request_npc_ai_mode(c,c.pending_mode_112,0);
        return false;
    case 7:
        if(m.value==10&&c.path.current_50>=0){
            if(!host.graph)throw std::runtime_error("NPC AI host: no node collection");
            if(host.graph->node(c.path.current_50).parameter_7c==p.region_18)request_npc_weapon_draw(c,2,20);
        }
        return false;
    default:return false;
    }
}
}
