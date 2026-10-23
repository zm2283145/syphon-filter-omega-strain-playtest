#include "float_bits.hpp"
#include "actor_physical_state.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Physical state requires finite normal-or-zero arithmetic");
    return value;
}
void interval(float elapsed){
    if(checked(elapsed)<=0)throw std::runtime_error("Physical state requires positive elapsed time");
}
constexpr float missing=std::bit_cast<float>(0xff7fffffU);
ActorPhysicalMode copy_mode(ActorPhysicalMode destination,ActorPhysicalMode source){
    destination.value=source.value;
    destination.bits=(destination.bits&~0x1fffU)|(source.bits&0x1fffU);
    return destination;
}
}
ActorPhysicalHistory advance_actor_physical_history(const ActorPhysicalHistory& state,float elapsed){
    interval(elapsed);
    const auto delta=checked(checked(state.target_radius)-checked(state.radius));
    auto next=state;
    next.previous=copy_mode(state.previous,state.current);
    next.current=copy_mode(state.current,state.pending);
    next.previous_flags=state.current_flags;
    next.current_flags=state.pending_flags;
    next.radius=checked(state.radius+std::min(elapsed,delta));
    return next;
}
bool actor_physical_mode_runs_response(std::int32_t mode){
    return mode==0||mode==5||mode==9||mode==10||mode==11||mode==12;
}
ActorPhysicalPredictionState reset_actor_physical_integrator(
    const ActorPhysicalPredictionState& state,const RootVector& target){
    auto next=state;
    for(unsigned i=0;i<4;++i){
        const auto delta=checked(checked(target[i])-checked(state.position[i]));
        next.position[i]=checked(state.position[i]+delta);
        // 1e9630 updates the old history before 1e9600 replaces it. Keep the
        // executed arithmetic within our admitted domain even when overwritten.
        checked(checked(state.previous_position[i])+delta);
    }
    next.previous_position=next.position;
    next.force={};next.acceleration={};next.velocity={};
    return next;
}
void clear_actor_physical_override(ActorPhysicalPredictionState& state){state.override_motion={};}
void finish_actor_physical_step(ActorPhysicalPredictionState& state,ActorPhysicalTail& tail,float elapsed){
    interval(elapsed);
    tail.contact_motion={};state.previous_dt=elapsed;
    if(tail.initialized==0)tail.initialized=1;
    tail.reset_requested=0;
}
float capture_actor_support_height(float height,float unavailable_height,float normal_z){
    checked(height);checked(unavailable_height);
    if(height==unavailable_height)return missing;
    checked(normal_z);
    return normal_z>std::bit_cast<float>(0x3f795810U)?height:missing;
}
ActorSupportHeightChange classify_actor_support_height(
    float previous_capture,float height,float unavailable_height,float normal_z){
    checked(previous_capture);
    const auto captured=capture_actor_support_height(height,unavailable_height,normal_z);
    const bool supported=captured!=missing;
    const bool was_supported=previous_capture!=missing;
    return {captured,supported,was_supported&&supported&&previous_capture!=captured,
        was_supported&&!supported};
}
}
