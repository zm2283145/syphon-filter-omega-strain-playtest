#include "npc_perception_think.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned i){return ((v>>i)&1U)!=0;}
void set_bit(std::uint8_t& v,unsigned i,bool on){
    v=static_cast<std::uint8_t>((v&~(1U<<i))|((on?1U:0U)<<i));
}
NpcSenseObject need(const NpcPerceptionHost& h,std::uint64_t object){
    if(!h.object)throw std::runtime_error("NPC perception: no object views (NpcPerceptionHost::object)");
    auto v=h.object(object);
    if(!v)throw std::runtime_error("NPC perception: an object the perception reads is not known to the host");
    return *v;
}
NpcVisibilityQuery& visibility(const NpcPerceptionHost& h){
    if(!h.visibility)throw std::runtime_error("NPC perception: the target manager's visibility cache 0022cb10 is not hosted (NpcVisibilityQuery)");
    return *h.visibility;
}
// 0016db40: the first record whose handle resolves to the object.
std::size_t find_record(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    for(std::size_t i=0;i<p.records.size();++i)
        if(npc_resolve_handle(h,p.records[i].object,p.records[i].id_4)==object)return i;
    return p.records.size();
}
std::uint64_t target_of(const NpcPerception& p,const NpcPerceptionHost& h){return npc_resolve_handle(h,p.target_40,p.target_id_44);}
NpcPerceptionSend ai_event(std::uint8_t event,std::int32_t value,std::uint8_t cause){
    NpcPerceptionSend s;s.kind=NpcPerceptionSend::Kind::ai_event;s.event=event;s.value=value;s.cause=cause;return s;
}
}

bool npc_sense_present(const NpcSenseObject& o){return o.root[1]!=0&&o.root[0]!=0&&o.root[2]==0;}

NpcSenseObject npc_node_sense_object(std::int32_t id,const std::array<float,3>& position){
    NpcSenseObject v;
    v.id=id;v.life_108=3;v.root={1,0,0};v.actor_class=false;v.class_4ea770=true;
    v.flag_3468=0;v.byte_14=1;v.stealth_44=0;
    v.position={position[0],position[1],position[2],1.0F};v.eye=v.position;
    v.weapon_ready_d4=true;v.kind_5b0=false;v.kind_580=false;
    v.controller=NpcSenseObject::Controller::none;v.team=-1;
    return v;
}

std::uint64_t npc_resolve_handle(const NpcPerceptionHost& h,std::uint64_t object,std::int32_t id){
    if(object==0)return 0;
    if(!h.object)throw std::runtime_error("NPC perception: no object views (NpcPerceptionHost::object)");
    const auto v=h.object(object);
    return v&&v->id==id?object:0;
}
bool npc_is_enemy(const NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object){
    if(c.team_54<0||object==0)return false;                                  // virtual +68 = 0015b800
    const auto team=need(h,object).team;                                     // 003cc950
    if(team<0)return false;
    if(!h.teams)throw std::runtime_error("NPC perception: IsEnemy needs the team matrix *004ee620");
    return h.teams->enemy(c.team_54,team);                                   // range-checked like 0022f3b8..0022f3d8
}

void npc_perception_reset(NpcPerception& p,float far_distance,std::uint32_t frame){
    p.half_angle_14=std::bit_cast<float>(0x3f490fdbU);                       // pi / 4
    p.radius_10=far_distance;                                                // 0053834c
    p.start_frame_50=frame+20U;                                              // 005383c8 + 20
}

bool npc_perception_ignores(const NpcPerceptionHost& h,std::uint64_t object){
    return object!=0&&h.port_ignored&&h.port_ignored(object);
}

void npc_perception_add(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object,std::uint8_t cause,float value,
    std::vector<NpcPerceptionSend>& out){
    if(npc_perception_ignores(h,object))return;                              // port-only: no awareness of an ignored object
    const auto o=need(h,object);
    AwarenessObjectView v;
    v.object=object;v.id=o.id;v.life_108=o.life_108;v.actor_class=o.actor_class;v.root=o.root;v.flag_3468=o.flag_3468;v.position=o.position;
    std::vector<PerceptionSend> sends;
    perception_add_awareness(p,v,cause,value,sends);                         // 0016c9d0
    for(const auto& s:sends){NpcPerceptionSend d;d.kind=NpcPerceptionSend::Kind::dead_notice;d.object=s.object;out.push_back(d);}
}

