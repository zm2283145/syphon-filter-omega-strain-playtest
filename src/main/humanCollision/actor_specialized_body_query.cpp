#include "float_bits.hpp"
#include "actor_specialized_body_query.hpp"
#include "morph_resources.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float value(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Specialized body query requires finite normal-or-zero arithmetic");
    return v;
}
float add(float a,float b){return value(value(a)+value(b));}
float sub(float a,float b){return value(value(a)-value(b));}
float mul(float a,float b){return value(value(a)*value(b));}
float div(float a,float b){if(value(b)==0)throw std::runtime_error("Undefined specialized body division");return value(value(a)/b);}
void valid(const RootVector& v){for(float x:v)value(x);}
RootVector rotate(const RootVector& q,const RootVector& v){
    RootVector first,result,conjugate{sub(0,q[0]),sub(0,q[1]),sub(0,q[2]),q[3]};
    for(unsigned i=0;i<3;++i){const auto j=(i+1)%3,k=(i+2)%3;
        first[i]=sub(add(mul(q[j],v[k]),mul(v[i],q[3])),mul(v[j],q[k]));}
    first[3]=sub(sub(sub(mul(q[3],0),mul(q[2],v[2])),mul(q[1],v[1])),mul(q[0],v[0]));
    for(unsigned i=0;i<3;++i){const auto j=(i+1)%3,k=(i+2)%3;
        result[i]=sub(add(add(mul(first[j],conjugate[k]),mul(first[i],conjugate[3])),
            mul(conjugate[i],first[3])),mul(conjugate[j],first[k]));}
    result[3]=v[3];return result;
}
RootVector transform(const RootParentTransform& frame,const RootVector& point){
    const auto matrix=root_placement_matrix(frame);RootVector result{};
    for(unsigned row=0;row<4;++row){result[row]=mul(matrix[row],point[0]);
        for(unsigned column=1;column<4;++column)result[row]=add(result[row],mul(matrix[column*4+row],point[column]));}
    return result;
}
}
RootParentTransform invert_specialized_body_placement(const RootParentTransform& frame){
    valid(frame.orientation);valid(frame.position);RootParentTransform result=frame;
    const auto& q=frame.orientation;
    const float norm=add(add(add(mul(q[0],q[0]),mul(q[1],q[1])),mul(q[2],q[2])),mul(q[3],q[3]));
    if(norm!=0){const float reciprocal=div(1,norm);
        for(unsigned i=0;i<4;++i)result.orientation[i]=mul(i<3?sub(0,q[i]):q[i],reciprocal);}
    RootVector negated=frame.position;for(unsigned i=0;i<3;++i)negated[i]=sub(0,negated[i]);
    result.position=rotate(result.orientation,negated);return result;
}
RootVector resolve_specialized_actor_body_center(const RootVector& origin,const RootVector& displacement,
    float radius,const ActorSpecializedRootHost& host){
    if(!host.weight)throw std::runtime_error("Specialized body weight getter required");
    valid(origin);valid(displacement);value(radius);
    if(radius<=0)throw std::runtime_error("Specialized body radius must be positive");
    RootVector center=origin;
    for(auto which:{ActorSpecializedRoot::first_4a03d8,ActorSpecializedRoot::second_4a03d0}){
        if(value(host.weight(which))<=0)continue;
        if(!host.placement)throw std::runtime_error("Active specialized body placement getter required");
        const auto inverse=invert_specialized_body_placement(host.placement(which));
        RootVector endpoint{};for(unsigned i=0;i<4;++i)endpoint[i]=add(origin[i],displacement[i]);
        auto local=transform(inverse,endpoint);
        if(which==ActorSpecializedRoot::first_4a03d8){
            if(-local[1]>local[2])local[1]=std::min(-radius,local[1]);
        }else if(local[1]<=0&&local[2]>=0){
            const float length=value(std::sqrt(add(mul(local[1],local[1]),mul(local[2],local[2]))));
            if(length<radius){const float scale=div(radius,length);local[1]=mul(local[1],scale);local[2]=mul(local[2],scale);}
        }else if(-local[1]>local[2])local[1]=std::min(-radius,local[1]);
        else local[2]=std::max(radius,local[2]);
        const auto world=transform(host.placement(which),local);
        RootVector candidate{};for(unsigned i=0;i<4;++i)candidate[i]=sub(world[i],displacement[i]);
        const float weight=value(host.weight(which));
        for(unsigned i=0;i<4;++i)center[i]=sub(add(center[i],mul(candidate[i],weight)),mul(center[i],weight));
    }
    return center;
}
ActorBodyQueryPlan make_specialized_actor_body_query(const ActorBodyQueryInput& input,const ActorSpecializedRootHost& host){
    const auto center=resolve_specialized_actor_body_center(
        {input.resolved_origin.x,input.resolved_origin.y,input.resolved_origin.z,1},
        {input.resolved_displacement.x,input.resolved_displacement.y,input.resolved_displacement.z,0},input.radius_370,host);
    // The blend can leave W an ulp or more from 1 (fractional weights). The
    // transformed constructor 003c66b0 overwrites W with 1 before building the
    // query placement (3c67f4) and stores the raw center at query +f0
    // (3c6f5c); see make_transformed_actor_body_query.
    return make_transformed_actor_body_query(input,{center[0],center[1],center[2]},{1,1,1},center[3]);
}
}
