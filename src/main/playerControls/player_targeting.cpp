#include "player_targeting.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Player targeting 00247d60: host ")+what+" required");
    return f;
}
// The weak-handle resolution used throughout: the object when it is null or
// its +c still equals the stored id, otherwise null.
std::uint64_t resolve(const PlayerTargetingHost& h,std::uint64_t object,std::int32_t id){
    if(object==0)return 0;
    return need(h.object_id,"object_id")(object)==id?object:0;
}
TargetHandle handle_of(const PlayerTargetingHost& h,std::uint64_t object){
    if(object==0)return {0,-1};
    return {object,need(h.object_id,"object_id")(object)};
}
// vmul.xyz, then (x*x + y*y) + z*z through the accumulator (vf3.x = 1).
float distance_squared(const RootVector& a,const RootVector& b){
    const float dx=a[0]-b[0],dy=a[1]-b[1],dz=a[2]-b[2];
    const float xx=dx*dx,yy=dy*dy,zz=dz*dz;
    return (xx+yy)+zz;
}
void rescan(std::int32_t& selection,std::uint32_t mask){
    for(int k=11;k>=0;--k)if(((mask>>k)&1U)!=0){selection=k;return;}
    selection=-1;
}
// The multiplayer record writes (005721c8 set; 132ln5h): the host's record (absent: they throw).
void record(const PlayerTargetingHost& h,const TargetingRecord& r){
    if(!h.multiplayer)return;
    if(!h.record)throw std::runtime_error("Player targeting 00247d60: the multiplayer record writes (00249420, 00249490, 002494e0) are not hosted");
    h.record(r);
}
void record_point(const PlayerTargetingHost& h,const RootVector& p){TargetingRecord r;r.kind=TargetingRecord::Kind::point;r.point=p;record(h,r);}
TargetingEvent send_event(const TargetingSend& s){TargetingEvent e;e.type=TargetingEvent::Type::send;e.send=s;return e;}
void send_look(std::vector<TargetingEvent>& ev,std::int32_t id){
    TargetingSend s;s.action=0x3e;s.kind=5;s.object_id=id;ev.push_back(send_event(s));
}
void send_clear_look(std::vector<TargetingEvent>& ev){
    TargetingSend s;s.action=0x3f;s.kind=0;ev.push_back(send_event(s));
}
void send_aim_point(std::vector<TargetingEvent>& ev,const RootVector& p){
    TargetingSend s;s.action=0x39;s.kind=4;s.point=p;ev.push_back(send_event(s));
}
// 00249580(input, target, actor).
void look_target(std::vector<TargetingEvent>& ev,const PlayerTargetingHost& h,std::uint64_t target){
    if(target!=0)send_look(ev,need(h.object_id,"object_id")(target));
    else send_clear_look(ev);
}
// 00249670(input, camera, actor): aim at camera +da0 row 3 + 100 x row 1.
void aim_view_point(std::vector<TargetingEvent>& ev,const PlayerTargetingHost& h){
    const auto m=need(h.camera_da0,"camera_da0")();
    RootVector p{};
    for(int k=0;k<4;++k){const float scaled=m[1][static_cast<std::size_t>(k)]*100.0F;p[static_cast<std::size_t>(k)]=m[3][static_cast<std::size_t>(k)]+scaled;}
    send_aim_point(ev,p);
    record_point(h,p);                                                     // 002497a0 00249490
}
std::int32_t current_item(const PlayerTargetingHost& h){return need(h.current_item,"current_item")();}