bool npc_sight_test(const NpcPerception& p,const NpcPerceptionHost& h,const RootVector& point,bool head,std::int32_t stealth){
    if(!(h.blind_33a8<=0.0F))return false;                                   // c.ole.s +33a8, 0
    const bool node=head&&h.self_root[1]!=0&&h.self_root[0]!=0&&h.self_root[2]==0;
    const RootVector& eye=node?h.self_head[3]:h.self_eye;                    // node 14 row 3 / virtual +c0(-1)
    const float vx=point[0]-eye[0],vy=point[1]-eye[1],vz=point[2]-eye[2];   // vsub.xyzw
    const float d=vu::sqrt((vx*vx+vy*vy)+vz*vz);                             // vmul.xyz, vmulax.w / vmadday.w / vmaddz.w, vsqrt
    const float q=vu::div(1.0F,d);                                           // vdiv Q, vf0.w, d
    const float nx=vx*q,ny=vy*q,nz=vz*q;                                     // vmulq.xyz
    float r=p.radius_10;
    if(stealth>0)r=r*((100.0F-static_cast<float>(stealth))/100.0F);
    if(!(d<r))return false;
    if(d==0.0F)return false;
    if(d<=1.0F)return true;
    const RootVector& f=node?h.self_head[1]:h.self_forward;                  // node 14 row 1 / virtual +e8 row 1
    const float dot=(f[0]*nx+f[1]*ny)+f[2]*nz;                               // vmul.xyzw, vmulax.w / vmadday.w / vmaddz.w
    const float cs=game_cosf(p.half_angle_14);                               // 00121688
    return !((dot+(1.0F-cs))<=1.0F);
}

void npc_perception_set_last_known(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,
    const RootVector& position,bool seen,std::optional<float> f20){
    bool update=true;
    std::int32_t region=-1;
    NpcSenseObject::Controller kind=NpcSenseObject::Controller::none;
    std::int32_t team=-1;
    if(object!=0){
        const auto o=need(h,object);
        kind=o.controller;team=o.team;
        if(kind==NpcSenseObject::Controller::player)update=!bit(o.flags_3204,7);
        else if(kind==NpcSenseObject::Controller::ai)region=o.region_5c;     // the AI's path follower +5c
    }
    if(!update)return;
    if(kind==NpcSenseObject::Controller::player){
        if(!h.player_region)throw std::runtime_error("NPC perception: 0016c030 needs the per-player region table 004ed9c0");
        region=h.player_region(team);                                       // 004ed9c0[003cc950(object)]
    }
    if(region<0){
        if(!h.region_at)throw std::runtime_error("NPC perception: 0016c030 needs the region at the last known position (0015d790)");
        region=h.region_at(p.last_known_00,p.region_18,f20,"0016c15c: 0016c030's caller's f20");   // reads +00 before it is replaced
    }
    p.region_18=region;
    p.last_known_00=position;
    set_bit(c.perception_flags_20,4,true);
    if(seen)set_bit(c.perception_flags_20,3,true);
}

void npc_perception_set_target(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,std::int32_t priority,
    std::optional<float> f20){
    if(object!=0&&object==target_of(p,h))return;
    if(npc_perception_ignores(h,object))return;                              // port-only: never target an ignored object
    if(priority<p.priority_1c)return;
    p.target_40=object;
    p.target_id_44=object!=0?need(h,object).id:-1;
    p.priority_1c=priority;
    for(unsigned b:{3U,4U,0U,1U,2U,6U})set_bit(c.perception_flags_20,b,false);
    if(object==0)return;
    const auto i=find_record(p,h,object);
    if(i<p.records.size()){
        set_bit(c.perception_flags_20,3,(p.records[i].byte_3a&1U)!=0);
        const RootVector position=p.records[i].position_10;
        npc_perception_set_last_known(p,c,h,object,position,false,f20); // 0016df58: 0016ddb0 leaves f20 as its caller's
    }
    if(need(h,object).controller==NpcSenseObject::Controller::none)visibility(h).register_target(object);   // 0022c650 when +58 is null
}

