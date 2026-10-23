#include "hit_direction.hpp"
#include "game_libm.hpp"
#include <bit>
#include <cmath>

namespace omega {

HitDirection classify_hit_direction(const TargetingMatrix& f4,const RootVector& eye,const std::optional<RootVector>& position,
    std::int32_t base){
    float yaw=0.0F;                                                          // sp+108 starts 0
    if(position){
        TargetingMatrix frame=f4;frame[3]=eye;                               // the eye copied over row 3 (sp+f0)
        yaw=target_angles(frame,*position).angles[2];                        // 0022abc0
    }
    HitDirection out;
    const double wide=static_cast<double>(std::fabs(yaw));                   // 0011bd70
    const bool beyond=std::isnan(wide)||wide>1.57;                           // 00100260 against 0x3ff91eb851eb851f
    const float half_turn=std::bit_cast<float>(0x4048f5c3U);                 // 3.14
    if(beyond){
        yaw=yaw<=0.0F?yaw+half_turn:yaw-half_turn;
        out.code=base;
    }else out.code=static_cast<std::int32_t>((static_cast<std::uint32_t>(base)+1U)&0xffU);
    if(std::fabs(yaw)<=std::bit_cast<float>(0x3e99999aU))return out;        // 0.3
    const auto& r0=f4[0];const auto& r1=f4[1];
    const float h=r0[2]<r1[2]?game_atan2f(r0[1],r0[0]):game_atan2f(-r1[0],r1[1]);
    out.heading=h+yaw;
    return out;
}

}
