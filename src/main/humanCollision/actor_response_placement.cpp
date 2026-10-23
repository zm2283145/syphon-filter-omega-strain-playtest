#include "float_bits.hpp"
#include "actor_response_placement.hpp"
#include "vu_float.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float checked(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Actor response placement requires finite normal-or-zero arithmetic");
    return v;
}
}
ActorResponsePlacementResult apply_actor_response_placement(ActorResponsePlacementState& state,
    RootAttachments& roots,const ActorResponsePlacementInput& input){
    auto next=roots;auto retained=state;
    ActorResponsePlacementResult result{input.resolved_position,ActorRootPublication::none};
    const auto& primary=next.node(input.primary);
    if(input.floor_hit&&!input.flag_dc){
        retained.support_height=checked(input.floor_height);
        retained.child_offset=primary.parent?checked(-checked(primary.local.position[2])):
            checked(retained.support_height-checked(result.resolved_position[2]));
        if(retained.child_offset<0)retained.child_offset=0;
        else if(!primary.parent)
            result.resolved_position[2]=checked(result.resolved_position[2]+retained.child_offset);
    }else retained.child_offset=0;
    if(!primary.parent){
        if((input.flags34&8)==0){
            next.set_world_position(input.primary,result.resolved_position);
            result.publication=ActorRootPublication::position;
        }else{
            if(!input.secondary)throw std::runtime_error("Actor response placement needs its secondary root");
            const auto secondary=next.world_placement(*input.secondary);
            next.set_world_placement(input.primary,secondary,true);
            result.publication=ActorRootPublication::secondary_placement;
        }
    }
    next.set_children_offset(input.primary,{{0,0,0,1},{0,0,checked(-retained.child_offset),1}});
    roots=std::move(next);state=retained;return result;
}
bool apply_actor_response_orientation(RootAttachments& roots,const ActorResponseOrientationInput& input){
    if((input.flags24&2)==0||!input.flag_7a2)return false;
    const auto elapsed=checked(input.elapsed);
    if(elapsed<0)throw std::runtime_error("Actor response orientation requires nonnegative elapsed time");
    const auto factor=checked(3.0F*elapsed);
    auto next=roots;const auto current=next.world_placement(input.secondary).orientation;
    auto target=input.target;
    float alignment=0;
    for(unsigned i=0;i<4;++i){
        const auto product=checked(checked(current[i])*checked(target[i]));
        alignment=i==0?product:checked(alignment+product);
    }
    if(alignment<0)for(auto& v:target)v=checked(0.0F-v);
    RootVector interpolated{};
    float squared=0;
    for(unsigned i=0;i<4;++i){
        // Original accumulates current + target*t, then subtracts current*t.
        interpolated[i]=checked(checked(current[i]+checked(target[i]*factor))-checked(current[i]*factor));
        const auto product=checked(interpolated[i]*interpolated[i]);
        squared=i==0?product:checked(squared+product);
    }
    if(squared<=0)throw std::runtime_error("Actor response orientation interpolation has zero norm");
    // 00190750: VU RSQRT Q = vf0.w/sqrt(|q|^2) over XYZW, then vmulq.xyzw.
    const auto reciprocal=checked(vu::rsqrt(squared));
    for(auto& v:interpolated)v=checked(v*reciprocal);
    next.set_world_orientation(input.secondary,interpolated,true);
    roots=std::move(next);return true;
}
}
