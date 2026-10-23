#include "hud_crosshair.hpp"
#include <bit>
#include <stdexcept>

namespace omega {

std::int32_t hud_crosshair_index(bool aim_1e0,bool far_200,const std::optional<HudCrosshairRecord>& held,
    const std::optional<std::int32_t>& goggles_130,bool camera_matches_ffc10){
    std::int32_t s2=-1;
    if(!aim_1e0)return s2;                                             // 0x00241eec
    if(!far_200&&goggles_130){                                         // 0x00241ef4..0x00241f0c
        s2=*goggles_130+1;
        if(s2>=0)return s2;
    }
    if(!held)return s2;                                                // 0x00241f18
    const std::uint32_t hold=static_cast<std::uint32_t>(held->class_f0)-3U;
    if(hold<8U&&((1U<<hold)&0x83U)!=0)return s2;                       // 0x00241f20..0x00241f50
    if(!camera_matches_ffc10)return s2;                                // 0x00241f58..0x00241f84
    if(!far_200&&held->index_130>=0)return held->index_130+1;          // 0x00241f8c..0x00241fa4
    return 0;                                                          // 0x00241fa8..0x00241fc0 (s2 < 0 here)
}

namespace {
const std::optional<float>& element(const HudCrosshairState& hud,std::int32_t k){
    if(k<0||static_cast<std::size_t>(k)>=hud.elements_84.size())
        throw std::runtime_error("HUD reticle index outside the +84 elements (00241ec0)");
    return hud.elements_84[static_cast<std::size_t>(k)];
}
}

HudCrosshairResult hud_crosshair_update(HudCrosshairState& hud,std::int32_t s2,
    const std::optional<HudCrosshairRecord>& held){
    using K=HudCrosshairEvent::Kind;
    HudCrosshairResult r;
    r.index=s2;
    r.value_346e=s2>=0&&held?held->value_100:std::int8_t{0};          // 0x00241fc8..0x00241fe0
    auto& ev=r.events;
    if(s2!=hud.index_8c){                                              // 0x00241fe8
        if(s2==0){
            ev.push_back({K::default_a10,-1,0});                       // 0x00242004
        }else if(s2>0){
            if(element(hud,s2-1)){                                     // 0x0024202c
                ev.push_back({K::visible_de0,s2-1,1});
                ev.push_back({K::alpha_5a0,s2-1,std::bit_cast<std::uint32_t>(1.0F)});
                ev.push_back({K::fade_4e0,s2-1,std::bit_cast<std::uint32_t>(10.0F)});
            }
            ev.push_back({K::release_a00,-1,0});                       // 0x00242060
        }
        const std::int32_t old=hud.index_8c;                           // 0x00242068
        if(old==0){
            ev.push_back({K::release_a00,-1,0});                       // 0x0024207c
        }else if(old>0&&element(hud,old-1)){
            ev.push_back({K::fade_4e0,old-1,0});                       // 0x002420b0 (0.0)
        }
    }
    if(s2<0)ev.push_back({K::clear_9a0,-1,0});                         // 0x002420c0
    if(held&&held->class_f0==1)ev.push_back({K::clear_9a0,-1,0});     // 0x002420c8..0x002420e0
    const auto count=static_cast<std::int32_t>(hud.elements_84.size());
    for(std::int32_t k=0;k<count;++k){                                 // 0x002420e8..0x0024214c
        if(k==s2-1)continue;
        const auto& e=element(hud,k);
        if(!e)throw std::runtime_error("HUD reticle element is null in the hide loop (00241ec0 at 0x00242118)");
        if(*e==0.0F)ev.push_back({K::visible_de0,k,0});                // c.eq.s 0.0, 00242180, 003e8de0(e, 0)
    }
    hud.index_8c=s2;                                                   // 0x00242158
    return r;
}

}