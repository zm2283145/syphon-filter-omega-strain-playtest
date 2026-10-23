#include "float_bits.hpp"
#include "actor_model_context.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float scalar(float v) {
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Actor context requires finite normal-or-zero arithmetic");
    return v;
}
float add(float a,float b){return scalar(a+b);}
float mul(float a,float b){return scalar(a*b);}
std::size_t count(const ActorModelContextHost& host) {
    const auto result=host.count();
    if(result>65536)throw std::runtime_error("Actor context node count exceeds bounded extent");
    return result;
}
ActorModelContextNode node(const ActorModelContextHost& host,std::size_t index,const ActorModelContextState& state) {
    auto result=host.snapshot(index);
    if(result.dirty) {
        if(!host.refresh)throw std::runtime_error("Actor context dirty node has no refresh host");
        host.refresh(index,state);result=host.snapshot(index);
    }
    return result;
}
RootVector center(const ActorModelContextNode& node) {
    RootVector result;
    if(node.bounds_at_translation) {
        for(unsigned i=0;i<4;++i)result[i]=scalar(node.transform[12+i]);
    } else {
        for(unsigned row=0;row<4;++row) {
            float value=mul(scalar(node.transform[row]),scalar(node.bounds_center[0]));
            for(unsigned column=1;column<4;++column)
                value=add(value,mul(scalar(node.transform[column*4+row]),scalar(node.bounds_center[column])));
            result[row]=value;
        }
    }
    return result;
}
void include(ActorModelContextState& state,const RootVector& value) {
    for(unsigned i=0;i<3;++i) {
        state.bounds_min[i]=std::min(state.bounds_min[i],value[i]);
        state.bounds_max[i]=std::max(state.bounds_max[i],value[i]);
    }
}
}
void update_actor_model_context(ActorModelContextState& state,bool enabled,const ActorModelContextHost& host) {
    if(!enabled)return;
    if(!host.count||!host.snapshot)throw std::runtime_error("Actor context host is incomplete");
    auto next=state;
    const float limit=std::numeric_limits<float>::max();
    next.bounds_min={limit,limit,limit,1};next.bounds_max={-limit,-limit,-limit,1};
    const auto bounds_count=count(host);
    for(std::size_t i=0;i<bounds_count;++i) {
        const auto current=node(host,i,next);
        if(!current.bounds_enabled)continue;
        const auto point=center(current);const auto radius=scalar(current.radius);
        if(radius>0) {
            RootVector lower,upper;
            for(unsigned c=0;c<4;++c)lower[c]=add(point[c],-(c<3?radius:0.0F));
            include(next,lower);
            for(unsigned c=0;c<4;++c)upper[c]=add(point[c],c<3?radius:0.0F);
            include(next,upper);
        } else include(next,point);
    }
    next.centroid={};float total=0;
    const auto center_count=count(host);
    for(std::size_t i=0;i<center_count;++i) {
        const auto current=node(host,i,next);const auto weight=scalar(current.center_weight);
        if(weight==0)continue;
        for(unsigned c=0;c<4;++c)
            next.centroid[c]=add(next.centroid[c],mul(scalar(current.transform[12+c]),weight));
        total=add(total,weight);
    }
    if(total!=0) {
        const auto inverse=scalar(1.0F/total);
        for(auto& value:next.centroid)value=mul(value,inverse);
    }
    next.centroid[3]=1;state=next;
}
}
