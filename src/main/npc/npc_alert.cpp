#include "npc_alert.hpp"
#include "actor_fire.hpp"
#include "npc_attack.hpp"
#include "npc_cover.hpp"
#include "npc_route.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
NpcPerception& perception(NpcAiHost& h){
    if(!h.perception||!h.sense)throw std::runtime_error("NPC AI: the perception (+1ac) is not hosted (NpcAiHost::perception / sense)");
    return *h.perception;
}
const NpcPerceptionHost& sense(NpcAiHost& h){perception(h);return *h.sense;}
NpcSenseObject view(const NpcPerceptionHost& h,std::uint64_t object){
    if(!h.object)throw std::runtime_error("NPC AI: no object views (NpcPerceptionHost::object)");
    const auto v=h.object(object);
    if(!v)throw std::runtime_error("NPC AI: an object the AI reads is not known to the host");
    return *v;
}
// The look entries by index (+16c + 20 i); 0014d390 (+a8) reads the active byte.
std::int32_t& look_priority(NpcAiComponent& c,unsigned i){
    if(i<2)return c.look_entries[i].priority;
    if(i==2)return c.look_entry_2.priority;
    throw std::runtime_error("NPC AI: look entry index +164 outside the three entries");
}
std::uint8_t& look_active(NpcAiComponent& c,unsigned i){
    if(i<2)return c.look_entries[i].active;
    if(i==2)return c.look_entry_2.active;
    throw std::runtime_error("NPC AI: look entry index +164 outside the three entries");
}
void forward(const std::vector<NpcPerceptionSend>& out,NpcAiHost& host);
}

std::int32_t npc_cvt_w_s(float value){
    if(std::isnan(value))throw std::runtime_error("NPC AI: cvt.w.s of a NaN");
    if(value>=2147483648.0F)return std::numeric_limits<std::int32_t>::max();
    if(value<=-2147483648.0F)return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
std::int32_t npc_weapon_rating(const WeaponInventory& inventory,const WeaponDatabase& database){
    if(inventory.current_84==weapon_no_slot)return 0;                        // +84 == 6
    const auto item=inventory.slots.at(inventory.current_84).item;           // +8 of the slot
    const auto value=static_cast<float>(database.record(item).damage_mid_64());   // 00147840, +64, cvt.s.w
    return npc_cvt_w_s(value*std::bit_cast<float>(0x3f800000U));             // x 1.0 (00489c10), cvt.w.s
}

bool npc_search_allowed(const NpcAiComponent& c,NpcAiHost& host){
    bool allowed=c.contact_158!=0;
    auto& p=perception(host);const auto& h=sense(host);
    if(const auto t=npc_resolve_handle(h,p.target_40,p.target_id_44);t!=0&&!view(h,t).actor_class)allowed=false;
    return allowed;
}

bool npc_alert_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        if(m.event!=0&&m.event!=3)return false;
        if(c.path.destination_5c<0)return true;                              // not in a region
        if(!host.held_weapon_rating)throw std::runtime_error("NPC AI: mode 1 needs the held weapon's rating (00142b50)");
        const float rating=c.aggressiveness_6c*static_cast<float>(host.held_weapon_rating());
        auto& p=perception(host);const auto& h=sense(host);
        const auto t=npc_resolve_handle(h,p.target_40,p.target_id_44);
        const float threat=t!=0?npc_perception_threat(p,h,t):0.0F;           // 0016c730
        if(!(threat<=rating)){                                                 // the search, then the pair {1, 2}
            npc_cover_search(c,host);                                          // 00150150 (N5c)
            c.pending_tag_110=1;c.pending_mode_112=2;
            return true;
        }
        if(c.aggressiveness_6c<std::bit_cast<float>(0x3f666666U)){           // 0.9
            npc_firing_position_search(c,host);                                // 0014fec0 (N5c)
            c.pending_tag_110=1;c.pending_mode_112=3;
            return true;
        }
        if(c.contact_158!=0)request_npc_ai_mode(c,4,0);
        return true;
    }
    if(m.kind!=NpcAiMessage::Kind::path)return false;
    switch(m.event){
    case 2:
        if(c.pending_tag_110==1)request_npc_ai_mode(c,c.pending_mode_112,0);
        return false;
    case 1:case 3:
        if(npc_search_allowed(c,host))request_npc_ai_mode(c,5,0);
        else c.timer_e8=60;
        return false;
    case 5:case 6:
        c.timer_e8=1;
        return false;
    default:return false;
    }
}

