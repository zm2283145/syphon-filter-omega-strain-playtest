#include "float_bits.hpp"
#include "actor_gather_bounds.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Actor gather bounds: unsupported nonfinite/subnormal arithmetic");
    return v;
}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
}

Bounds actor_gather_bounds(const ActorGatherBoundsInput& in,const ActorGatherBoundsTuning& tuning){
    float half=2.0F,lower=-3.5F,upper;
    // 1ea26c..: +660, then +5c0, then +3d3 (each read only when reached).
    if(in.byte_660)upper=tuning.upper_660;
    else if(in.byte_5c0)upper=tuning.upper_5c0;
    else if(!in.byte_3d3)upper=8.0F;
    else upper=add(tuning.upper_3d3_extra,8.0F);
    if(in.byte_480)lower=-8.0F;
    if(in.mode_20==9){lower=-8.0F;upper=14.0F;}
    for(bool slot:in.slot_3e0)if(slot){half=3.0F;break;}
    // vsub (all lanes) of +2f0 and +300.
    RootVector step{};for(std::size_t i=0;i<4;++i)step[i]=sub(in.predicted_2f0[i],in.position_300[i]);
    const float low_xy=sub(-half,1.0F),high_xy=add(half,1.0F);
    const RootVector minimum{low_xy,low_xy,sub(lower,1.0F),1.0F};
    const RootVector maximum{high_xy,high_xy,add(upper,1.0F),1.0F};
    // vadd.xyz of the step to both corners; W keeps 1.
    RootVector shifted_min=minimum,shifted_max=maximum;
    for(std::size_t i=0;i<3;++i){shifted_min[i]=add(minimum[i],step[i]);shifted_max[i]=add(maximum[i],step[i]);}
    // 00182760 copy, then 00182290: when the second box is valid (min.x <= max.x)
    // vmini/vmax with its minimum, then with its maximum (XYZ only).
    RootVector lo=minimum,hi=maximum;
    if(shifted_min[0]<=shifted_max[0]){
        for(const RootVector* p:{&shifted_min,&shifted_max})
            for(std::size_t i=0;i<3;++i){lo[i]=(*p)[i]<lo[i]?(*p)[i]:lo[i];hi[i]=(*p)[i]>hi[i]?(*p)[i]:hi[i];}
    }
    // vadd.xyz of the floor root's world position.
    for(std::size_t i=0;i<3;++i){lo[i]=add(lo[i],in.floor_root_position[i]);hi[i]=add(hi[i],in.floor_root_position[i]);}
    return {{lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]}};
}
}
