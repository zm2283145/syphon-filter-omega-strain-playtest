#include "camera_curves.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if(!std::isfinite(value)) throw std::runtime_error("Camera curve requires finite arithmetic");
    return value;
}
void check(const RootVector& v) { for(float x:v) checked(x); }
float add(float a,float b) { return checked(a+b); }
float sub(float a,float b) { return checked(a-b); }
float mul(float a,float b) { return checked(a*b); }
float reciprocal(float d) { return d==0 ? 0.0F : checked(1.0F/d); }
RootVector scaled(const RootVector& v,float s) {
    return {mul(v[0],s),mul(v[1],s),mul(v[2],s),mul(v[3],s)};
}
RootVector negated(const RootVector& v) {
    // vmulax ACC = 0; vmsubw vf = ACC - v * 1.
    return {sub(0,v[0]),sub(0,v[1]),sub(0,v[2]),sub(0,v[3])};
}
void check_segment_duration(float duration) {
    if(checked(duration)<0) throw std::runtime_error("Camera curve duration is negative");
}
}

float camera_dot4(const RootVector& a,const RootVector& b) {
    return add(add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2])),mul(a[3],b[3]));
}

RootVector camera_quaternion_multiply(const RootVector& a,const RootVector& b) {
    // vopmula/vmaddaw/vmaddaw/vopmsub for XYZ; vmul then vsubaz/vmsubay/vmsubx for W.
    const float x=sub(add(add(mul(a[1],b[2]),mul(a[0],b[3])),mul(b[0],a[3])),mul(b[1],a[2]));
    const float y=sub(add(add(mul(a[2],b[0]),mul(a[1],b[3])),mul(b[1],a[3])),mul(b[2],a[0]));
    const float z=sub(add(add(mul(a[0],b[1]),mul(a[2],b[3])),mul(b[2],a[3])),mul(b[0],a[1]));
    const float w=sub(sub(sub(mul(a[3],b[3]),mul(a[2],b[2])),mul(a[1],b[1])),mul(a[0],b[0]));
    return {x,y,z,w};
}

CameraQuaternionCurve make_camera_quaternion_curve(const RootVector& start,const RootVector& rate,
    bool hemisphere,float damping) {
    check(start);check(rate);checked(damping);
    CameraQuaternionCurve c;
    c.previous=c.current=c.target=start;
    c.rate=rate;
    c.damping=damping;
    c.segment={start,rate,start,rate,0,0};
    c.remaining=0;
    c.hemisphere=hemisphere;
    return c;
}

void retime_camera_quaternion_curve(CameraQuaternionCurve& c,float duration) {
    if(!(checked(duration)>=0)) return;
    auto next=c;
    const float factor=mul(duration,next.segment.inverse_duration);
    next.segment.start_tangent=scaled(next.segment.start_tangent,factor);
    next.segment.end_tangent=scaled(next.segment.end_tangent,factor);
    next.segment.duration=duration;
    next.segment.inverse_duration=reciprocal(duration);
    next.remaining=duration;
    c=next;
}