void npc_travel_speed_for_purpose(NpcAiComponent& c,std::uint8_t purpose,NpcAiHost& host){
    // 00160130: |the actor's +f4 translation - the route buffer +74's point|,
    // 0 without a route buffer (npc_route_distance). A buffer no route was
    // written to holds heap content: measured as the origin for actors
    // loaded with the level (npc_unwritten_route_distance); otherwise throws.
    float distance=0.0F;bool heap_point=false;
    if(c.path.route_74){
        const RootVector translation=host.actor.world_matrix()[3];
        if(c.path.route_74->point_00)distance=npc_route_distance(c.path,translation);
        else if(host.level_loaded){
            distance=npc_unwritten_route_distance(translation);
            if(host.unwritten_route_reads)++*host.unwritten_route_reads;
        }
        else heap_point=true;
    }
    if(!host.random)throw std::runtime_error("NPC AI: 0014d730 needs the game RNG (00127dd0)");
    const auto r=static_cast<std::int32_t>(host.random());
    const float scaled=std::bit_cast<float>(0x2fa8f5c3U)*static_cast<float>(r);   // 00489df0 x cvt.s.w
    const float a=c.aggressiveness_6c+scaled;
    if(heap_point){
        // Any other actor's unwritten point is heap content (NPC_COMBAT.md 5.6). 00160130's D is the VU vsqrt of a
        // sum of squares (001601ac..001601c4): finite and >= 0 for any content. With a - 0.33 <= 0, 0014d798's
        // (a - 0.33) x D is <= 0, so cvt.w.s gives <= 0 and i = 0 (0014d7c4) whatever D is: D = 0 then gives the same
        // speed. Decided only with a margin (2^-20) far above the port's few-ulp rounding difference; otherwise throws.
        if(a<=std::bit_cast<float>(0x3ea8f5c3U)-std::bit_cast<float>(0x35800000U)){
            if(host.value_free_route_reads)++*host.value_free_route_reads;
        }
        else{
            // Undefined-read policy: the speed depends on the heap content, so the original is not reproducible here;
            // the host may take the level-loaded measured point (the origin, npc_unwritten_route_distance).
            if(!host.spawned_route_policy_reads)
                throw std::runtime_error("NPC AI: 0014d730 reads an unwritten route buffer point (00160130) of an actor not loaded with the level (heap content, not reproducible), and its speed depends on it (a = "+std::to_string(a)+")");
            distance=npc_unwritten_route_distance(host.actor.world_matrix()[3]);
            ++*host.spawned_route_policy_reads;
        }
    }
    const float b=(a-std::bit_cast<float>(0x3ea8f5c3U))*distance;               // - 0.33, x D
    std::int32_t i=npc_cvt_w_s(b/5.0F);
    if(!(i<4))i=3;
    if(i<0)i=0;
    static constexpr std::uint8_t table1[4]={3,2,3,2},table23[4]={1,2,2,3},table4[4]={2,2,3,3},table56[4]={2,3,2,3};   // 00489e10, 00489df8 / 00489e00, 00489e08, 00489e18
    std::uint8_t speed=1;                                                     // Walk
    const auto k=static_cast<std::size_t>(i);
    switch(purpose){
    case 1:speed=table1[k];break;
    case 2:case 3:speed=table23[k];break;
    case 4:speed=table4[k];break;
    case 5:case 6:speed=table56[k];break;
    default:break;
    }
    const float value=npc_travel_speed(speed);                               // virtual +148
    if(!(0<c.speed_priority_64)){
        c.travel_speed_60=value;c.speed_priority_64=0;
        c.flags_141=static_cast<std::uint8_t>(c.flags_141&~0x40U);
    }
    request_npc_weapon_draw(c,1,20);                                          // virtual +bc(1, 20)
}

void npc_request_look_at(NpcAiComponent& c,std::uint64_t object,std::int32_t object_id,std::int32_t priority,float duration){
    if(priority<c.look_entries[0].priority)return;
    const unsigned i=c.look_index_164;
    if(priority<look_priority(c,i)){c.look_timer_80=0.0F;c.look_timer_80_reset=true;return;}
    if(look_active(c,i)!=0&&!(priority<look_priority(c,i))){look_active(c,i)=0;look_priority(c,i)=0;}
    c.look_index_164=0;
    auto& e=c.look_entries[0];
    e.object=object;e.id=object!=0?object_id:-1;e.active=1;e.flag=1;e.duration=duration;e.priority=priority;
    c.look_timer_80=0.0F;c.look_timer_80_reset=true;
}

