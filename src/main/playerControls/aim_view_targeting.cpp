#include "aim_view_targeting.hpp"
#include "player_weapon_input.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* where,const char* what){
    if(!f)throw std::runtime_error(std::string(where)+": host "+what+" required");
    return f;
}
float word_float(std::int32_t v){return static_cast<float>(v);}    // cvt.s.w (the records' words are far below 2^24)
}

ShotFrame aim_view_shot_frame(const ShotFrame& m){
    ShotFrame out=m;                                                     // 00176440..00176480
    out[3]=pad_transform_point(m,{0.0F,1.0F,0.0F,1.0F});                 // 001764a8..001764cc
    return out;
}

FlashlightProbeRequest aim_view_lock_request(std::uint64_t actor,const ShotFrame& frame,float range){
    FlashlightProbeRequest p;
    p.owner=actor;
    p.frame=frame;                                                       // +4 (00249b7c)
    p.range=range;                                                       // +14 (00249b8c)
    p.hit_radius=std::bit_cast<float>(0x3e4ccccdU);                      // +28 (00249b80)
    p.los_mask=0x10;                                                     // a2 (00249b9c)
    return p;
}

AimViewLockResult aim_view_lock(const AimViewLockHost& h){
    constexpr const char* where="Aim-view lock 00249b10";
    AimViewLockResult out;
    const ShotFrame frame=need(h.shot_frame,where,"shot_frame")();       // 00249b68
    const float range=need(h.range,where,"range")();                     // 0013faf0
    const auto probe=need(h.probe,where,"probe")(aim_view_lock_request(h.actor,frame,range));   // 0022a8c0
    std::int32_t category=probe.category_30;
    const std::uint64_t object=probe.victim_38;
    if(object!=0&&!need(h.enemy,where,"enemy")(object)){
        out.hud.push_back({AimViewHudEvent::Kind::reticle_friend_9d0});   // 00249bc4
        category=0;                                                      // 00249bc8
    }else out.hud.push_back({AimViewHudEvent::Kind::reticle_clear_9a0}); // 00249bdc
    bool headshot=false;
    if(category==2){
        const auto life=need(h.life_108,where,"life_108")(object);       // 00249c08
        headshot=!(life==2||life==3);
    }
    AimViewHudEvent meter{AimViewHudEvent::Kind::meter_960};
    meter.value=category==0?1.0F:0.0F;                                   // 00249c50 / 00249c68
    out.hud.push_back(meter);
    if(headshot){
        AimViewHudEvent popup{AimViewHudEvent::Kind::popup_2fa0};
        popup.object_id=need(h.object_id,where,"object_id")(object);      // object +c (00249c90)
        out.hud.push_back(popup);
    }
    out.range_f4=probe.step;                                             // 00249ccc
    out.object=object;                                                   // 00249cd0
    return out;
}

AimViewThrownResult aim_view_thrown_point(const AimViewThrownHost& h){
    constexpr const char* where="Aim-view thrown point 002497c0";
    AimViewThrownResult out;
    ShotFrame m=need(h.camera,where,"camera")();                         // 002497e8..00249834
    AimViewHudEvent meter{AimViewHudEvent::Kind::meter_960};meter.value=0.0F;
    out.hud.push_back(meter);                                            // 0024983c
    const RootVector origin=pad_transform_point(m,{0.0F,1.5F,0.0F,1.0F});   // 00249898
    m[3]=origin;                                                         // 002498b8: replaces the copy's row 3 (sp+230)
    const float range=word_float(need(h.record_3c,where,"record_3c")());   // 002498c4
    FlashlightLosRequest q;
    q.origin=origin;                                                     // +30
    for(std::size_t i=0;i<4;++i)q.displacement[i]=m[1][i]*range;         // +40 (vmulx.xyzw)
    q.origin[3]=1.0F;                                                    // +3c (002499b4)
    q.displacement[3]=0.0F;                                              // +4c (002499bc)
    q.owner=h.actor;                                                     // +50
    q.mask=0x10;                                                         // +58
    const auto hit=need(h.los,where,"los")(q);                           // 002499cc
    const RootVector far=pad_transform_point(m,{0.0F,range,0.0F,1.0F});  // 00249a14
    if(hit){
        out.point=hit->point;                                            // 00249a4c
        out.direction=hit->surface_50;
        out.hit=true;
    }else{
        out.point=far;                                                   // 00249a74
        for(std::size_t i=0;i<4;++i)out.direction[i]=0.0F-m[1][i];      // vmulax ACC 0, vmsubw
    }
    AimViewHudEvent marker{AimViewHudEvent::Kind::thrown_580};
    marker.point=out.point;marker.direction=out.direction;
    out.hud.push_back(marker);                                           // 00249ab8
    return out;
}

}
