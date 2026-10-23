#include "actor_water_support.hpp"
#include <stdexcept>

namespace omega {
ActorWaterSupportResult notify_actor_water_support(float previous_capture,float height,float unavailable_height,
    float normal_z,std::uint32_t actor,const ActorWaterSupportHost& host,WaterEffectManager& manager,
    WaterEffectActorHost& actors,WaterEffectSink& sink){
    if(!host.class_key_matches||!host.controller_58||!host.controller_54||!host.set_in_water_346d)
        throw std::runtime_error("Water support: complete actor host required");
    ActorWaterSupportResult out;
    out.change=classify_actor_support_height(previous_capture,height,unavailable_height,normal_z);
    // 1e5e1c..5ea0: only a class-key actor whose controller answers +58 or +54
    // receives the in-water byte.
    bool publish=false;
    if(host.class_key_matches()){
        const auto first=host.controller_58();
        publish=first.value_or(false);
        if(!publish)publish=host.controller_54().value_or(false);
    }
    if(publish){host.set_in_water_346d(out.change.supported);out.published=true;}
    if(out.change.supported){
        manager.ensure(out.change.height,actor,actors,sink);
        out.effect=ActorWaterSupportResult::Effect::ensure;
        if(out.change.changed){
            manager.update_height(out.change.height,actor,actors);
            out.effect=ActorWaterSupportResult::Effect::ensure_and_update;
        }
    }else if(out.change.lost){
        manager.remove(actor,actors,sink);
        out.effect=ActorWaterSupportResult::Effect::remove;
    }
    return out;
}
}