void npc_request_facial_look(NpcAiComponent& c,std::uint64_t object,std::int32_t object_id,std::int32_t priority,float duration){
    if(priority<c.look_entry_2.priority)return;                              // 0014dec4: entry 2's own +1a0
    const unsigned i=c.look_index_164;                                        // 0014ded4 lbu
    if(priority<look_priority(c,i))return;                                    // 0014dee8 (no +80 reset)
    if(look_active(c,i)!=0&&!(priority<look_priority(c,i))){look_active(c,i)=0;look_priority(c,i)=0;}   // +a8 = 0014d390
    c.look_index_164=2;                                                       // 0014df44
    auto& e=c.look_entry_2;
    e.object=object;e.id=object!=0?object_id:-1;e.active=1;e.flag=1;e.duration=duration;e.priority=priority;
}

void npc_release_look_entry(NpcAiComponent& c,NpcAiHost& host){
    const unsigned i=c.look_index_164;
    if(!(look_priority(c,i)<51))return;
    look_active(c,i)=0;look_priority(c,i)=0;
    c.byte_166=0;c.look_160=0;
    if(c.look_resume_168!=0)resume_npc_movement(c,host);                     // virtual +158
}

void npc_dispatch_awareness_event(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(!host.object_by_id)throw std::runtime_error("NPC AI: the dispatcher's AI event 7 needs the object lookup 0040c540");
    const auto object=host.object_by_id(m.value);
    if(object==0)return;
    const auto cause=static_cast<std::uint8_t>(m.word_2c&0xff);
    if(cause==2||cause==3||cause==4||cause==7)npc_hearing_reaction(c,cause,object,host);
}

void npc_hearing_reaction(NpcAiComponent& c,std::uint8_t cause,std::uint64_t object,NpcAiHost& host){
    if(object==0)return;
    auto& p=perception(host);const auto& h=sense(host);
    if(npc_perception_ignores(h,object))return;                              // port-only: no reaction to an ignored object
    std::vector<NpcPerceptionSend> out;
    std::uint64_t shooter_target=0;                                          // s1
    if(cause==4){
        const auto o=view(h,object);
        if(o.controller==NpcSenseObject::Controller::player){
            if(h.local_player==0)throw std::runtime_error("NPC AI: 00152c50 reads the local player (*004ffc0c), which the host does not name");
            shooter_target=view(h,h.local_player).aim_target;              // 003ea550(*004ffc0c)
        }else if(o.controller==NpcSenseObject::Controller::ai)shooter_target=o.ai_target;
        if(npc_perception_ignores(h,shooter_target))shooter_target=0;        // port-only
        if(shooter_target!=0&&shooter_target!=h.self)npc_perception_add(p,h,shooter_target,2,0.0F,out);
    }
    forward(out,host);
    bool react=cause==7;
    if(p.priority_1c<50){
        const auto t=npc_resolve_handle(h,p.target_40,p.target_id_44);
        if(t==0||npc_perception_threat(p,h,t)<=std::bit_cast<float>(0x3dcccccdU))react=true;   // 0.1
    }
    if(!react)return;
    std::uint64_t subject=0;
    if(npc_is_enemy(c,h,object))subject=object;
    else if(shooter_target!=0&&npc_is_enemy(c,h,shooter_target))subject=shooter_target;
    if(subject==0)return;
    if(bit(c.perception_flags_20,0)&&bit(c.perception_flags_20,2))return;  // the own target is visible
    if(c.look_timer_80_reset&&!host.attack)
        throw std::runtime_error("NPC AI: 00152c50 reads +80 after a look request reset it; +80 is advanced by the look-at selection 0014e0c0, which this host leaves out (NpcAiHost::attack)");
    if(c.look_timer_80<=5.0F)return;
    const std::int32_t id=view(h,subject).id;
    const auto* r=npc_perception_sensed_record(p,h,subject);
    if(r&&r->byte_39!=0){
        const unsigned i=c.look_index_164;
        if(!(look_priority(c,i)<21))return;
        if(look_active(c,i)!=0&&look_priority(c,i)<21){look_active(c,i)=0;look_priority(c,i)=0;}   // +a8(+164)
        c.look_index_164=1;
        auto& e=c.look_entries[1];
        e.object=subject;e.id=id;e.active=1;e.flag=1;e.duration=1.0F;e.priority=20;
        return;
    }
    npc_request_look_at(c,subject,id,20,1.0F);                               // +128(subject, 20, 1.0)
}

