#include "float_bits.hpp"
#include "projectile_launch.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Projectile launch outside finite normal-or-zero domain");
    return value;
}
float add(float a,float b){return checked(a+b);}
float sub(float a,float b){return checked(a-b);}
float mul(float a,float b){return checked(a*b);}
float divide(float a,float b){if(b==0)throw std::runtime_error("Projectile launch division by zero");return checked(a/b);}
// FPU sqrt.s (003a8f04, 003a8f64, 003a8fb8, 003a8fec) and VU SQRT (003a8d84).
float square_root(float value){if(value<0)throw std::runtime_error("Projectile launch has no real solution");return checked(vu::sqrt(value));}
float norm_squared(const RootVector& vector) {
    return add(add(mul(vector[0],vector[0]),mul(vector[1],vector[1])),mul(vector[2],vector[2]));
}
void position(const RootVector& vector) {
    for(auto value:vector)checked(value);
    if(vector[3]!=1)throw std::runtime_error("Projectile launch requires world positions with W=1");
}
RootVector limit_speed(RootVector vector,float speed) {
    const auto squared=norm_squared(vector);
    if(squared<=0)throw std::runtime_error("Projectile minimum speed cannot normalize a zero vector");
    // 0025d5a8 / 0025d6a4: VU RSQRT Q = vf0.w/sqrt(|v|^2), then vmulq.xyz.
    const auto reciprocal=checked(vu::rsqrt(squared));
    for(unsigned index=0;index<3;++index)vector[index]=mul(vector[index],reciprocal);
    // The original normalizes XYZ, then scales all four lanes.
    for(auto& value:vector)value=mul(value,speed);
    return vector;
}
}
ProjectileTrajectory solve_projectile_trajectory(const RootVector& source,
    const RootVector& target,float gravity,float arc,float minimum_angle) {
    position(source);position(target);checked(gravity);checked(arc);checked(minimum_angle);
    if(gravity>=0)throw std::runtime_error("Projectile solver requires negative gravity");
    RootVector delta{};for(unsigned i=0;i<4;++i)delta[i]=sub(target[i],source[i]);
    const auto height=delta[2];delta[2]=0;
    RootVector direction{};float distance=0;
    if(delta[0]!=0||delta[1]!=0) {
        distance=square_root(norm_squared(delta));
        const auto reciprocal=divide(1,distance);
        direction=delta;
        for(unsigned i=0;i<3;++i)direction[i]=mul(direction[i],reciprocal);
    }
    float horizontal=0,vertical=0,terminal_delta=0;
    if(distance==0) {
        if(height>0) {
            vertical=square_root(mul(mul(-2,gravity),height));
            terminal_delta=-vertical;
        } else {
            terminal_delta=square_root(mul(mul(2,gravity),height));
        }
    } else {
        const auto elevation=checked(game_atan2f(height,distance));          // 003a8df0 -> 00121ab8
        constexpr auto half_pi=std::bit_cast<float>(0x3fc90fdbU);
        const auto headroom=elevation>0?sub(half_pi,elevation):half_pi;
        const auto offset=mul(arc,headroom);
        auto angle=add(elevation,offset>=0?offset:0.0F);
        if(angle<minimum_angle)angle=minimum_angle;
        constexpr auto limit=std::bit_cast<float>(0x3fc90fccU);
        if(angle< -limit)angle=-limit;
        if(angle>limit)angle=limit;
        const auto distance_squared=mul(distance,distance);
        if(angle==0) {
            horizontal=square_root(divide(mul(gravity,distance_squared),mul(2,height)));
        } else {
            const auto tangent=checked(game_tanf(angle));                      // 003a8ea4 -> 00121a10
            const auto numerator=mul(mul(gravity,distance_squared),mul(tangent,tangent));
            auto denominator=mul(2,sub(height,mul(tangent,distance)));
            if(denominator>=0)denominator=std::bit_cast<float>(0x80800000U);
            vertical=square_root(divide(numerator,denominator));
            if(tangent<0)vertical=-vertical;
            horizontal=divide(vertical,tangent);
        }
        terminal_delta=divide(mul(gravity,distance),horizontal);
    }
    ProjectileTrajectory result;
    for(unsigned i=0;i<4;++i)result.initial_velocity[i]=mul(direction[i],horizontal);
    result.initial_velocity[2]=vertical;
    result.terminal_velocity=result.initial_velocity;
    result.terminal_velocity[2]=add(vertical,terminal_delta);
    return result;
}
RootVector prepare_projectile_launch(const RootVector& source,const RootVector& target,
    float arc,std::uint8_t definition_type,std::int32_t maximum_speed,
    const ProjectileLaunchTuning& tuning) {
    checked(tuning.gravity);checked(tuning.type1_minimum_angle);checked(tuning.type7_gravity_offset);
    if(maximum_speed<1||maximum_speed>46340)
        throw std::runtime_error("Projectile speed outside positive safe integer-square domain");
    const auto gravity=definition_type==7?add(tuning.gravity,tuning.type7_gravity_offset):tuning.gravity;
    const auto angle=definition_type==1?tuning.type1_minimum_angle:-1000.0F;
    auto velocity=solve_projectile_trajectory(source,target,gravity,arc,angle).initial_velocity;
    const auto integer_square=maximum_speed*maximum_speed;
    if(norm_squared(velocity)>static_cast<float>(integer_square))
        velocity=limit_speed(velocity,static_cast<float>(maximum_speed));
    const auto fraction=definition_type==1?std::bit_cast<float>(0x3dcccccdU):std::bit_cast<float>(0x3f666666U);
    const auto minimum=mul(static_cast<float>(maximum_speed),fraction);
    if(norm_squared(velocity)<mul(minimum,minimum))velocity=limit_speed(velocity,minimum);
    return velocity;
}
}
