#pragma once
// Shared vector arithmetic of the upper-body pose pass and its IK chain
// (001cb160 .. 001d7de0), as the EE interpreter models it. Internal to
// upper_body_pose.cpp and upper_body_ik.cpp.
#include "animation_root.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace omega::upper_body_math {
using V=RootVector;

[[noreturn]] inline void fail(const std::string& what){throw std::runtime_error("Upper-body pose: "+what);}
inline float fb(std::uint32_t b){return std::bit_cast<float>(b);}
// The interpreter's per-result denormal flush; a nonfinite result throws.
// Checkpoint 132win9 (port-side speed only): the finiteness / denormal tests on the bits (MSVC's
// std::isfinite is an out-of-line _fdclass call); the results are unchanged.
inline float fin(float x){
    const std::uint32_t b=std::bit_cast<std::uint32_t>(x);
    if((b&0x7f800000U)==0x7f800000U)fail("nonfinite arithmetic");
    if((b&0x7f800000U)==0U)return std::bit_cast<float>(b&0x80000000U);
    return x;
}
// The MAC sign flag of a pre-flush result: negative, or a negative zero.
inline bool sign_flag(float x){
    const std::uint32_t b=std::bit_cast<std::uint32_t>(x);
    if((b&0x7f800000U)==0x7f800000U)fail("nonfinite arithmetic");
    return (b&0x80000000U)!=0U;
}
// The accumulating VU / FPU forms: the binary32 product is not flushed, the
// sum is.
inline float madd(float acc,float a,float b){return fin(acc+a*b);}
inline float msub(float acc,float a,float b){return fin(acc-a*b);}
// max.s $fd, zero, x as the interpreter models it (Python max): x if x > 0,
// else +0.
inline float max_zero(float x){return x>0.0F?x:0.0F;}

// vsub.xyz vf1,vf0,vf1: 0 - xyz, w kept.
inline V conjugate(const V& q){return {fin(0.0F-q[0]),fin(0.0F-q[1]),fin(0.0F-q[2]),q[3]};}
// vmulax.xyzw ACC,vf0,vf0 / vmsubw.xyzw vd,v,vf0: 0 - v * 1 in every lane.
inline V negate(const V& v){return {msub(0.0F,v[0],1.0F),msub(0.0F,v[1],1.0F),msub(0.0F,v[2],1.0F),msub(0.0F,v[3],1.0F)};}

// The inline product of 001a7df0 and 001d5730 with vf1 = a, vf2 = b: a (x) b.
inline V multiply(const V& a,const V& b){
    const V p{fin(a[0]*b[0]),fin(a[1]*b[1]),fin(a[2]*b[2]),fin(a[3]*b[3])};
    float x=fin(a[1]*b[2]),y=fin(a[2]*b[0]),z=fin(a[0]*b[1]); // vopmula
    x=madd(x,a[0],b[3]);y=madd(y,a[1],b[3]);z=madd(z,a[2],b[3]);  // vmaddaw a, b.w
    x=madd(x,b[0],a[3]);y=madd(y,b[1],a[3]);z=madd(z,b[2],a[3]);  // vmaddaw b, a.w
    x=msub(x,b[1],a[2]);y=msub(y,b[2],a[0]);z=msub(z,b[0],a[1]);  // vopmsub
    float w=fin(p[3]-p[2]);                                       // vsubaz.w
    w=msub(w,1.0F,p[1]);                                          // vmsubay.w
    w=msub(w,1.0F,p[0]);                                          // vmsubx.w
    return {x,y,z,w};
}

// 001d2ba0 and the inline copies in 001d5730 / 001d02b0: x is negated when
// the sign flag of x . reference (x + y + z + w through the accumulator) is
// set.
inline void hemisphere(V& x,const V& reference){
    const V p{fin(x[0]*reference[0]),fin(x[1]*reference[1]),fin(x[2]*reference[2]),fin(x[3]*reference[3])};
    const V negated=negate(x);
    float acc=fin(1.0F*p[0]);
    acc=madd(acc,1.0F,p[1]);
    acc=madd(acc,1.0F,p[2]);
    if(sign_flag(acc+1.0F*p[3]))x=negated;
}

// 001d5d20..001d5de4: +a0 = offset rotated by the parent rotation, plus the
// parent position (scaled by offset.w unless it is 1).
// The rotation sandwich of the offset's xyz (w 0); the result's w is offset.w
// (vmove.w vf6, vf1).
inline V rotate_offset(const V& rotation,const V& offset){
    const V& q=rotation;
    const V v{offset[0],offset[1],offset[2],0.0F};
    const V c{fin(0.0F-q[0]),fin(0.0F-q[1]),fin(0.0F-q[2]),q[3]};
    const V p{fin(q[0]*v[0]),fin(q[1]*v[1]),fin(q[2]*v[2]),fin(q[3]*v[3])};
    float x=fin(q[1]*v[2]),y=fin(q[2]*v[0]),z=fin(q[0]*v[1]);
    x=madd(x,v[0],q[3]);y=madd(y,v[1],q[3]);z=madd(z,v[2],q[3]);
    const float fx=msub(x,v[1],q[2]),fy=msub(y,v[2],q[0]),fz=msub(z,v[0],q[1]);
    float fw=fin(p[3]-p[2]);
    fw=msub(fw,1.0F,p[1]);
    fw=msub(fw,1.0F,p[0]);
    x=fin(fy*c[2]);y=fin(fz*c[0]);z=fin(fx*c[1]);
    x=madd(x,fx,c[3]);y=madd(y,fy,c[3]);z=madd(z,fz,c[3]);
    x=madd(x,c[0],fw);y=madd(y,c[1],fw);z=madd(z,c[2],fw);
    x=msub(x,c[1],fz);y=msub(y,c[2],fx);z=msub(z,c[0],fy);
    return {x,y,z,offset[3]};
}
// The tail after the sandwich was stored: +w == 1.0 adds the translation's
// xyz, else xyz += translation * w and w = (translation * w).w.
inline V add_translation(const V& r,const V& translation){
    const float w=r[3];
    if(w==1.0F)return {fin(r[0]+translation[0]),fin(r[1]+translation[1]),fin(r[2]+translation[2]),w};
    const V t{fin(translation[0]*w),fin(translation[1]*w),fin(translation[2]*w),fin(translation[3]*w)};
    return {fin(r[0]+t[0]),fin(r[1]+t[1]),fin(r[2]+t[2]),t[3]};
}
inline V place(const V& rotation,const V& translation,const V& offset){
    return add_translation(rotate_offset(rotation,offset),translation);
}

// 00190950(out, a, b, t): vaddax / vmaddax / vmsubx on xyz; out.w = a.w.
inline V lerp3(const V& a,const V& b,float t){
    V r=a;
    for(std::size_t k=0;k<3;++k)r[k]=msub(madd(fin(a[k]+0.0F),b[k],t),a[k],t);
    return r;
}

}
