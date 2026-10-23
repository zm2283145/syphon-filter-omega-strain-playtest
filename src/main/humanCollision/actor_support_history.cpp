#include "float_bits.hpp"
#include "actor_support_history.hpp"
#include "actor_support_motion.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float number(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Support history requires finite normal-or-zero arithmetic");
    return value;
}
std::array<float,16> inverse(std::array<float,16> value){
    for(const auto lane:value)number(lane);
    if(value[3]!=0||value[7]!=0||value[11]!=0||value[15]!=1)
        throw std::runtime_error("Support history requires an affine owner matrix");
    std::swap(value[1],value[4]);std::swap(value[2],value[8]);std::swap(value[6],value[9]);
    if(value[12]!=0||value[13]!=0||value[14]!=0){
        const RootVector negative{number(0.0F-value[12]),number(0.0F-value[13]),number(0.0F-value[14]),1};
        for(unsigned row=0;row<3;++row){
            auto sum=number(value[row]*negative[0]);
            sum=number(sum+number(value[4+row]*negative[1]));
            value[12+row]=number(sum+number(value[8+row]*negative[2]));
        }
    }
    return value;
}
}
void capture_actor_support_history(ActorSupportHistory& state,const ActorSupportHistoryHost& host){
    if(!host.primary_enabled)throw std::runtime_error("Support history has no primary gate");
    auto next=state;
    const auto component=[&](ActorSupportIdentity owner){
        if(!host.component)throw std::runtime_error("Support history has no component lookup");
        return host.component(owner);
    };
    const auto root=[&](std::size_t slot){
        if(!host.root)throw std::runtime_error("Support history has no root lookup");
        auto* result=host.root(slot);
        if(!result)throw std::runtime_error("Support history registry root is missing");
        return result;
    };
    const auto refresh=[&](ActorSupportHistoryComponent& captured){
        if(captured.dirty){
            if(!host.refresh)throw std::runtime_error("Support history dirty component has no refresh");
            host.refresh(captured,next);
        }
    };
    const auto capture=[&](std::size_t slot,ActorSupportHistoryRoot& captured_root,ActorSupportHistoryComponent& captured){
        refresh(captured);
        const auto inverse_placement=actor_support_matrix_placement(inverse(captured.matrix));
        if(!host.world_placement)throw std::runtime_error("Support history has no world placement getter");
        const auto placement=host.world_placement(captured_root,next);
        next.local[slot]=compose_actor_support_placement(placement,inverse_placement);
        refresh(captured);
        next.previous_owner[slot]=actor_support_matrix_placement(captured.matrix);
    };
    if(host.primary_enabled()){
        if(!host.contact_owner)throw std::runtime_error("Support history has no contact owner lookup");
        const auto owner=host.contact_owner();
        next.owners[0]=0;
        if(owner){
            if(auto* const captured=component(owner)){
                auto* const captured_root=root(0);
                next.owners[0]=owner;
                capture(0,*captured_root,*captured);
            }
        }
    }
    next.retain[0]=false;
    for(std::size_t slot=1;slot<3;++slot){
        const auto owner=next.owners[slot];
        if(!owner)continue;
        auto* const captured_root=root(slot);
        if(number(captured_root->weight)>0||next.retain[slot]){
            if(auto* const captured=component(owner))capture(slot,*captured_root,*captured);
        }else next.owners[slot]=0;
        if(number(captured_root->weight)>0)next.retain[slot]=false;
    }
    state=next;
}
}
