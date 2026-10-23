#include "actor_position_curve.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
void retarget_actor_position_curve(AnimationVectorCurveState& state,
    float duration,const RootVector& target) {
    auto next=state;
    retime_animation_vector_curve(next,duration);
    set_animation_vector_target(next,target);
    state=next;
}

std::optional<RootVector> update_actor_position_curve(AnimationVectorCurveState& state,
    bool enabled,float root_weight,const RootVector& root_position,float elapsed) {
    if(!enabled)return std::nullopt;
    if(!std::isfinite(root_weight)||(root_weight!=0&&!std::isnormal(root_weight)))
        throw std::runtime_error("Actor position curve root weight is invalid");
    auto next=state;
    const bool publish=root_weight>=0.98F;
    if(publish)reset_animation_vector_current(next,root_position);
    else {
        retime_animation_vector_curve(next,0.2F);
        rebase_animation_vector_current(next,root_position);
    }
    step_animation_vector_curve(next,elapsed);
    state=next;
    return publish?std::optional<RootVector>(next.current):std::nullopt;
}
}