template<class F> const F& need_pick(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Target pick (0022b660 / 0022ad30): host ")+what+" required");
    return f;
}
std::uint64_t resolve_pick(const TargetPickHost& h,const TargetHandle& t){
    if(t.object==0)return 0;
    return need_pick(h.object_id,"object_id")(t.object)==t.id?t.object:0;
}
// The common candidate filter: resolved, +2d and +2c set and +2e clear, not
// the actor, an enemy (003cc8e0) and a character (+c4 = 2).
bool pick_candidate(const TargetPickHost& h,std::uint64_t object){
    if(object==0)return false;
    const auto b=need_pick(h.root_bytes,"root_bytes")(object);
    if(!(b[1]!=0&&b[0]!=0&&b[2]==0))return false;
    if(object==h.actor)return false;
    if(!need_pick(h.enemy,"enemy")(object))return false;
    return need_pick(h.kind_c4,"kind_c4")(object)==2;
}
// The stealth override: +33a8 > 0 hides everything (100), +3470 = 1 ignores stealth.
std::int32_t pick_stealth(const TargetPickHost& h,std::int32_t stealth){
    if(!(h.actor_33a8<=0.0F))return 100;
    if(h.actor_3470==1)return 0;
    return stealth;
}
float stealth_scaled(float d2,std::int32_t stealth){
    if(stealth==0)return d2;
    const float f=100.0F/static_cast<float>(100-stealth);
    return d2*(f*f);
}
RootVector frame_point(const TargetingMatrix& m,const RootVector& v){
    RootVector p{};
    for(std::size_t k=0;k<4;++k){
        const float a0=m[0][k]*v[0],a1=m[1][k]*v[1],a2=m[2][k]*v[2],a3=m[3][k]*v[3];
        p[k]=((a0+a1)+a2)+a3;
    }
    return p;
}
// 001437e0: nullopt or the hold class / range; 1, 3, 4 and 10 give no pick.
std::optional<float> pick_range_squared(const TargetPickHost& h){
    const auto rec=need_pick(h.held_record,"held_record")();
    if(!rec)return std::nullopt;
    const auto hold=static_cast<std::uint8_t>((*rec)[0]);
    if(hold==1||hold==3||hold==4||hold==10)return std::nullopt;
    const auto range=static_cast<std::int64_t>((*rec)[1]);
    const auto lo=static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(range*range)&0xffffffffU));   // mult: lo
    return static_cast<float>(lo);
}
}

TargetAngles target_angles(const TargetingMatrix& frame,const RootVector& point){
    TargetingMatrix m=frame;                                                   // 0013ee20: transpose the 3x3 rows
    std::swap(m[0][1],m[1][0]);std::swap(m[0][2],m[2][0]);std::swap(m[1][2],m[2][1]);
    if(!(m[3][0]==0.0F&&m[3][1]==0.0F&&m[3][2]==0.0F)){                         // 0013edb0
        const float tx=0.0F-m[3][0],ty=0.0F-m[3][1],tz=0.0F-m[3][2];          // 0013ed90
        RootVector t=m[3];
        for(std::size_t c=0;c<3;++c){const float a=m[0][c]*tx,b=m[1][c]*ty,d=m[2][c]*tz;t[c]=(a+b)+d;}   // 0013ed60
        m[3]=t;                                                                // 00131e70
    }
    const RootVector p=frame_point(m,point);
    const float xx=p[0]*p[0],yy=p[1]*p[1],zz=0.0F*0.0F;
    const float planar=vu::sqrt((xx+yy)+zz);
    TargetAngles r;
    r.planar=planar;
    r.angles={game_atan2f(p[2],planar),0.0F,game_atan2f(-p[0],p[1]),0.0F};
    return r;
}

std::uint64_t auto_target_pick(const TargetPickHost& h,std::uint64_t previous){
    const RootVector origin=need_pick(h.position_e4,"position_e4")(h.actor);
    const TargetingMatrix frame=need_pick(h.actor_matrix_f4,"actor_matrix_f4")();
    const auto range2=pick_range_squared(h);
    if(!range2)return 0;
    float best_d2=*range2;                                                     // f20
    std::uint64_t best=0;
    const float pi=std::bit_cast<float>(0x40490fdbU);
    for(const auto& node:h.candidates){
        const std::uint64_t obj=resolve_pick(h,node);
        if(!pick_candidate(h,obj))continue;
        if(need_pick(h.blocker,"blocker")(obj,0,false)!=0)continue;
        const RootVector eye=need_pick(h.eye_c0,"eye_c0")(obj);
        const RootVector v{eye[0]-origin[0],eye[1]-origin[1],eye[2]-origin[2],eye[3]-origin[3]};
        const auto a=target_angles(frame,h.position_e4(obj));
        const float sum=a.angles[0]*a.angles[0]+a.angles[2]*a.angles[2];
        float score=1.0F-vu::sqrt(sum)/pi;
        if(obj==previous)score=score+std::bit_cast<float>(0x3d4ccccdU);       // 0048ac54 (0.05)
        if(score<=std::bit_cast<float>(0x3f59999aU))continue;                  // 0048ac4c (0.85)
        const float d2=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];
        const std::int32_t stealth=pick_stealth(h,need_pick(h.stealth_44,"stealth_44")(obj));
        if(stealth>=100)continue;
        const float scaled=stealth_scaled(d2,stealth);
        if(scaled<best_d2){best_d2=scaled;best=obj;}
    }
    return best;
}