void npc_perception_update_records(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,float dt,
    std::vector<NpcPerceptionSend>& out){
    const RootVector self_eye=h.self_eye;                                    // virtual +c0(-1), once
    // Checkpoint 132uk3: f20 through the record loop (the caller's f20 at the two 0016c030 calls 0016d440 /
    // 0016d4ec): 0016b580's dt (0016cf10 does not write it before) until a record reaches 0016d7d4, which
    // leaves max.s(+10, 1.0) (0016d7e0) for the following records.
    float f20=dt;
    std::size_t i=0;
    while(i<p.records.size()){
        const std::uint64_t object=npc_resolve_handle(h,p.records[i].object,p.records[i].id_4);
        bool remove=object==0||npc_perception_ignores(h,object);               // port-only: drop an ignored object's record
        NpcSenseObject o{};
        if(!remove){
            o=need(h,object);
            if(o.life_108==3&&!(npc_sense_present(o)&&o.actor_class&&o.flag_3468!=0))remove=true;
        }
        if(remove){
            // 00138da0 leaves the iterator on the next node and the loop then
            // advances once more: the record after a removed one is skipped,
            // and removing the last one restarts the walk at the first.
            p.records.erase(p.records.begin()+static_cast<std::ptrdiff_t>(i));
            i=i<p.records.size()?i+1:0;
            continue;
        }
        const bool present=npc_sense_present(o);
        {
            auto& r=p.records[i];
            if(present){
                r.score_30=r.score_30*std::bit_cast<float>(0x3f4ccccdU);          // 0.8
                r.score_34=r.score_34*std::bit_cast<float>(0x3f666666U);          // 0.9
            }else{r.byte_38=0;r.byte_39=0;r.score_30=0.0F;r.score_34=0.0F;}
        }
        const float old=p.records[i].level_24;
        if(old<=0.0F||p.records[i].byte_39==0)p.records[i].level_24=p.records[i].level_24+dt;
        if(old<=0.0F&&!(p.records[i].level_24<=0.0F)&&o.life_108!=2&&o.life_108!=3){
            const auto target=target_of(p,h);
            if(target==0){
                if(npc_is_enemy(c,h,object))npc_perception_set_last_known(p,c,h,object,o.position,false,f20);   // 0016d440
            }else if(need(h,target).id==o.id){
                const bool seen=(p.records[i].byte_39&p.records[i].byte_38)!=0;
                npc_perception_set_last_known(p,c,h,target,o.position,seen,f20);   // 0016d4ec
            }
            if(p.records[i].byte_3c==0||!(p.records[i].time_28<=1.0F)){
                out.push_back(ai_event(7,o.id,p.records[i].cause_20));          // 004ee550 +28 ID, +2c cause
                p.records[i].time_28=0.0F;
                if(p.records[i].byte_3c==0){
                    bool kind=true;
                    if(o.controller==NpcSenseObject::Controller::player)kind=o.kind_580;   // virtual +f8(004ee580)
                    if(!kind)
                        for(const auto slot:h.player_slots){                    // 003fb8d0(0..3)
                            if(slot==0||!need(h,slot).kind_580)continue;
                            const auto before=p.records.size();
                            npc_perception_add(p,h,slot,9,0.0F,out);            // a new record goes to the front
                            i+=p.records.size()-before;
                        }
                }
            }
            if(p.records[i].byte_3c==0){
                const bool to_scripts=npc_is_enemy(c,h,object)||
                    (o.controller==NpcSenseObject::Controller::player&&(p.flags_21&1U)!=0);
                if(to_scripts){
                    NpcPerceptionSend s;s.kind=NpcPerceptionSend::Kind::new_awareness;s.object=object;s.cause=p.records[i].cause_20;
                    out.push_back(s);
                    p.records[i].byte_3c=1;
                }
            }
        }
        auto& r=p.records[i];
        float visibility_score=0.0F;
        if(r.byte_39!=0){
            if(r.byte_38!=0)r.position_10=o.position;
            if(npc_is_enemy(c,h,object)){
                const float f=p.radius_10<1.0F?1.0F:p.radius_10;                  // max.s +10, 1.0
                f20=f;                                                           // 0016d7d4 / 0016d7e0: the register itself
                const float dx=o.eye[0]-self_eye[0],dy=o.eye[1]-self_eye[1],dz=o.eye[2]-self_eye[2];
                const float d=(dx==0.0F&&dy==0.0F&&dz==0.0F)?std::bit_cast<float>(0x3dcccccdU):vu::sqrt((dx*dx+dy*dy)+dz*dz);
                float q=d/f;
                if(!(q<=1.0F))q=1.0F;
                visibility_score=1.0F-q;
                const std::int32_t stealth=h.flag_3470==1?0:o.stealth_44;
                if(stealth>0)visibility_score=visibility_score*((100.0F-static_cast<float>(stealth>>1))/100.0F);
                // 0016d918..0016d95c: the held weapon's far range (0013faf0) of
                // an armed actor-class object is computed and not used.
                float t=d/f;
                if(!(t<=1.0F))t=1.0F;
                t=1.0F-t;
                if(o.controller!=NpcSenseObject::Controller::none){
                    if(o.aim_target==h.self)t=t*std::bit_cast<float>(0x3f99999aU);   // 003ea550: aimed at this NPC, 1.2
                    if(o.weapon_ready_d4)t=t*std::bit_cast<float>(0x3fb33333U);        // virtual +d4, 1.4
                }
                r.score_30=t;
            }
        }
        r.score_2c=visibility_score;
        r.time_28=r.time_28+dt;
        ++i;
    }
}

