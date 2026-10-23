#include "animation_rotation_curve.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace omega {
namespace {
float finite(float value){
    if(!std::isfinite(value))throw std::runtime_error("Angle curve requires finite arithmetic");
    return value;
}
float add(float a,float b){return finite(a+b);}
float sub(float a,float b){return finite(a-b);}
float mul(float a,float b){return finite(a*b);}
constexpr float pi=std::bit_cast<float>(std::uint32_t{0x40490fdb});
constexpr float tau=std::bit_cast<float>(std::uint32_t{0x40c90fdb});
float difference(float value){
    finite(value);
    // Repeated binary32 additions are observable and differ from fmod.
    // Bound work and reject inputs for which the original loop cannot advance.
    for(unsigned count=0;count<65536;++count){
        if(value<=pi && value>=-pi)return value;
        const float next=add(value,value>pi?-tau:tau);
        if(next==value)throw std::runtime_error("Angle curve wrapping made no progress");
        value=next;
    }
    throw std::runtime_error("Angle curve wrapping exceeds bounded iteration count");
}
void validate(const AnimationRotationCurveState& s){
    for(float x:{s.current,s.rate,s.target,s.damping,s.curve.start,s.curve.start_tangent,
        s.curve.end,s.curve.end_tangent,s.curve.duration,s.curve.inverse_duration,s.remaining})finite(x);
    if(s.curve.duration<0||s.remaining<0)throw std::runtime_error("Angle curve timer is negative");
}
float sample(const AnimationCurve& c,float remaining){
    const float t=c.duration==0?1:mul(sub(c.duration,remaining),c.inverse_duration);
    const float t2=mul(t,t),t3=mul(t2,t),a=mul(3,t2),b=mul(2,t3);
    const float h00=add(1,sub(b,a)),h01=add(mul(-2,t3),a);
    const float endpoints=add(mul(h00,c.start),mul(h01,c.end));
    const float h10=add(t,sub(t3,mul(2,t2)));
    const float start_term=add(mul(h10,c.start_tangent),endpoints);
    return add(start_term,mul(sub(t3,t2),c.end_tangent));
}
}
AnimationRotationCurveState make_animation_rotation_curve(float damping){
    AnimationRotationCurveState state;state.damping=finite(damping);return state;
}
void target_animation_rotation_curve(AnimationRotationCurveState& state,float target,float duration){
    finite(target);finite(duration);auto next=state;next.target=target;
    if(duration>=0){
        const float scale=mul(duration,finite(next.curve.inverse_duration));
        next.curve.start_tangent=mul(finite(next.curve.start_tangent),scale);
        next.curve.end_tangent=mul(finite(next.curve.end_tangent),scale);
        next.curve.duration=duration;
        next.curve.inverse_duration=duration==0?0:finite(1.0F/duration);
        next.remaining=duration;
    }
    state=next;
}
bool step_animation_rotation_curve(AnimationRotationCurveState& state,float elapsed){
    if(finite(elapsed)<=0)throw std::runtime_error("Angle curve elapsed time must be positive");
    const float inverse_elapsed=finite(1.0F/elapsed);
    validate(state);auto next=state;next.previous=next.current;
    auto& c=next.curve;
    if(next.remaining==c.duration && c.duration>0){
        next.target=add(next.current,difference(sub(next.target,next.current)));
        c.start=next.current;c.start_tangent=mul(next.rate,c.duration);
        c.end=next.target;c.end_tangent=mul(0,c.duration);
        c.inverse_duration=finite(1.0F/c.duration);
    }else if(next.remaining!=0){
        next.target=add(c.end,difference(sub(next.target,c.end)));
        c.end=next.target;c.end_tangent=mul(0,c.duration);
    }
    const float before=next.remaining;
    next.remaining=std::max(0.0F,sub(next.remaining,elapsed));
    if(next.remaining==0)next.current=next.target;
    else {
        next.current=sample(c,next.remaining);
        // Original001b8ac0 performs wrapping in registers, then returns without
        // storing it. Keep the sampled angle, including values outside +/-pi.
        (void)difference(next.current);
    }
    const float velocity=mul(inverse_elapsed,difference(sub(next.current,next.previous)));
    const float damping=std::max(0.0F,sub(1,mul(next.damping,elapsed)));
    next.rate=mul(velocity,damping);
    state=next;return before>0&&next.remaining==0;
}
}