void npc_hear(NpcAiComponent& c,std::uint8_t cause,float value,std::uint64_t source,NpcAiHost& host,std::vector<NpcPerceptionSend>& out){
    if(source==0)return;
    if(cause==5){
        if(c.mode_44==10)return;
        throw std::runtime_error("NPC AI: hearing a landed projectile (00152fd0 cause 5: its owner, 0025ac00, mode 10 or action 8) is not native (N5d)");
    }
    npc_perception_add(perception(host),sense(host),source,cause,value,out);  // 0016c9d0
}

void npc_ai_sound_event(const NpcSoundMessage& m,const NpcSoundWorld& w){
    if(!w.object_by_id)throw std::runtime_error("NPC AI sound: the object lookup 0040c540 is not hosted");
    const auto source=w.object_by_id(m.source_id_30);
    if(source==0)return;
    if(m.kind_24==2){
        if(m.cause_25!=7)return;
        const auto target=w.object_by_id(m.target_id_34);
        if(target==0)throw std::runtime_error("NPC AI sound: a directed sound to an object that does not exist");
        if(!w.ai_controlled||!w.hear_directed)throw std::runtime_error("NPC AI sound: directed sounds are not hosted");
        if(w.ai_controlled(target))w.hear_directed(target,7,source,0.0F);
        return;
    }
    if(m.cause_25==5)throw std::runtime_error("NPC AI sound: a landed projectile (cause 5, its owner +124) is not native (N5d)");
    if(!w.listeners||!w.position)throw std::runtime_error("NPC AI sound: the think list is not hosted");
    const RootVector from=m.kind_24==0?w.position(source):m.point_40;
    for(const auto& l:w.listeners()){
        if(!l||l->id==m.source_id_30||l->life_108==2||l->life_108==3||!l->ai_58)continue;
        if(!(l->deafness_33ac<=0.0F))continue;
        const float x=l->position[0]-from[0],y=l->position[1]-from[1],z=l->position[2]-from[2];
        const float d=vu::sqrt((x*x+y*y)+z*z);
        if(!(d<m.radius_2c))continue;
        if(!l->hear)throw std::runtime_error("NPC AI sound: a listener without its +d0");
        l->hear(m.cause_25,source,d);
    }
}