void npc_perception_register_sighting(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object,bool clear,bool view,
    std::vector<NpcPerceptionSend>& out){
    const bool both=clear&&view;
    const auto i=find_record(p,h,object);
    if(i==p.records.size()){
        if(both)npc_perception_add(p,h,object,6,0.0F,out);
        return;
    }
    auto& r=p.records[i];
    if(both&&r.byte_3c==0){npc_perception_add(p,h,object,6,0.0F,out);return;}
    r.byte_38=view?1:0;r.byte_39=clear?1:0;
    if(!clear)return;
    if(view){r.level_24=0.0F;r.byte_3a=1;r.cause_20=6;}
    else if(r.level_24<10.0F)r.level_24=0.0F;
}

const AwarenessRecord* npc_perception_sensed_record(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    const auto i=find_record(p,h,object);
    if(i==p.records.size())return nullptr;
    return p.records[i].level_24<0.0F?nullptr:&p.records[i];
}
float npc_perception_threat(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    const auto* r=npc_perception_sensed_record(p,h,object);
    return r?r->score_30:0.0F;
}

std::uint64_t npc_perception_next_candidate(const NpcPerception& p,const NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t previous){
    const auto& list=h.think_list;
    std::size_t start=0;
    if(previous!=0&&need(h,previous).controller==NpcSenseObject::Controller::ai){
        // Virtual +88 of a cNPC is 1: the entry after the previous one, or
        // the first when it is not listed. (A player's +88 only matters in
        // the same walk; a player is never on the think list, so the walk
        // starts at the first entry either way.)
        for(std::size_t k=0;k<list.size();++k)
            if(npc_resolve_handle(h,list[k].first,list[k].second)==previous){start=k+1;break;}
    }
    const auto candidate=[&](std::size_t k)->std::uint64_t{
        const auto o=npc_resolve_handle(h,list[k].first,list[k].second);
        if(o==0||o==h.self)return 0;
        const auto v=need(h,o);
        if(v.life_108!=3)return npc_is_enemy(c,h,o)?o:0;
        if(!(v.actor_class&&npc_sense_present(v)&&v.flag_3468!=0))return 0;
        const auto* r=npc_perception_sensed_record(p,h,o);
        return (!r||r->dead_notice_3b==0)?o:0;
    };
    for(std::size_t k=start;k<list.size();++k)if(const auto o=candidate(k))return o;
    const auto player=h.local_player;                                        // single player: *004ffc0c +30
    if(player!=0&&!npc_perception_ignores(h,player)&&(npc_is_enemy(c,h,player)||(p.flags_21&1U)!=0))return player;   // port-only: skip an ignored player
    for(std::size_t k=0;k<list.size();++k)if(const auto o=candidate(k))return o;
    return previous;
}

void npc_perception_visibility_test(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,
    std::vector<NpcPerceptionSend>& out){
    if(npc_perception_ignores(h,object))return;                              // port-only: an ignored object is never seen
    const auto o=need(h,object);
    bool clear=false;
    if(npc_sense_present(o)){
        bool skip=false;
        if(o.controller==NpcSenseObject::Controller::ai)skip=o.contact_158==0;   // its virtual +58 / +5c, +158
        if(!skip)skip=c.contact_158==0;                                          // this NPC's +58 (virtual +58 is 1)
        if(!skip&&o.controller==NpcSenseObject::Controller::player&&o.byte_14==0)skip=bit(o.flags_3034,0);
        if(!skip)clear=visibility(h).blocker(h.self,object)==0;                 // 0022cb10(004f77c0, actor, object, 0, 0)
    }
    const std::int32_t stealth=h.flag_3470==1?0:o.stealth_44;
    const bool view=npc_sight_test(p,h,o.eye,true,stealth);
    if(object==target_of(p,h)){
        auto& f=c.perception_flags_20;
        set_bit(f,2,clear);set_bit(f,0,view);
        set_bit(f,1,npc_sight_test(p,h,o.eye,false,stealth));
        const bool both=bit(f,0)&&bit(f,2);
        if(bit(f,6)!=both){
            out.push_back(ai_event(4,both?1:0,0));                            // +2c not written
            set_bit(f,6,both);
        }
    }
    npc_perception_register_sighting(p,h,object,clear,view,out);
    if(clear&&view)return;
    if(!o.actor_class||o.lit_spot==0)return;                                 // 00199700
    if(visibility(h).blocker(h.self,o.lit_spot)!=0)return;
    if(npc_sight_test(p,h,o.lit_spot_position,true,0))npc_perception_register_sighting(p,h,object,true,true,out);
}