std::uint64_t lock_target_pick(const TargetPickHost& h,std::uint64_t current,std::vector<TargetHandle>& recent){
    const TargetingMatrix aframe=need_pick(h.actor_frame_d8,"actor_frame_d8")();
    const RootVector eye_a=frame_point(aframe,{0.0F,0.0F,5.0F,1.0F});           // sp+220 = (0, 0, 5, 1)
    TargetingMatrix camera=need_pick(h.camera_frame_d8,"camera_frame_d8")();   // sp+190
    camera[3]=eye_a;                                                           // 0022ae7c: sq to sp+1c0 replaces the camera's row 3
    const auto range2=pick_range_squared(h);
    if(!range2)return 0;
    const float far2=*range2;                                                  // f25
    const float near2=std::bit_cast<float>(0x40a00000U)*std::bit_cast<float>(0x40a00000U);   // 0048ac38 (5.0) squared
    const float pi=std::bit_cast<float>(0x40490fdbU);
    std::vector<std::uint64_t> objects;std::vector<float> scores;               // sp+150 / sp+110
    bool current_scored=false;                                                 // s7
    for(const auto& node:h.candidates){
        const std::uint64_t obj=resolve_pick(h,node);
        if(!pick_candidate(h,obj))continue;
        const std::int32_t raw=need_pick(h.stealth_44,"stealth_44")(obj);
        if(need_pick(h.actor_flag_346c,"actor_flag_346c")(obj))continue;
        const std::int32_t stealth=pick_stealth(h,raw);
        if(stealth>=100)continue;
        const RootVector eye=need_pick(h.eye_c0,"eye_c0")(obj);
        const RootVector v{eye[0]-eye_a[0],eye[1]-eye_a[1],eye[2]-eye_a[2],eye[3]-eye_a[3]};
        const float d2=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];                         // f23
        const float scaled=stealth_scaled(d2,stealth);                         // f22
        float score=0.0F;
        if(scaled<far2){
            const auto a=target_angles(camera,need_pick(h.position_e4,"position_e4")(obj));
            const float facing=1.0F-std::fabs(a.angles[2])/pi;                 // f20
            const float closeness=(std::bit_cast<float>(0x3e800000U)*(far2-scaled))/far2;   // 0048ac24 (0.25)
            int tier=4;                                                        // s5
            if(!(facing<=std::bit_cast<float>(0x3f400000U)))tier=1;            // 0048ac3c (0.75)
            else if(scaled<near2)tier=1;
            else if(!(facing<=std::bit_cast<float>(0x3f000000U)))tier=2;       // 0048ac40 (0.5)
            else if(!(facing<=std::bit_cast<float>(0x3e19999aU)))tier=3;       // 0048ac44 (0.15)
            if(const auto c=need_pick(h.controller,"controller")(obj)){
                if(c->aim_target==h.actor&&d2<200.0F&&!(c->progress<=0.75F))tier=1;
            }
            const float tier_score=((4.0F-static_cast<float>(tier))/3.0F)*std::bit_cast<float>(0x3f99999aU);   // 0048ac34 (1.2)
            const float facing_score=facing*std::bit_cast<float>(0x3e4ccccdU);  // 0048ac2c (0.2)
            int position=-1;
            for(std::size_t k=0;k<recent.size();++k)
                if(resolve_pick(h,recent[k])==obj){position=static_cast<int>(k);break;}
            const float recency=(static_cast<float>(5-position)/6.0F)*std::bit_cast<float>(0x41a00000U);   // 0048ac28 (20)
            score=facing_score+(recency+(closeness+tier_score));
        }
        if(score<=0.0F)continue;
        if(obj==current){current_scored=true;continue;}
        if(objects.size()==16)throw std::runtime_error("Lock pick 0022ad30: more than 16 scored candidates overflow the original's stack arrays");
        objects.push_back(obj);scores.push_back(score);
    }
    for(;;){
        int chosen=-1;
        for(std::size_t k=0;k<objects.size();++k){
            if(chosen<0){if(!(scores[k]<=0.0F))chosen=static_cast<int>(k);}
            else if(!(scores[k]<=scores[static_cast<std::size_t>(chosen)]))chosen=static_cast<int>(k);
        }
        if(chosen<0)return current_scored?current:0;
        const auto c=static_cast<std::size_t>(chosen);
        scores[c]=-1.0F;
        if(need_pick(h.blocker,"blocker")(objects[c],0,true)==0){
            const TargetHandle t{objects[c],h.object_id(objects[c])};
            std::vector<TargetHandle> kept;                                    // 0022d740: drop entries resolving to it
            for(const auto& r:recent)if(resolve_pick(h,r)!=objects[c])kept.push_back(r);
            kept.push_back(t);                                                 // 00222980: at the back
            recent=std::move(kept);
            return objects[c];
        }
    }
}

