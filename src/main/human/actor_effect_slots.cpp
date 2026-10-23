#include "actor_effect_slots.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F>
const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Actor effect slots (00185d40): ")+what+" is not hosted");
    return f;
}
}

bool actor_effect_slot_valid(const ActorEffectSlot& slot,const ActorEffectSlotHost& host){
    return slot.object_354c!=0&&need(host.key_of,"the object key (+c)")(slot.object_354c)==slot.key_3550;
}

void set_actor_effect(ActorEffectSlotState& state,std::uint64_t kind,std::uint64_t on,float f12,const ActorEffectSlotHost& host){
    const auto k=static_cast<std::uint8_t>(kind);
    if(k>3)throw std::runtime_error("Actor effect slots (00185d40): kind "+std::to_string(k)+" is outside the tables 0048a2d8 / 0048a2e8");
    auto& slot=state.slots[k];
    if(on){
        if(actor_effect_slot_valid(slot,host))return;
        const auto object=need(host.create_effect,"the effect object (0036b180 + 00236810)")(
            actor_effect_descriptors[k],actor_effect_counts[k],f12);
        if(!object)throw std::runtime_error("Actor effect slots (00185d40): a null effect object (the original then calls through null)");
        slot.object_354c=object;
        slot.key_3550=need(host.key_of,"the object key (+c)")(object);
        if(k==0)state.light_34e0=need(host.create_light,"the fire light (0036b180 + 002624a0)")(actor_effect_light_f12,actor_effect_light_f13);
        const auto valid=actor_effect_slot_valid(slot,host)?slot.object_354c:0U;
        need(host.register_object,"0040c9d0")(valid);
        if(!valid)throw std::runtime_error("Actor effect slots (00185d40): virtual +5c through a stale handle");
        need(host.activate,"the effect's virtual +5c")(valid);
        return;
    }
    if(actor_effect_slot_valid(slot,host)){
        const auto object=slot.object_354c;
        need(host.deactivate,"the effect's virtual +60")(object);
        need(host.unregister_object,"0040c9a0")(object);
        need(host.release,"003cb230")(object,actor_effect_release_delay);
        slot.object_354c=0;slot.key_3550=0xffffffffU;
    }
    if(k!=0)return;
    state.burning_326a=false;
    need(host.stop_sound,"the sound stop (001ca210 + 0020eba0)")(actor_effect_stop_sound);
    if(state.light_34e0){
        need(host.unregister_object,"0040c9a0")(state.light_34e0);
        need(host.release,"003cb230")(state.light_34e0,actor_effect_release_delay);
        state.light_34e0=0;
    }
}

}  // namespace omega