void npc_perception_think(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,float dt,std::vector<NpcPerceptionSend>& out){
    if(const auto t=target_of(p,h)){
        // Port-only: a target that became ignored is dropped at any priority.
        // (The original's ordinary drop leaves priority 20.)
        if(npc_perception_ignores(h,t)){npc_perception_set_target(p,c,h,0,std::max(p.priority_1c,20));p.priority_1c=20;}
        else{
            const auto o=need(h,t);
            if(!npc_sense_present(o)||o.life_108==3||!npc_is_enemy(c,h,t))npc_perception_set_target(p,c,h,0,20);
        }
    }
    npc_perception_update_records(p,c,h,dt,out);
    // Checkpoint 132uk3: 0016b580's f20 (0016b5c8: dt; 0016cf10 restores its own). The scoring loop writes it
    // for each record past the enemy test (0016b7d0 0; 0016b894 the sum; 0016b8dc x1.1), and 0016b94c passes
    // it on to 0016ddb0 -> 0016c030.
    float f20=dt;
    float best=0.0F;std::uint64_t chosen=0;
    for(const auto& r:p.records){
        const auto object=npc_resolve_handle(h,r.object,r.id_4);
        if(object==0)continue;
        const auto o=need(h,object);
        if(!npc_sense_present(o)||o.life_108==2||o.life_108==3||!npc_is_enemy(c,h,object))continue;
        f20=0.0F;                                                                     // 0016b7d0 mtc1 zero
        if(o.controller==NpcSenseObject::Controller::player&&!o.kind_5b0)continue;   // virtual +f8(004ee5b0)
        float score=0.0F;
        if(r.level_24<10.0F)score=r.score_34+(r.score_30+r.score_2c);
        if(object==target_of(p,h))score=score*std::bit_cast<float>(0x3f8ccccdU);    // 1.1
        f20=score;                                                                    // 0016b894 / 0016b8dc
        if(!(score<=best)){best=score;chosen=object;}
    }
    if(!(best<=0.0F))npc_perception_set_target(p,c,h,chosen,20,f20);           // 0016b94c
    else if(const auto t=target_of(p,h);t!=0&&find_record(p,h,t)==p.records.size())npc_perception_set_target(p,c,h,0,20);
    bool tested=false;
    if(const auto t=target_of(p,h);t!=0&&!bit(c.perception_flags_20,7)){
        npc_perception_visibility_test(p,c,h,t,out);
        tested=true;
        set_bit(c.perception_flags_20,7,true);
    }
    if(tested)return;
    const auto candidate=npc_perception_next_candidate(p,c,h,npc_resolve_handle(h,p.candidate_48,p.candidate_id_4c));
    if(candidate!=0){
        p.candidate_48=candidate;
        p.candidate_id_4c=need(h,candidate).id;
        npc_perception_visibility_test(p,c,h,candidate,out);
    }
    set_bit(c.perception_flags_20,7,false);
}

void npc_perception_remove(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    if(object==0)return;                                                     // 0016db40: a null object finds nothing
    const auto i=find_record(p,h,object);
    if(i<p.records.size())p.records.erase(p.records.begin()+static_cast<std::ptrdiff_t>(i));   // 0036b130 + 0015af40 / 00138da0
}

void npc_perception_resend(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    for(auto& r:p.records)                                                   // 0016c5f8..0016c67c: every record
        if(object==0||npc_resolve_handle(h,r.object,r.id_4)==object)r.byte_3c=0;
}

bool npc_perception_in_view_cone(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object){
    if(object==0)throw std::runtime_error("NPC perception: 0016c1e0 with a null object (the original calls through a null vtable)");
    if(npc_perception_ignores(h,object))return false;                        // port-only: an ignored object is never seen
    const auto o=need(h,object);                                             // virtual +c0(-1), +44
    const std::int32_t stealth=h.flag_3470==1?0:o.stealth_44;                // 0016c214..0016c22c
    return npc_sight_test(p,h,o.eye,true,stealth);                           // 0016c260(p, eye, 1, stealth)
}

float npc_view_cone_half_angle(float angle){
    return std::bit_cast<float>(0x3c8efa35U)*(0.5F*angle);                   // 001492f0: (pi / 180) * (0.5 * f12)
}
}