void targeting_lock_timer(PadWeaponState& pad,PlayerTargetingState& s,float dt,std::vector<TargetingEvent>& ev){
    if(!(s.lock_time_e8<=0.0F)){                                               // 00249344 c.ole.s
        const float left=s.lock_time_e8-dt;
        s.lock_time_e8=left<0.0F?0.0F:left;                                    // max.s(0, x)
        return;
    }
    TargetingEvent c;c.type=TargetingEvent::Type::clear_recent;ev.push_back(c);   // 00177f60
    s.pick_c8={0,-1};
    pad.subtarget_e4=-1;
}

std::vector<TargetingEvent> run_player_targeting(PadWeaponState& pad,PlayerTargetingState& s,bool r1,bool l1,float dt,
    const PlayerTargetingHost& h){
    std::vector<TargetingEvent> ev;
    const std::uint64_t previous=resolve(h,pad.lock_c0,pad.lock_id_c4);        // s6
    bool auto_picked=false;                                                    // s3
    s.forward_119=0;
    auto set_lock=[&](std::uint64_t object){const auto t=handle_of(h,object);pad.lock_c0=t.object;pad.lock_id_c4=t.id;};
    auto pick=[&]{return resolve(h,s.pick_c8.object,s.pick_c8.id);};
    if(s.selector_38==8){                                                      // 00247e14: the aim view
        targeting_lock_timer(pad,s,dt,ev);
        if(current_item(h)<0){
            pad.lock_c0=0;pad.lock_id_c4=-1;
            TargetingEvent c;c.type=TargetingEvent::Type::hud_clear_960;c.value=0.0F;ev.push_back(c);
        }else{
            if(need(h.hold_class,"hold_class")()==1){
                TargetingEvent c;c.type=TargetingEvent::Type::thrown_point;ev.push_back(c);   // 002497c0
                pad.lock_c0=0;pad.lock_id_c4=-1;
            }else set_lock(need(h.aim_view_pick,"aim_view_pick")());          // 00249b10
            aim_view_point(ev,h);                                              // 00249670
        }
        look_target(ev,h,resolve(h,pad.lock_c0,pad.lock_id_c4));               // 00249580
        pad.group_mask_44&=~0x20U;
        rescan(pad.group_40,pad.group_mask_44);
    }else{
        if(r1&&s.recheck_failed_bc==0){                                        // 00248088
            if(s.r1_a8==0||pick()==0){                                         // 00248100: a new pick
                const auto t=handle_of(h,need(h.lock_pick,"lock_pick")(pick()));
                s.pick_c8=t;
                pad.subtarget_e4=-1;
            }else{
                const std::uint64_t target=pick();
                const auto bytes=need(h.root_bytes,"root_bytes")(target);
                bool keep=bytes[1]!=0&&bytes[0]!=0&&bytes[2]==0;
                if(keep&&need(h.blocker,"blocker")(target,50,false)!=0)keep=false;   // 0048aed0 (50 frames)
                if(!keep){
                    s.pick_c8={0,-1};
                    s.recheck_failed_bc=1;
                }else{
                    const float d2=distance_squared(need(h.position_e4,"position_e4")(h.actor),h.position_e4(target));
                    if(!(d2<=32.0F*32.0F))pad.subtarget_e4=-1;                  // 0048aed8 (32.0)
                    else if(need(h.blocker,"blocker")(pick(),0,false)!=0)pad.subtarget_e4=-1;
                }
            }
            s.lock_time_e8=std::bit_cast<float>(0x3e800000U);                 // 004a7550 (boot image 0.25)
        }else{
            if(!r1)s.recheck_failed_bc=0;
            targeting_lock_timer(pad,s,dt,ev);
        }
        if(pick()!=0&&l1&&need(h.agent_unlock_2,"agent_unlock_2")()){         // 00248530: the subtarget cycle
            if(need(h.blocker,"blocker")(pick(),0,false)==0){
                const std::uint64_t target=pick();
                const float d2=distance_squared(need(h.position_e4,"position_e4")(h.actor),h.position_e4(target));
                if(d2<32.0F*32.0F){
                    const std::int32_t next=pad.subtarget_e4+1;
                    pad.subtarget_e4=next;
                    if(next==need(h.subtarget_count_d0,"subtarget_count_d0")(pick()))pad.subtarget_e4=-1;
                }
            }
        }
        pad.lock_c0=s.pick_c8.object;pad.lock_id_c4=s.pick_c8.id;               // 00248790
        const float recent=std::bit_cast<float>(0x3e800000U)-std::bit_cast<float>(0x3e4ccccdU);   // 004a7550 - 004a7558
        if(pick()!=0&&!(s.lock_time_e8<=recent))pad.group_mask_44|=0x20U;
        else pad.group_mask_44&=~0x20U;
        rescan(pad.group_40,pad.group_mask_44);
        if(resolve(h,pad.lock_c0,pad.lock_id_c4)==0){                          // 00248968: the automatic pick
            set_lock(need(h.auto_pick,"auto_pick")(previous));
            auto_picked=resolve(h,pad.lock_c0,pad.lock_id_c4)!=0;
        }
        std::int32_t shown=-1;                                                 // sp+30c
        if(auto_picked||(pick()!=0&&((pad.selector_mask_3c>>5)&1U)!=0)){
            const std::uint64_t target=resolve(h,pad.lock_c0,pad.lock_id_c4);
            shown=h.object_id(target);
            send_look(ev,shown);
            TargetingSend a;a.action=0x39;a.kind=0xb;a.object_id=shown;a.subtarget=pad.subtarget_e4;
            ev.push_back(send_event(a));
            TargetingRecord r;r.kind=TargetingRecord::Kind::lock;r.object=target;r.shown=shown;r.subtarget=pad.subtarget_e4;
            record(h,r);                                                       // 00248cd8 002494e0, 00249420(1)
        }else{
            s.forward_119=1;                                                   // 00248d00
            const bool kind5=need(h.item_kind,"item_kind")()==5;
            const bool aim=kind5||need(h.pointer_33ec,"pointer_33ec")()||!(pad.timer_11c<=0.0F);
            if(!aim){
                send_clear_look(ev);
                TargetingSend a;a.action=0x39;a.kind=1;a.byte_2c=0;           // 0048aee8 (boot image 0)
                ev.push_back(send_event(a));
                TargetingRecord r;r.kind=TargetingRecord::Kind::lower;record(h,r);   // 00248ff0 00249420(0)
            }else{
                const float z=(kind5||!(pad.timer_11c<=0.0F))?0.0F:-std::bit_cast<float>(0x443b8000U);   // 0048aee0 (750.0)
                const auto m=need(h.actor_matrix_f4,"actor_matrix_f4")();
                const RootVector v{0.0F,5000.0F,z,0.0F};
                RootVector p{};
                for(std::size_t k=0;k<4;++k){
                    const float a0=m[0][k]*v[0],a1=m[1][k]*v[1],a2=m[2][k]*v[2],a3=m[3][k]*v[3];
                    p[k]=((a0+a1)+a2)+a3;
                }
                send_clear_look(ev);
                send_aim_point(ev,p);
                record_point(h,p);                                             // 00248efc 00249490
            }
        }
        TargetingEvent hud;                                                    // 00248ff8
        if(need(h.hud_target_changed,"hud_target_changed")())hud.type=TargetingEvent::Type::hud_changed_240;
        else if(!auto_picked&&shown!=-1){hud.type=TargetingEvent::Type::hud_target_380;hud.id=shown;hud.subtarget=pad.subtarget_e4;}
        else hud.type=TargetingEvent::Type::hud_none_280;
        ev.push_back(hud);
    }
    s.r1_a8=static_cast<std::uint8_t>(r1?1:0);                                 // 00249088
    std::uint64_t target=0;
    if(auto_picked||r1)target=resolve(h,pad.lock_c0,pad.lock_id_c4);
    const auto aim=need(h.aim_update,"aim_update")(target,current_item(h),pad.subtarget_e4);
    if(!auto_picked&&resolve(h,pad.lock_c0,pad.lock_id_c4)!=0&&s.selector_38!=8){
        TargetingEvent c;c.type=TargetingEvent::Type::hud_aim_2e0;c.value=aim[0]*aim[1];ev.push_back(c);
    }
    if(s.forward_119!=0){
        if(s.selector_38==5)pad.timer_11c=std::bit_cast<float>(0x3f000000U);   // 004a7590 (boot image 0.5)
        else{const float left=pad.timer_11c-dt;pad.timer_11c=left<0.0F?0.0F:left;}
    }else pad.timer_11c=std::bit_cast<float>(0x3f000000U);
    return ev;
}

}
