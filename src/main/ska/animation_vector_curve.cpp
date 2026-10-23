#include "animation_vector_curve.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Vector curve requires finite arithmetic");
    return value;
}
void check(const RootVector& value) { for (float x:value) checked(x); }
float add(float a,float b) { return checked(a+b); }
float sub(float a,float b) { return checked(a-b); }
float mul(float a,float b) { return checked(a*b); }
float reciprocal(float duration) { return duration==0 ? 0 : checked(1.0F/duration); }
void duration_check(float value) {
    if (checked(value)<0) throw std::runtime_error("Vector curve duration is negative");
}
void check(const AnimationVectorSegment& s) {
    check(s.start);check(s.end);check(s.start_tangent);check(s.end_tangent);
    duration_check(s.duration);checked(s.inverse_duration);
}
void check_retained(const AnimationVectorCurveState& s) {
    check(s.current);check(s.rate);check(s.target);checked(s.damping);
    check(s.segment);duration_check(s.remaining);
}
RootVector scaled(const RootVector& vector,float scale) {
    RootVector result;
    for (std::size_t i=0;i<4;++i) result[i]=mul(vector[i],scale);
    return result;
}
void rebuild(AnimationVectorCurveState& state) {
    state.segment=make_animation_vector_segment(state.segment.duration,
        state.current,state.rate,state.target,RootVector{});
}
}

AnimationVectorSegment make_animation_vector_segment(float duration,
    const RootVector& start,const RootVector& start_rate,
    const RootVector& end,const RootVector& end_rate) {
    duration_check(duration);check(start);check(start_rate);check(end);check(end_rate);
    return {start,scaled(start_rate,duration),end,scaled(end_rate,duration),
        duration,reciprocal(duration)};
}

RootVector sample_animation_vector_segment(const AnimationVectorSegment& s,float time) {
    check(s);
    // A zero-duration segment chooses its end without consuming the time value.
    const float t=s.duration==0 ? 1.0F : mul(checked(time),s.inverse_duration);
    const float t2=mul(t,t), t3=mul(t2,t), three_t2=mul(3.0F,t2);
    const float a=add(1.0F,sub(mul(2.0F,t3),three_t2));
    const float b=add(mul(-2.0F,t3),three_t2);
    const float c=add(t,sub(t3,mul(2.0F,t2))), d=sub(t3,t2);
    RootVector result;
    for(std::size_t i=0;i<4;++i) {
        const float endpoints=add(mul(s.start[i],a),mul(s.end[i],b));
        const float start_tangent=add(endpoints,mul(s.start_tangent[i],c));
        result[i]=add(start_tangent,mul(s.end_tangent[i],d));
    }
    return result;
}

void retime_animation_vector_curve(AnimationVectorCurveState& state,float duration) {
    checked(duration);if(duration<0) return;
    auto next=state;
    check(next.segment);
    const float scale=mul(duration,next.segment.inverse_duration);
    next.segment.start_tangent=scaled(next.segment.start_tangent,scale);
    next.segment.end_tangent=scaled(next.segment.end_tangent,scale);
    next.segment.duration=duration;
    next.segment.inverse_duration=reciprocal(duration);
    next.remaining=duration;
    state=next;
}

void set_animation_vector_target(AnimationVectorCurveState& state,const RootVector& value) {
    check(value);state.target=value;
}

void reset_animation_vector_current(AnimationVectorCurveState& state,const RootVector& value) {
    check(value);auto next=state;next.current=value;
    rebuild(next);state=next;
}

void rebase_animation_vector_current(AnimationVectorCurveState& state,const RootVector& value) {
    check(value);auto next=state;next.current=value;next.rate={};
    rebuild(next);state=next;
}

bool step_animation_vector_curve(AnimationVectorCurveState& state,float elapsed) {
    if(checked(elapsed)<=0) throw std::runtime_error("Vector curve elapsed time must be positive");
    const float inverse_elapsed=checked(1.0F/elapsed);
    check_retained(state);
    auto next=state;
    next.previous=next.current;
    if(next.remaining==next.segment.duration && next.segment.duration>0) rebuild(next);
    else if(next.remaining!=0) {
        next.segment.end=next.target;
        next.segment.end_tangent=scaled(RootVector{},next.segment.duration);
    }
    const float old_remaining=next.remaining;
    next.remaining=std::max(0.0F,sub(next.remaining,elapsed));
    if(next.remaining==0) next.current=next.target;
    else {
        next.current=sample_animation_vector_segment(next.segment,sub(next.segment.duration,next.remaining));
        next.current[3]=1.0F;
    }
    const float damping=std::max(0.0F,sub(1.0F,mul(next.damping,elapsed)));
    for(std::size_t i=0;i<4;++i) {
        const float velocity=mul(sub(next.current[i],next.previous[i]),inverse_elapsed);
        next.rate[i]=mul(velocity,damping);
    }
    state=next;
    return old_remaining>0 && next.remaining==0;
}
}
