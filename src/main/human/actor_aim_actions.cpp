#include "actor_aim_actions.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Actor aim action (001a92a0): host ")+what+" required");
    return f;
}
// 001ae230(+3314, 0, point): slot 0 of the aim slots at +3a0 (stride 0x70).
void aim_at(ActorAimState& s,const ActorAimHost& h,const RootVector& point){
    need(h.secondary_mode,"secondary_mode")(1);                            // 001977f0(actor, 1)
    s.aim_point_3b0=point;
    s.aim_active_3a5=1;
}
// 001ae100(actor + 60, object).
void look_at(ActorAimState& s,const ActorAimHost& h,std::uint64_t object){
    s.look_2dd0=object;
    s.look_id_2dd4=object!=0?need(h.object_id,"object_id")(object):-1;
    s.look_2dec=0;
}
}

bool handle_actor_aim_action(ActorAimState& s,const ActorAimMessage& m,const ActorAimHost& h){
    switch(m.action){
    case 0x39:                                                                 // 001a9eb0
        if(m.kind==5){
            const std::uint64_t o=need(h.find_object,"find_object")(static_cast<std::int32_t>(m.word_2c));
            if(o==0)break;
            const RootVector p=need(h.point_e0,"point_e0")(o);
            if(h.secondary)aim_at(s,h,p);
        }else if(m.kind==0xb){
            const std::uint64_t o=need(h.find_object,"find_object")(m.object_30);
            if(o==0)break;
            const RootVector p=need(h.eye_c0,"eye_c0")(o,m.subtarget_34);
            if(h.secondary)aim_at(s,h,p);
        }else if(m.kind==4){
            if(h.secondary)aim_at(s,h,m.point_30);
        }else if(m.kind==1){
            need(h.secondary_mode,"secondary_mode")(static_cast<std::uint8_t>(m.word_2c&0xffU));
        }
        break;
    case 0x3a: need(h.secondary_mode,"secondary_mode")(0);break;               // 001a9ff0
    case 0x3d:                                                                 // 001aa2f0 (static)
        if(m.kind==4){
            if(h.secondary){s.head_point_420=m.point_30;s.head_active_415=1;}    // 001ae230(+3314, 1, +30)
        }else if(m.kind==0xb){
            const std::uint64_t o=need(h.find_object,"find_object")(m.object_30);
            if(o==0)throw std::runtime_error("Actor aim action (001a92a0): 0x3d kind 0xb reads +c0 of an object that does not exist");
            const RootVector p=need(h.eye_c0,"eye_c0")(o,m.subtarget_34);
            if(h.secondary){s.head_point_420=p;s.head_active_415=1;}
        }else if(m.kind==5){
            const std::uint64_t o=need(h.find_object,"find_object")(static_cast<std::int32_t>(m.word_2c));
            if(o==0)break;
            const RootVector p=need(h.point_e0,"point_e0")(o);
            if(h.secondary){s.head_point_420=p;s.head_active_415=1;}
        }else if(m.kind==1){
            if((m.word_2c&0xffU)==0&&h.secondary)s.head_active_415=0;            // 001ae150(+3314, 1)
        }
        break;
    case 0x3e: look_at(s,h,need(h.find_object,"find_object")(static_cast<std::int32_t>(m.word_2c)));break;   // 001aa420
    case 0x3f: look_at(s,h,0);break;                                            // 001aa448
    default: return false;
    }
    return true;
}

}
