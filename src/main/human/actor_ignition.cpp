#include "actor_ignition.hpp"

#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F>
const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Actor ignition (00186360): ")+what+" is not hosted");
    return f;
}
std::uint32_t key(std::uint64_t source,const ActorIgnitionHost& h){
    return source?need(h.key_of,"the source's key (+c)")(source):0xffffffffU;
}
}

void ignite_actor(ActorIgnitionState& s,std::uint64_t on,std::uint64_t source,std::uint64_t a3,std::uint64_t t0,
    float f12,const ActorIgnitionHost& h){
    const std::uint8_t was=s.burning_326a;
    s.burning_326a=(s.fire_immune_33b8||s.invulnerable_33a4)?0:static_cast<std::uint8_t>(on);
    if(!was&&s.burning_326a){                                            // 001863cc
        const auto& effect=need(h.effect,"00185d40");
        effect(0,1,actor_ignite_effect_f12);
        effect(1,1,actor_ignite_effect_f12);
        s.f31f4=f12;
        s.source_3480=source;
        s.source_key_3484=key(source,h);
        s.byte_3488=static_cast<std::uint8_t>(a3);
        need(h.start_sound,"the fire sound (001ca210 / 0020f990)")(actor_fire_sound);
    }else if(!s.burning_326a){                                           // 001864dc
        const auto& effect=need(h.effect,"00185d40");
        effect(0,0,actor_extinguish_effect_f12);
        effect(1,0,actor_extinguish_effect_f12);
        need(h.stop_sound,"the fire sound stop (001ca210 / 0020eba0)")(actor_fire_sound);
    }
    if(need(h.networked,"*005721c8")()&&t0==0)                           // 001865a0
        need(h.replicate,"the replication 004e0180 (003c9420)")(key(source,h),s.burning_326a);
}

}  // namespace omega
