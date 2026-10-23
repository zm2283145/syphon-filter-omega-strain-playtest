#include "skm_vision_shading.hpp"

#include <bit>
#include <cmath>

namespace omega {

std::array<ShadowVector,3> skm_thermal_rows_00387c00(const ShadowMatrix& view){
    const ShadowMatrix inv=shadow_rigid_inverse(view);
    const ShadowVector r0=shadow_transform(inv,{-1.0F,0.0F,0.0F,0.0F});
    const ShadowVector r1=shadow_transform(inv,{0.0F,0.0F,-1.0F,0.0F});
    std::array<ShadowVector,3> rows{};
    for(std::size_t j=0;j<3;++j)rows[j]={r0[j],r1[j],0.0F,0.0F};
    return rows;
}

std::array<float,2> skm_thermal_uv(const std::array<ShadowVector,3>& rows,const std::array<float,3>& n){
    std::array<float,2> uv{};
    for(std::size_t c=0;c<2;++c)uv[c]=0.5F+0.5F*(rows[0][c]*n[0]+rows[1][c]*n[1]+rows[2][c]*n[2]);
    return uv;
}

std::uint32_t skm_night_gif_tag(std::uint32_t flags_2bd4){
    return (flags_2bd4&0x80U)?0x303E4000U:0x302E4000U;
}

bool skm_night_ghost_drawn(std::uint32_t stamp,std::uint32_t d0,std::uint32_t s0){
    return !(stamp<d0-s0-1U);
}

std::uint32_t skm_night_ghost_alpha(std::uint32_t s0,std::uint32_t s7){
    const auto k=static_cast<std::int32_t>((5U-s0)*s7);
    const float f=std::bit_cast<float>(0x3DCCCCCDU)*static_cast<float>(k);
    if(f>=2147483648.0F)return static_cast<std::uint32_t>(static_cast<std::int32_t>(f-2147483648.0F))|0x80000000U;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(f));
}

void skm_night_stamp_shift(std::array<std::uint32_t,skm_night_slots>& stamps,std::uint32_t d0){
    for(std::size_t i=skm_night_slots-1;i>0;--i)stamps[i]=stamps[i-1];
    stamps[0]=d0;
}

}  // namespace omega
