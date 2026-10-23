#include "float_bits.hpp"
#include "los_frame.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float value(float x){
    if(::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("LOS corridor arithmetic outside finite normal-or-zero domain");
    return x;
}
float add(float a,float b){return value(value(a)+value(b));}
float sub(float a,float b){return value(value(a)-value(b));}
float mul(float a,float b){return value(value(a)*value(b));}
float squared(const RootVector& v){return add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));}
RootVector cross(const RootVector& a,const RootVector& b){
    return {sub(mul(a[1],b[2]),mul(a[2],b[1])),sub(mul(a[2],b[0]),mul(a[0],b[2])),
        sub(mul(a[0],b[1]),mul(a[1],b[0])),0};
}
RootVector normalize(RootVector v){
    const auto length_squared=squared(v);
    if(length_squared<=0)throw std::runtime_error("Degenerate LOS corridor basis");
    // 00132060 / 00132030: VU RSQRT Q = vf0.w/sqrt(|v|^2), then vmulq.xyz.
    const auto reciprocal=value(vu::rsqrt(length_squared));
    for(unsigned i=0;i<3;++i)v[i]=mul(v[i],reciprocal);
    return v;
}
}
LosCorridorFrame prepare_los_corridor(const RootVector& origin,const RootVector& displacement,float radius){
    for(auto x:origin)value(x);
    for(auto x:displacement)value(x);
    value(radius);
    if(origin[3]!=1||displacement[3]!=0||radius<=0)
        throw std::runtime_error("LOS corridor requires an affine point, displacement and positive radius");
    if(displacement[0]==0&&displacement[1]==0&&displacement[2]==0)
        throw std::runtime_error("Zero LOS segment must take the enclosing query early return");
    LosCorridorFrame result{};result.start=origin;
    for(unsigned i=0;i<4;++i)result.end[i]=add(origin[i],displacement[i]);
    const float ax=std::abs(displacement[0]),ay=std::abs(displacement[1]),az=std::abs(displacement[2]);
    const auto least=std::min(std::min(ax,ay),az);
    RootVector axis{};axis[least==az?2:least==ay?1:0]=1;
    const auto first=normalize(cross(displacement,axis));
    const auto second=normalize(displacement);
    const auto third=cross(first,second);
    auto& frame=result.world_to_corridor;
    for(unsigned i=0;i<4;++i){frame[i]=first[i];frame[4+i]=second[i];frame[8+i]=third[i];}
    RootVector half{};
    for(unsigned i=0;i<4;++i)half[i]=mul(displacement[i],.5F);
    for(unsigned i=0;i<3;++i)frame[12+i]=add(origin[i],half[i]);
    frame[15]=1;
    // Same rigid transpose/translation path as 0013ee20/0013ed60, before scaling.
    std::swap(frame[1],frame[4]);std::swap(frame[2],frame[8]);std::swap(frame[6],frame[9]);
    if(frame[12]!=0||frame[13]!=0||frame[14]!=0){
        const RootVector negative{sub(0,frame[12]),sub(0,frame[13]),sub(0,frame[14]),1};
        for(unsigned row=0;row<3;++row){
            auto x=mul(frame[row],negative[0]);x=add(x,mul(frame[4+row],negative[1]));
            frame[12+row]=add(x,mul(frame[8+row],negative[2]));
        }
    }
    const auto half_squared=squared(half);
    if(half_squared<=0)throw std::runtime_error("Degenerate LOS corridor length");
    // 003c2588 VU SQRT of the half length, then FPU div.s 1/radius (003c2570)
    // and 1/half_length (003c25d4).
    const auto half_length=value(vu::sqrt(half_squared));
    const auto lateral=value(vu::div(1.0F,radius)),longitudinal=value(vu::div(1.0F,half_length));
    for(unsigned column=0;column<4;++column){
        frame[column*4]=mul(frame[column*4],lateral);
        frame[column*4+1]=mul(frame[column*4+1],longitudinal);
        frame[column*4+2]=mul(frame[column*4+2],lateral);
    }
    return result;
}
}
