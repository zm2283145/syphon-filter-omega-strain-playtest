#include "float_bits.hpp"
#include "projectile_simulation.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Projectile integration outside finite normal-or-zero domain");
    return value;
}
float add(float a,float b){return checked(checked(a)+checked(b));}
float sub(float a,float b){return checked(checked(a)-checked(b));}
float mul(float a,float b){return checked(checked(a)*checked(b));}
float divide(float a,float b) {
    checked(a);checked(b);
    if(b==0)throw std::runtime_error("Projectile integration division by zero");
    return checked(a/b);
}
RootVector difference(const RootVector& a,const RootVector& b) {
    RootVector result{};for(unsigned i=0;i<4;++i)result[i]=sub(a[i],b[i]);return result;
}
RootVector sum(const RootVector& a,const RootVector& b) {
    RootVector result{};for(unsigned i=0;i<4;++i)result[i]=add(a[i],b[i]);return result;
}
RootVector scale(const RootVector& a,float value) {
    RootVector result{};for(unsigned i=0;i<4;++i)result[i]=mul(a[i],value);return result;
}
}
ProjectileIntegrationDraft prepare_projectile_integration(
    const ProjectileIntegrationState& state,const ProjectileIntegrationInput& input) {
    ProjectileIntegrationDraft result;result.state=state;
    if(state.retired)return result;
    checked(input.dt);
    if(input.dt<0)throw std::runtime_error("Projectile integration requires nonnegative time");
    auto& next=result.state;
    next.age=add(state.age,input.dt);
    if(state.motion_disabled) {
        result.boundary=ProjectileIntegrationBoundary::await_lifetime_continuation;
        return result;
    }
    if(input.dt==0)throw std::runtime_error("Moving projectile requires positive time");
    checked(state.inverse_mass);
    if(state.inverse_mass<0)throw std::runtime_error("Projectile inverse mass must be nonnegative");
    auto gravity=input.gravity;
    if(input.definition_type==7)gravity[2]=add(gravity[2],input.type7_gravity_offset);
    if(state.first_launch) {
        next.launch=scale(state.launch,input.dt);
        next.previous_position=difference(state.position,next.launch);
        next.force=sum(state.force,scale(gravity,input.launch_force_scale));
        next.previous_dt=input.dt;
        next.first_launch=false;
    }
    checked(next.previous_dt);
    if(next.previous_dt<=0)throw std::runtime_error("Projectile history requires positive prior time");
    const auto dt_squared=mul(input.dt,input.dt);
    const auto inverse_dt=divide(1,input.dt);
    const auto time_ratio=divide(input.dt,next.previous_dt);
    next.previous_dt=input.dt;
    const auto history=scale(difference(state.position,next.previous_position),time_ratio);
    const auto drift=sum(state.position,history);
    const auto force_delta=scale(next.force,mul(state.inverse_mass,dt_squared));
    next.position=sum(drift,force_delta);
    next.previous_position=state.position;
    //00183000 writes only W after all four integration lanes have executed.
    next.position[3]=1;
    const auto displacement=difference(next.position,next.previous_position);
    next.velocity=scale(displacement,inverse_dt);
    next.acceleration=scale(difference(next.velocity,state.velocity),inverse_dt);
    result.displacement=displacement;
    if(input.definition_type==7)
        result.gravity_z_after=sub(gravity[2],input.type7_gravity_offset);
    result.boundary=ProjectileIntegrationBoundary::await_world_continuation;
    return result;
}
}