void npc_add_awareness(NpcAiComponent&,std::uint64_t object,NpcAiHost& host,std::vector<NpcPerceptionSend>& out){
    if(object==0)throw std::runtime_error("NPC AI: AddAwareness of a null object (0016c9d0 dereferences it)");
    npc_perception_add(perception(host),sense(host),object,1,0.0F,out);
}
void npc_add_awareness_of_players(NpcAiComponent& c,NpcAiHost& host,std::vector<NpcPerceptionSend>& out){
    const auto slots=sense(host).player_slots;
    for(const auto player:slots)if(player!=0)npc_add_awareness(c,player,host,out);
}
void npc_script_set_target(NpcAiComponent& c,std::uint64_t object,NpcAiHost& host){
    npc_perception_set_target(perception(host),c,sense(host),object,100);
}
void npc_script_request_facial(NpcAiComponent& c,std::uint64_t object,float duration,NpcAiHost& host){
    const std::int32_t id=object!=0?view(sense(host),object).id:-1;           // 0014df48..0014df70: *(object +c)
    npc_request_facial_look(c,object,id,100,duration);                         // 00149e80: +12c(c, object, 100, f12)
}
void npc_script_request_look_at(NpcAiComponent& c,std::uint64_t object,float duration,NpcAiHost& host){
    const std::int32_t id=object!=0?view(sense(host),object).id:-1;           // 0014e054..0014e078: *(object +c)
    npc_request_look_at(c,object,id,100,duration);                             // 00149ed0: +128(c, object, 100, f12)
}
void npc_set_grenade_target(NpcAiComponent& c,std::uint64_t object,std::int32_t id,float scatter,std::uint32_t flag){
    c.throw_target_1c8=object;                                                  // 0014b418 (in the delay slot)
    c.throw_target_id_1cc=object!=0?id:-1;                                     // 0014b414..0014b444
    c.throw_scatter_90=scatter;                                                 // 0014b44c
    c.flags_141=static_cast<std::uint8_t>((c.flags_141&~0x10U)|((flag&1U)<<4));   // 0014b43c..0014b45c
}
void npc_script_set_grenade_target(NpcAiComponent& c,std::uint64_t object,NpcAiHost& host){
    const std::int32_t id=object!=0?view(sense(host),object).id:-1;
    npc_set_grenade_target(c,object,id,std::bit_cast<float>(0x40800000U),0);  // 00149830 lui 0x4080, a2 = 0
}
void npc_resume_look_at(NpcAiComponent& c,std::int32_t priority){
    for(unsigned i=0;i<3;++i)                                                  // 0014de30: +17c / +178 + 20 i
        if(look_active(c,i)!=0&&look_priority(c,i)==priority){look_active(c,i)=0;look_priority(c,i)=0;}
}
void npc_deactivate_dead(NpcAiComponent& c,bool deactivate){
    c.flags_141=static_cast<std::uint8_t>((c.flags_141&~0x04U)|(deactivate?0x04U:0U));   // 0014b680 (solo)
}
void npc_resume_ai_targeting(NpcAiHost& host){perception(host).priority_1c=0;}
void npc_resume_ai_mode(NpcAiComponent& c,NpcAiHost& host){
    if(c.priority_4c<21)return;
    c.priority_4c=0;
    auto& p=perception(host);
    request_npc_ai_mode(c,npc_resolve_handle(sense(host),p.target_40,p.target_id_44)!=0?1:0,0);
}
void npc_set_mode(NpcAiComponent& c,std::uint32_t mode){
    if((mode&0xffU)<7U)request_npc_ai_mode(c,static_cast<std::int32_t>(mode&0xffU),100);
}

void npc_forward_perception_sends(const std::vector<NpcPerceptionSend>& out,NpcAiHost& host){forward(out,host);}
void npc_snap_burst_check(NpcAiComponent& c,NpcAiHost& host){
    auto& p=perception(host);const auto& h=sense(host);
    const auto t=npc_resolve_handle(h,p.target_40,p.target_id_44);
    if(t==0)return;
    const auto f=c.perception_flags_20;
    if(bit(f,0)&&bit(f,2))return;                                            // virtual +b4 = 0014d310
    if(c.force_count_130!=0)return;
    if(!bit(f,3))return;
    if(!host.actor.secondary_3314)throw std::runtime_error("NPC AI: the snap-burst check reads the secondary component +3314 (NpcAiActor::secondary_3314)");
    const auto states=host.actor.secondary_3314();
    if(!states||!secondary_state_allows_fire((*states)[0])||!secondary_state_allows_fire((*states)[1]))return;
    const auto* r=npc_perception_sensed_record(p,h,t);
    if(!r||r->byte_39==0||!(r->level_24<1.0F)||(r->cause_20!=3&&r->cause_20!=4))return;
    if(!npc_sight_test(p,h,p.last_known_00,true,view(h,t).stealth_44>>1))return;
    npc_force_weapon_fire(c,c.ammo_a4,-1,0.0F,host);                        // virtual +198(c, +a4, -1, 0.0)
}
namespace {
void forward(const std::vector<NpcPerceptionSend>& out,NpcAiHost& host){
    for(const auto& s:out){
        NpcAiSend m;m.transport=0;
        switch(s.kind){
        case NpcPerceptionSend::Kind::ai_event:
            m.kind=NpcAiSend::Kind::component;
            m.message={NpcAiMessage::Kind::ai,s.event,s.value,s.event==7?static_cast<std::int32_t>(s.cause):0};
            break;
        case NpcPerceptionSend::Kind::new_awareness:
            m.kind=NpcAiSend::Kind::script;m.key=npc_script_new_awareness;m.object=s.object;m.cause=s.cause;
            break;
        case NpcPerceptionSend::Kind::dead_notice:
            // 0016cd7c: AIPickupableAwarenessMsg (vtable 004dcbd0, +20 2, +24 the object) to perception +3c, the component.
            m.kind=NpcAiSend::Kind::script;m.key=npc_script_pickupable_awareness;m.object=s.object;
            break;
        }
        if(!host.send)throw std::runtime_error("NPC AI host: send");
        host.send(m);
    }
}
}
}
