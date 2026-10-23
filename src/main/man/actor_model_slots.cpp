#include "actor_model_slots.hpp"
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void extent(const ActorModelSlots& state){check(state.slots.size()>=2&&state.slots.size()<=256,"Actor model slot extent outside bounded domain");}
}
MorphEquipment model_slot_equipment(const ActorModelSlots& state){
    extent(state);MorphEquipment result;
    for(unsigned i=0;i<2;++i)if(state.slots[i])result[i]=state.slots[i]->grip;
    return result;
}
bool set_actor_model_slot(ActorModelSlots& state,MorphHostState& morph,const MorphGripMap& map,
    std::size_t slot,std::optional<ActorModelPart> part){
    extent(state);check(slot<state.slots.size(),"Actor model slot index");
    if(part&&part->type>=8)return false;
    if(part)check(part->identity!=0,"Actor model part identity must be nonzero");
    auto next=state;auto next_morph=morph;next.slots[slot]=part;
    initialize_equipment_grips(next_morph,map,model_slot_equipment(next));
    state=std::move(next);morph=std::move(next_morph);return true;
}
void swap_actor_model_slots(ActorModelSlots& state,MorphHostState& morph,const MorphGripMap& map,
    std::size_t first,std::size_t second){
    extent(state);check(first<state.slots.size()&&second<state.slots.size(),"Actor model swap index");
    auto next=state;auto next_morph=morph;std::swap(next.slots[first],next.slots[second]);
    initialize_equipment_grips(next_morph,map,model_slot_equipment(next));
    state=std::move(next);morph=std::move(next_morph);
}
namespace {
void clear_hand_weights(const ActorModelSlots& next,MorphHostState& morph){
    const auto equipment=model_slot_equipment(next);
    check(!equipment[0]&&!equipment[1],"Actor model hand part: 0038c5e0 needs the grip tables 00571510 / 00571528");
    for(auto& grip:morph.grips)grip.primary_weight=grip.secondary_weight=0;
}
}
bool set_actor_model_slot(ActorModelSlots& state,MorphHostState& morph,std::size_t slot,std::optional<ActorModelPart> part){
    extent(state);check(slot<state.slots.size(),"Actor model slot index");
    if(part&&part->type>=8)return false;
    if(part)check(part->identity!=0,"Actor model part identity must be nonzero");
    auto next=state;auto next_morph=morph;next.slots[slot]=part;
    clear_hand_weights(next,next_morph);
    state=std::move(next);morph=std::move(next_morph);return true;
}
void swap_actor_model_slots(ActorModelSlots& state,MorphHostState& morph,std::size_t first,std::size_t second){
    extent(state);check(first<state.slots.size()&&second<state.slots.size(),"Actor model swap index");
    auto next=state;auto next_morph=morph;std::swap(next.slots[first],next.slots[second]);
    clear_hand_weights(next,next_morph);
    state=std::move(next);morph=std::move(next_morph);
}
ActorModelSlots constructed_actor_model_slots(){
    ActorModelSlots result;result.slots.assign(actor_model_slot_count,std::nullopt);return result;
}
}
