#include "float_bits.hpp"
#include "actor_physical_prediction.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Actor physical prediction requires finite normal-or-zero arithmetic");
    return value;
}
float add(float a,float b){return checked(checked(a)+checked(b));}
float sub(float a,float b){return checked(checked(a)-checked(b));}
float mul(float a,float b){return checked(checked(a)*checked(b));}
float div(float a,float b){
    checked(a);checked(b);
    if(b==0)throw std::runtime_error("Actor physical prediction division by zero");
    return checked(a/b);
}
RootVector sum(const RootVector& a,const RootVector& b){
    RootVector r{};for(unsigned i=0;i<4;++i)r[i]=add(a[i],b[i]);return r;
}
RootVector difference(const RootVector& a,const RootVector& b){
    RootVector r{};for(unsigned i=0;i<4;++i)r[i]=sub(a[i],b[i]);return r;
}
RootVector scale(const RootVector& v,float factor){
    RootVector r{};for(unsigned i=0;i<4;++i)r[i]=mul(v[i],factor);return r;
}
bool zero_xyz(const RootVector& v){return v[0]==0&&v[1]==0&&v[2]==0;}
}
ActorPhysicalPredictionDraft prepare_actor_physical_prediction(
    const ActorPhysicalPredictionState& state,float elapsed){
    checked(elapsed);
    if(elapsed<=0)throw std::runtime_error("Actor physical prediction requires positive elapsed time");
    checked(state.previous_dt);checked(state.inverse_mass);
    if(state.previous_dt<0||state.inverse_mass<0)
        throw std::runtime_error("Actor physical prediction time/inverse mass is outside bounded domain");
    for(const auto value:state.position)checked(value);
    bool override_present=false;
    for(const auto value:state.override_motion){checked(value);override_present|=value!=0;}
    ActorPhysicalPredictionDraft result{state,override_present,false};
    auto& next=result.state;
    const float inverse_dt=div(1,elapsed);
    if(override_present){
        // The override replaces accumulated force and history; no reads from
        // their overwritten values are required by this path.
        next.force={};next.acceleration={};next.previous_position=state.position;next.velocity={};
        const auto scaled_override=scale(state.override_motion,state.mass);
        next.force=sum(next.force,scale(scaled_override,inverse_dt));
    }
    const auto ratio=state.previous_dt==0?1.0F:div(elapsed,state.previous_dt);
    const auto dt_squared=mul(elapsed,elapsed);
    const auto drift=sum(next.position,scale(difference(next.position,next.previous_position),ratio));
    const auto force_displacement=scale(next.force,mul(state.inverse_mass,dt_squared));
    next.previous_position=next.position;
    next.position=sum(drift,force_displacement);
    next.position[3]=1;
    if(!override_present){
        const auto tentative_velocity=scale(difference(next.position,next.previous_position),inverse_dt);
        const auto tentative_acceleration=scale(difference(tentative_velocity,next.velocity),inverse_dt);
        if(!zero_xyz(tentative_acceleration)){
            const float length_squared=add(add(mul(tentative_acceleration[0],tentative_acceleration[0]),
                mul(tentative_acceleration[1],tentative_acceleration[1])),
                mul(tentative_acceleration[2],tentative_acceleration[2]));
            if(length_squared<=0)throw std::runtime_error("Actor acceleration normalization is undefined");
            const float length=checked(static_cast<float>(std::sqrt(static_cast<double>(length_squared))));
            const float reciprocal=div(1,length);
            auto direction=tentative_acceleration;
            for(unsigned i=0;i<3;++i)direction[i]=mul(direction[i],reciprocal);
            if(length>900){
                const auto limited_acceleration=scale(direction,900);
                const auto limited_velocity=sum(next.velocity,scale(limited_acceleration,elapsed));
                auto displacement=scale(limited_velocity,elapsed);
                displacement[3]=0;
                // Original keeps the predicted position and changes history.
                next.previous_position=difference(next.position,displacement);
                result.acceleration_limited=true;
            }
        }
    }
    const auto old_velocity=next.velocity;
    next.velocity=scale(difference(next.position,next.previous_position),inverse_dt);
    next.acceleration=scale(difference(next.velocity,old_velocity),inverse_dt);
    next.force={};
    return result;
}
}
