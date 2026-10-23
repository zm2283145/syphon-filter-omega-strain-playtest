#include "actor_curve_update.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
ActorCurveUpdateState make_actor_curve_update(float position_damping){
    require(std::isfinite(position_damping)&&(position_damping==0||std::isnormal(position_damping)),
        "Actor position damping must be finite normal or zero");
    ActorCurveUpdateState state;state.position.damping=position_damping;return state;
}
void update_actor_curves(AnimationRotationCurveState& heading,ActorCurveUpdateState& state,
    float elapsed,const ActorCurveUpdateHost& host){
    auto next_heading=heading;auto next=state;
    step_animation_rotation_curve(next_heading,elapsed);
    require(static_cast<bool>(host.position_enabled),"Actor position enable binding is absent");
    if(host.position_enabled()){
        require(static_cast<bool>(host.root_weight),"Actor root weight binding is absent");
        const float weight=host.root_weight();
        require(std::isfinite(weight)&&(weight==0||std::isnormal(weight)),"Actor root weight must be finite normal or zero");
        const bool publish=weight>=0.98F;
        if(!publish)retime_animation_vector_curve(next.position,0.2F);
        require(static_cast<bool>(host.root_world_position),"Actor root world-position binding is absent");
        const auto position=host.root_world_position();
        if(publish)reset_animation_vector_current(next.position,position);
        else rebase_animation_vector_current(next.position,position);
        step_animation_vector_curve(next.position,elapsed);
        if(publish){
            require(static_cast<bool>(host.publish_root_world_position),"Actor root publication binding is absent");
            host.publish_root_world_position(next.position.current);
        }
    }
    //001b2680, then001b2670, after any root publication and even when disabled.
    next.control_flags3030[0]=next.control_flags3030[1];
    next.control_flags3030[1]=next.control_flags3030[2];
    next.control_flags3030[2]=0;
    heading=next_heading;state=next;
}
}
