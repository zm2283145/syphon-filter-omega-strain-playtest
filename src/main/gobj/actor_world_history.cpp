#include "float_bits.hpp"
#include "actor_world_history.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float number(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Actor world history requires finite normal-or-zero arithmetic");
    return v;
}
float mul(float a,float b){return number(a*b);}
float add(float a,float b){return number(a+b);}
ActorWorldMatrix rigid_inverse(ActorWorldMatrix value){
    std::swap(value[1],value[4]);std::swap(value[2],value[8]);std::swap(value[6],value[9]);
    if(value[12]!=0||value[13]!=0||value[14]!=0){
        const RootVector negative{number(0.0F-value[12]),number(0.0F-value[13]),number(0.0F-value[14]),value[15]};
        for(unsigned row=0;row<3;++row){
            auto result=mul(value[row],negative[0]);
            result=add(result,mul(value[4+row],negative[1]));
            value[12+row]=add(result,mul(value[8+row],negative[2]));
        }
    }
    return value;
}
}
void update_actor_world_history(ActorWorldHistory& state,const ActorWorldMatrix& current,float dt){
    number(dt);if(dt<=0)throw std::runtime_error("Actor world history requires positive elapsed time");
    for(const auto v:current)number(v);
    for(const auto v:state.previous)number(v);
    auto next=state;next.changed=current!=state.previous;
    const auto inverse=rigid_inverse(state.previous);
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<4;++row){
        auto result=mul(current[row],inverse[column*4]);
        for(unsigned lane=1;lane<4;++lane)
            result=add(result,mul(current[lane*4+row],inverse[column*4+lane]));
        next.relative[column*4+row]=result;
    }
    for(unsigned lane=0;lane<4;++lane)next.displacement[lane]=number(current[12+lane]-state.previous[12+lane]);
    next.displacement[3]=0;
    const auto reciprocal=number(1.0F/dt);
    for(unsigned lane=0;lane<4;++lane)next.velocity[lane]=mul(next.displacement[lane],reciprocal);
    next.velocity[3]=0;next.previous=current;state=next;
}
void update_actor_world_history(const ActorWorldHistoryHost& host,float dt){
    if(!host.history)throw std::runtime_error("Actor world history host has no history lookup");
    if(!host.history())return;
    if(!host.node)throw std::runtime_error("Actor world history host has no transform lookup");
    auto* const node=host.node();if(!node)return;
    if(node->dirty){
        if(!host.refresh)throw std::runtime_error("Actor world history dirty node has no refresh");
        host.refresh(*node);
    }
    auto* const history=host.history();
    if(!history)throw std::runtime_error("Actor world history disappeared during refresh");
    update_actor_world_history(*history,node->matrix,dt);
}
}
