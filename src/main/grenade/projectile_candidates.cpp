#include "float_bits.hpp"
#include "projectile_candidates.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float value(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Projectile candidate arithmetic outside finite normal-or-zero domain");
    return v;
}
float add(float a,float b){return value(value(a)+value(b));}
float sub(float a,float b){return value(value(a)-value(b));}
float mul(float a,float b){return value(value(a)*value(b));}
// 0022bc04..0022bce0 (the transform, the extent, its squared length, vsqrt and the
// lateral ACC sum) are VU0 macro ops: binary32, round to nearest, overflow
// saturated to +-FLT_MAX, denormal results flushed to zero (as terrain_point_lights
// models them). A VUM declaring the empty bounds +FLT_MAX / -FLT_MAX (BELARUS2's
// NIE 0xa0000013) gets radius sqrt(FLT_MAX). Infinite or NaN inputs still throw.
float vu(float v){
    const auto b=std::bit_cast<std::uint32_t>(v);
    if((b&0x7f800000U)==0x7f800000U){
        if((b&0x7fffffU)!=0U)throw std::runtime_error("Projectile candidate VU0 arithmetic produced a NaN");
        return std::copysign(std::numeric_limits<float>::max(),v);
    }
    if((b&0x7f800000U)==0U)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
float vu_in(float v){
    if((std::bit_cast<std::uint32_t>(v)&0x7f800000U)==0x7f800000U)
        throw std::runtime_error("Projectile candidate arithmetic outside finite normal-or-zero domain");
    return vu(v);
}
float vadd(float a,float b){return vu(vu_in(a)+vu_in(b));}
float vsub(float a,float b){return vu(vu_in(a)-vu_in(b));}
float vmul(float a,float b){return vu(vu_in(a)*vu_in(b));}
RootVector transform(const ProjectileQueryMatrix& m,const RootVector& v){
    RootVector result{};
    for(unsigned row=0;row<4;++row){
        auto x=vmul(m[row],v[0]);x=vadd(x,vmul(m[4+row],v[1]));
        x=vadd(x,vmul(m[8+row],v[2]));result[row]=vadd(x,vmul(m[12+row],v[3]));
    }
    return result;
}
}
ProjectileQueryMatrix prepare_projectile_query_frame(const ProjectileQueryMatrix& world_frame){
    auto result=world_frame;for(float v:result)value(v);
    std::swap(result[1],result[4]);std::swap(result[2],result[8]);std::swap(result[6],result[9]);
    if(result[12]!=0||result[13]!=0||result[14]!=0){
        const RootVector negative{sub(0,result[12]),sub(0,result[13]),sub(0,result[14]),result[15]};
        for(unsigned row=0;row<3;++row){
            auto x=mul(result[row],negative[0]);x=add(x,mul(result[4+row],negative[1]));
            result[12+row]=add(x,mul(result[8+row],negative[2]));
        }
    }
    return result;
}
std::optional<float> projectile_candidate_distance(const ProjectileQueryMatrix& frame,
    const RootVector& position,const RootVector& minimum,const RootVector& maximum,float segment_length){
    const auto local=transform(frame,position);
    RootVector extent{};
    for(unsigned i=0;i<4;++i)extent[i]=vsub(vmul(vadd(minimum[i],maximum[i]),.5F),minimum[i]);
    const auto squared=vadd(vadd(vmul(extent[0],extent[0]),vmul(extent[1],extent[1])),vmul(extent[2],extent[2]));
    const auto radius=vu(static_cast<float>(std::sqrt(static_cast<double>(squared))));
    if(local[1]<=0)return std::nullopt;
    // COP1 from here: sub.s 0022bd00 and mul.s 0022bd40.
    if(!(sub(local[1],radius)<value(segment_length)))return std::nullopt;
    const auto lateral=vadd(vadd(vmul(local[0],local[0]),0),vmul(local[2],local[2]));
    if(!(value(lateral)<mul(radius,radius)))return std::nullopt;
    return local[1];
}
std::vector<ProjectileCandidateDistance> prepare_projectile_candidates(const ProjectileQueryMatrix& world_frame,
    std::span<const ProjectileCandidateGeometry> objects,float segment_length){
    if(objects.size()>65536)throw std::runtime_error("Projectile candidate snapshot exceeds bounded extent");
    const auto frame=prepare_projectile_query_frame(world_frame);
    std::vector<ProjectileCandidateDistance> result;
    for(const auto& object:objects){
        if(!object.identity)throw std::runtime_error("Projectile geometry requires a resolved nonnull object identity");
        const auto distance=projectile_candidate_distance(frame,object.position,object.minimum,object.maximum,segment_length);
        if(!distance)continue;
        if(result.size()==20)throw std::runtime_error("Projectile candidate overflow requires original diagnostic handling");
        result.push_back({object.identity,*distance});
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
    return result;
}
}