void step_camera_quaternion_curve(CameraQuaternionCurve& c,float elapsed,float inverse_elapsed) {
    checked(elapsed);checked(inverse_elapsed);
    check(c.current);check(c.rate);check(c.target);checked(c.damping);
    check_segment_duration(c.segment.duration);checked(c.remaining);
    auto n=c;
    n.previous=n.current;                                   // 001325f0
    // 00180310 preupdate.
    if(n.remaining==n.segment.duration&&n.segment.duration>0) {
        if(n.hemisphere&&camera_dot4(n.target,n.current)<0) n.target=negated(n.target);
        n.segment=make_animation_vector_segment(n.segment.duration,n.current,n.rate,n.target,RootVector{});
    } else if(n.remaining!=0) {
        if(n.hemisphere&&camera_dot4(n.target,n.segment.end)<0) n.target=negated(n.target);
        n.segment.end=n.target;
        n.segment.end_tangent=scaled(RootVector{},n.segment.duration);
    }
    // 0017d540 timer, 0017d510 test.
    n.remaining=std::max(0.0F,sub(n.remaining,elapsed));
    if(n.remaining==0) n.current=n.target;
    else {
        // 0017d340 then post 00180550: multiply by rsqrt of the 4D squared length.
        const RootVector s=sample_animation_vector_segment(n.segment,sub(n.segment.duration,n.remaining));
        const float length2=add(add(add(mul(s[0],s[0]),mul(s[1],s[1])),mul(s[2],s[2])),mul(s[3],s[3]));
        if(!(length2>0)) throw std::runtime_error("Camera quaternion curve sampled a zero quaternion");
        const float inverse=checked(1.0F/std::sqrt(length2));
        n.current=scaled(s,inverse);
    }
    // 00180580 rate: extrapolate one more step of the frame rotation.
    const RootVector conjugate{sub(0,n.previous[0]),sub(0,n.previous[1]),sub(0,n.previous[2]),n.previous[3]};
    RootVector q=camera_quaternion_multiply(camera_quaternion_multiply(n.current,conjugate),n.current);
    if(n.hemisphere&&camera_dot4(q,n.current)<0) q=negated(q);
    RootVector d{sub(q[0],n.current[0]),sub(q[1],n.current[1]),sub(q[2],n.current[2]),sub(q[3],n.current[3])};
    d=scaled(d,inverse_elapsed);
    const float damping=std::max(0.0F,sub(1.0F,mul(n.damping,elapsed)));
    n.rate=scaled(d,damping);
    c=n;
}

void settle_camera_quaternion_curve(CameraQuaternionCurve& c) {
    c.current=c.target;
    c.previous=c.current;
    c.rate={};
}

void retime_camera_scalar_curve(CameraScalarCurve& c,float duration) {
    checked(duration);
    auto n=c;
    const float factor=mul(n.inverse_duration,duration);
    n.m0=mul(n.m0,factor);
    n.m1=mul(n.m1,factor);
    n.duration=duration;
    n.inverse_duration=reciprocal(duration);
    n.remaining=n.duration;
    c=n;
}

void step_camera_scalar_curve(CameraScalarCurve& c,float elapsed,float inverse_elapsed) {
    checked(elapsed);checked(inverse_elapsed);
    checked(c.current);checked(c.rate);checked(c.target);checked(c.damping);
    check_segment_duration(c.duration);checked(c.remaining);
    auto n=c;
    n.previous=n.current;
    // 0017c900 preupdate; the end rate is vtable +14 (00180090) = 0.
    if(n.remaining==n.duration&&n.duration>0) {
        n.p0=n.current;n.m0=mul(n.rate,n.duration);
        n.p1=n.target;n.m1=mul(0.0F,n.duration);
        n.inverse_duration=reciprocal(n.duration);
    } else if(n.remaining!=0) {
        n.p1=n.target;
        n.m1=mul(0.0F,n.duration);
    }
    // 0017c8a0 timer.
    const float left=sub(n.remaining,elapsed);
    n.remaining=0.0F<=left ? left : 0.0F;
    if(n.remaining==0) n.current=n.target;
    else {
        // 0017c7c0.
        const float t=n.duration==0 ? 1.0F : mul(sub(n.duration,n.remaining),n.inverse_duration);
        const float t2=mul(t,t),t3=mul(t2,t);
        const float three_t2=mul(3.0F,t2);
        const float a=add(1.0F,sub(mul(2.0F,t3),three_t2));
        const float b=add(mul(-2.0F,t3),three_t2);
        const float cc=add(t,sub(t3,mul(2.0F,t2)));
        const float d=sub(t3,t2);
        n.current=add(add(add(mul(a,n.p0),mul(b,n.p1)),mul(cc,n.m0)),mul(d,n.m1));
    }
    // 0017c760 rate.
    const float damping=sub(1.0F,mul(n.damping,elapsed));
    n.rate=mul(mul(inverse_elapsed,sub(n.current,n.previous)),0.0F<=damping ? damping : 0.0F);
    c=n;
}

}
