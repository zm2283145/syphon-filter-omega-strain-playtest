#include "actor_stun.hpp"

#include <stdexcept>

namespace omega {
namespace {

template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("actor stun: ")+what+" is not hosted");
    return f;
}

}  // namespace

void start_actor_stun(ActorStunState& state,std::uint64_t lethal,std::uint64_t notify,float duration,float f13,
    const ActorStunHost& host){
    if(state.elapsed_33bc<0.0f){
        if(notify!=0)need(host.replicate,"the stun message 004ef078")(static_cast<std::uint8_t>(lethal),duration,f13);
        need(host.effect,"the shock effect 00185d40")(stun_effect_slot,1,1.0f);
        need(host.sound,"the stun sound 0020f990")(stun_sound);
        need(host.action,"the stun action message 0055d480")(stun_action_24,stun_action_kind_28,stun_start_code);
        state.elapsed_33bc=0.0f;
        state.duration_33c0=duration;
        state.lethal_33c8=static_cast<std::uint8_t>(lethal);
        return;
    }
    if(!(duration<=state.duration_33c0))state.duration_33c0=duration;
    if(lethal!=0)state.lethal_33c8=1;
}

bool update_actor_stun(ActorStunState& state,float dt,const ActorStunHost& host){
    if(state.elapsed_33bc<0.0f)return false;
    state.elapsed_33bc+=dt;
    if(state.elapsed_33bc<=state.duration_33c0)return false;
    state.elapsed_33bc=-1.0f;
    need(host.effect,"the shock effect 00185d40")(stun_effect_slot,0,1.0f);
    need(host.action,"the stun action message 0055d480")(stun_action_24,stun_action_kind_28,stun_end_code);
    need(host.stop_sound,"the stun sound stop 0020eba0")(stun_sound);
    if(state.lethal_33c8!=0)need(host.damage,"the stun damage (virtual +100)")(1,4,stun_lethal_damage);
    else need(host.damage,"the stun damage (virtual +100)")(1,7,0.0f);
    return true;
}

}  // namespace omega
