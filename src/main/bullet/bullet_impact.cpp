#include "bullet_impact.hpp"
#include "vu_float.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
// VU results flush denormals to zero (sign kept); each product and sum is one
// binary32 operation (VU_ROUNDING.md).
float vu_flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float vu_mul(float a,float b){return vu_flush(a*b);}
float vu_add(float a,float b){return vu_flush(a+b);}
float vu_sub(float a,float b){return vu_flush(a-b);}
// acc - s * t: the vopmsub product is rounded but not flushed; only the result is.
float vu_msub(float acc,float s,float t){const float p=s*t;return vu_flush(acc-p);}
// 001320f0: vopmula ACC = a.yzx * b.zxy, vopmsub - b.yzx * a.zxy, W = vf0.w - vf0.w = 0.
RootVector vu_cross(const RootVector& a,const RootVector& b){
    return {vu_msub(vu_mul(a[1],b[2]),b[1],a[2]),vu_msub(vu_mul(a[2],b[0]),b[2],a[0]),
            vu_msub(vu_mul(a[0],b[1]),b[0],a[1]),0.0F};
}
// 00132060 / 00132030: (x*x + y*y) + z*z, Q = RSQRT (FLT_MAX for a zero sum),
// xyz *= Q, W kept.
RootVector vu_normalize(RootVector v){
    const float s=vu_add(vu_add(vu_mul(v[0],v[0]),vu_mul(v[1],v[1])),vu_mul(v[2],v[2]));
    const float q=s==0.0F?std::numeric_limits<float>::max():vu_flush(vu::rsqrt(s));
    for(std::size_t i=0;i<3;++i)v[i]=vu_mul(v[i],q);
    return v;
}
constexpr float flip_limit=0.70700002F;      // 0x3f34fdf4
constexpr float lift=0.08F;                  // 0x3da3d70a
constexpr float default_decal_size=0.2F;     // 0x3e4ccccd
constexpr std::uint8_t default_decal_mode=8;
}

RootVector bullet_impact_step(const RootVector& before,const RootVector& after){
    RootVector d{};
    for(std::size_t i=0;i<4;++i)d[i]=vu_sub(after[i],before[i]);
    return d;
}

std::array<RootVector,4> bullet_impact_frame(const RootVector& normal_c0,const RootVector& step_ad0,const RootVector& point_100){
    RootVector n=normal_c0;
    // vmul.xyzw, then ACC.w = x, + y, + z (the mtsah 6 / qfsrv pair takes w).
    const float dot=vu_add(vu_add(vu_mul(n[0],step_ad0[0]),vu_mul(n[1],step_ad0[1])),vu_mul(n[2],step_ad0[2]));
    if(!(dot<=0.0F))                         // c.ole.s dot, 0 / bc1t
        for(auto& c:n)c=vu_sub(0.0F,c);      // vmulax ACC = 0, vmsubw: 0 - n
    // 00132250 (+8) and fabs 001d3e10.
    const RootVector ref=std::fabs(n[2])<flip_limit?RootVector{0,0,1,0}:RootVector{1,0,0,0};
    const RootVector row1=vu_normalize(n);
    const RootVector row0=vu_normalize(vu_cross(n,ref));
    const RootVector row2=vu_cross(row0,row1);
    RootVector row3{};
    for(std::size_t i=0;i<4;++i)row3[i]=vu_add(point_100[i],vu_mul(n[i],lift));
    return {row0,row1,row2,row3};
}

BulletImpact bullet_static_impact(const MaterialRow& row,std::uint8_t item_kind_48,const RootVector& normal_c0,
                                  const RootVector& step_ad0,const RootVector& point_100){
    BulletImpact out;
    std::uint8_t s=item_kind_48==6||item_kind_48==4?3:item_kind_48==2?2:0;
    const auto& p=row.props;
    // Sound: a -1 bank of a nonzero property takes property 0's bank and name.
    bool zero=p[s].bank==-1&&s!=0;
    const std::int32_t bank=zero?p[0].bank:p[s].bank;
    if(bank==-1)out.sound=BulletImpact::Sound::default_hit;
    else if(bank!=-2){
        out.sound=BulletImpact::Sound::named;
        out.bank=bank;
        out.sound_name=p[zero?0:s].sound;
    }
    out.frame=bullet_impact_frame(normal_c0,step_ad0,point_100);
    const std::int32_t effect=p[s].effect_id==-1&&s!=0?p[0].effect_id:p[s].effect_id;
    if(effect==-1)out.effect=BulletImpact::Effect::table;
    else if(effect!=-2){out.effect=BulletImpact::Effect::id;out.effect_id=effect;}
    if(p[s].decal_id==-1&&s!=0)s=0;
    const std::int32_t decal=p[s].decal_id;
    if(decal==-1){
        out.decal=BulletImpact::Decal::table;
        out.decal_size=default_decal_size;
        out.decal_mode=default_decal_mode;
    }else if(decal!=-2){
        out.decal=BulletImpact::Decal::id;
        out.decal_id=decal;
        out.decal_size=p[s].size;
        // lb a3, sp+b9c[blend]: the 3-byte copy of 0048b350, unchecked.
        if(p[s].blend>=bullet_impact_decal_modes.size())
            throw std::runtime_error("Bullet impact: a decal blend outside 0048b350 reads the stack");
        out.decal_mode=bullet_impact_decal_modes[p[s].blend];
    }
    out.property=s;
    return out;
}

}