#include "actor_running_jump.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor running jump: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float fmax_s(float s,float t){return t>s?t:s;}                       // max.s fd, fs, ft (fs kept on ties)
RootVector sub4(const RootVector& a,const RootVector& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
// 00192fb0 / inline: vmul.xyzw, then 1*x + 1*y + 1*z in the W lane.
float dot3(const RootVector& a,const RootVector& b){
    return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));
}
// 00171b30-style row-vector product over xyzw (vmulax/vmadday/vmaddaz/vmaddw).
RootVector transform(const std::array<float,16>& m,const RootVector& p){
    RootVector q{};
    for(std::size_t c=0;c<4;++c)q[c]=add(add(add(mul(m[c],p[0]),mul(m[4+c],p[1])),mul(m[8+c],p[2])),mul(m[12+c],p[3]));
    return q;
}
}

RootVector rotate_actor_local_point(const RootVector& t,const RootVector& q){
    for(float v:t)check(v);
    for(float v:q)check(v);
    const float rx=sub(add(mul(q[1],t[2]),mul(t[0],q[3])),mul(t[1],q[2]));
    const float ry=sub(add(mul(q[2],t[0]),mul(t[1],q[3])),mul(t[2],q[0]));
    const float rz=sub(add(mul(q[0],t[1]),mul(t[2],q[3])),mul(t[0],q[1]));
    const float rw=sub(sub(sub(mul(q[3],0.0F),mul(q[2],t[2])),mul(q[1],t[1])),mul(q[0],t[0]));
    const float cx=sub(0.0F,q[0]),cy=sub(0.0F,q[1]),cz=sub(0.0F,q[2]),cw=q[3];
    return {sub(add(add(mul(ry,cz),mul(rx,cw)),mul(cx,rw)),mul(cy,rz)),
            sub(add(add(mul(rz,cx),mul(ry,cw)),mul(cy,rw)),mul(cz,rx)),
            sub(add(add(mul(rx,cy),mul(rz,cw)),mul(cz,rw)),mul(cx,ry)),t[3]};
}

std::optional<ActorRunningJumpTakeoff> request_actor_running_jump(const ActorJumpZoneMessage& m,
    const ActorJumpZoneActor& a,const std::optional<ActorJumpZoneTarget>& target){
    if(!a.byte_14&&a.component_is_player)return std::nullopt;            // 00192824
    if(!((a.flags_3204>>1)&1U))return std::nullopt;                       // 00192860
    if(a.mode_320c==2||a.mode_320c==4||a.mode_320c==5)return std::nullopt;
    if(m.kind!=6)return std::nullopt;                                     // 00192898
    if(!target)return std::nullopt;                                       // 0040c540, class word 004ee6b8
    const auto& lo=target->minimum;const auto& hi=target->maximum;
    // 00192910: c = (min + max) * 0.5 over xyzw (w lanes 1).
    RootVector c{mul(add(lo[0],hi[0]),0.5F),mul(add(lo[1],hi[1]),0.5F),mul(add(lo[2],hi[2]),0.5F),mul(add(1.0F,1.0F),0.5F)};
    RootVector d{};                                                       // 00192974: min.x <= max.x
    if(lo[0]<=hi[0])d={sub(hi[0],lo[0]),sub(hi[1],lo[1]),sub(hi[2],lo[2]),sub(1.0F,1.0F)};
    const float longest=fmax_s(d[0],fmax_s(d[1],d[2]));                 // 001929fc/00192a08
    RootVector p0=c,p1=c;
    for(std::size_t axis=0;axis<3;++axis)
        if(longest==d[axis]){p0[axis]=lo[axis];p1[axis]=hi[axis];break;}  // 00192a2c..00192a94
    if(target->world){p0=transform(*target->world,p0);p1=transform(*target->world,p1);}
    // 00192b38: the heading.
    RootVector v=rotate_actor_local_point(a.desired_31c0,a.flor.orientation);
    v[2]=0.0F;                                                            // 00192bc4
    const float squared=add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));
    // vrsqrt with a zero radicand gives +FLT_MAX in the interpreter model.
    const float scale=squared==0.0F?std::numeric_limits<float>::max():check(vu::rsqrt(squared));
    for(std::size_t i=0;i<3;++i)v[i]=mul(v[i],scale);
    const RootVector n{-v[1],v[0],0.0F,0.0F};                             // 00192c58..00192c6c (neg.s)
    const RootVector& floor=a.flor.position;
    const auto segment=sub4(p1,p0);                                       // 00183380
    float along=dot3(segment,n),across=dot3(sub4(floor,p0),n);            // 00192fb0
    if(along<0.0F){along=-along;across=-across;}                          // 00192ca4
    if(across<0.0F||!(across<=along)||along==0.0F)return std::nullopt;
    const float t=check(vu::div(across,along));                           // 00192cec
    RootVector crossing{};                                                // 0017f670, 00134080
    for(std::size_t i=0;i<4;++i)crossing[i]=add(p0[i],mul(segment[i],t));
    if(dot3(sub4(crossing,floor),v)<=0.0F)return std::nullopt;            // 00192d4c..00192db4
    ActorRunningJumpTakeoff out;out.payload=crossing;
    if(m.side_30==2){                                                     // 00192dd0
        RootVector e{-segment[1],segment[0],0.0F,0.0F};
        if(dot3(e,v)<0.0F)for(auto& x:e)x=sub(0.0F,x);                   // vmulax/vmsubw: 0 - e
        out.payload[3]=check(game_atan2f(-e[0],e[1]));
        out.action=0x16;
    }else if(m.side_30==1){
        out.payload[3]=1.0F;out.action=0x15;
    }else throw std::runtime_error("Actor action 0x17: jump-zone side other than 1/2 (the original sends an uninitialized action)");
    return out;
}
}
