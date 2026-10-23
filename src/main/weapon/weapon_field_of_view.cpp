#include "weapon_field_of_view.hpp"
#include "actor_fire.hpp"
#include "game_libm.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& reason){throw std::runtime_error("Weapon field of view: "+reason);}
// COP1 single precision: the result's denormals flush to a signed zero.
float fpu(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return v!=0.0F&&std::fabs(v)<std::numeric_limits<float>::min()?std::copysign(0.0F,v):v;
}
float add(float a,float b){return fpu(a+b);}
float sub(float a,float b){return fpu(a-b);}
float mul(float a,float b){return fpu(a*b);}
float divide(float a,float b){
    if(b==0.0F)fail("division by zero");
    return fpu(a/b);
}
float maximum(float s,float t){return fpu(t>s?t:s);}      // max.s fd, fs, ft
float minimum(float s,float t){return fpu(t<s?t:s);}      // min.s fd, fs, ft
const float degree=std::bit_cast<float>(0x3c8efa35U);    // pi / 180 (00143100, 001f05d0)
const float radian=std::bit_cast<float>(0x42652ee0U);    // 57.29578 (001433cc)
constexpr float unzoomed=65.0F;                           // 0x42820000 (00142f24)
const float step_limit=std::bit_cast<float>(0x3e99999aU); // 00489ca8, 0.3
constexpr float step_rate=7.0F;                           // 00489cb0
const float settled=std::bit_cast<float>(0x3c23d70aU);    // 0.01 (00143140)
const float moving=std::bit_cast<float>(0x3a83126fU);     // 0.001 (00143168)
}

float weapon_zoom_angle(float zoom,float angle){
    // 00143370: f12 = pi/180 x a / 2.0; tanf 00121a10; / z; atanf 001212e0;
    // 57.29578 x (2.0 x atan).
    const float half=divide(mul(degree,angle),2.0F);
    const float t=fpu(game_tanf(half));
    const float a=fpu(game_atanf(divide(t,zoom)));
    return mul(radian,mul(2.0F,a));
}

float weapon_field_of_view(WeaponInventory& inv,const WeaponDatabase& db,const WeaponFieldOfViewInput& in){
    if(in.zoom<-1||in.zoom>1)fail("zoom direction outside -1..1");
    fpu(in.current_c64);fpu(in.elapsed);
    float f23=unzoomed;
    float f22=in.current_c64;
    bool aim=in.aim;
    bool s0=false,s1=false;
    // 00142f6c..00142f84: 001437a0 (andi 0xff) == 1 clears the aim argument.
    if(static_cast<std::uint8_t>(weapon_held_class(inv,db))==1)aim=false;
    // 00142f88..00142fa0: a change of the aim argument stores it to +ed and sets +ee.
    if(inv.byte_ed!=static_cast<std::uint8_t>(aim)){inv.byte_ed=static_cast<std::uint8_t>(aim);inv.byte_ee=1;}
    if(inv.current_84!=weapon_no_slot){                                   // 00142fa8
        if(inv.current_84>=inv.slots.size())fail("current slot outside the slot array");
        const auto item=inv.slots[inv.current_84].item;                 // (+84 << 4) + 8
        if(item!=-1){                                                     // 00142fc8
            const auto& rec=db.record(item);                              // 00147840(*004ffd30, item)
            const float f20=weapon_zoom_angle(rec.real(0x110),unzoomed);
            const float f0=weapon_zoom_angle(rec.real(0x114),unzoomed);
            if(aim){                                                      // 00143004
                const float f4=mul(static_cast<float>(in.zoom),in.elapsed);   // cvt.s.w a2 x f13
                float f2=0.0F;
                const auto steps=rec.word(0xb4);
                if(steps!=0){                                             // 0014301c
                    if(inv.byte_ec==0&&!(f4==0.0F)){                      // 00143024..00143034
                        const float sign=f4<=0.0F?-1.0F:1.0F;             // 0014303c..00143058
                        f2=divide(mul(sub(f0,f20),sign),static_cast<float>(steps));
                    }
                    inv.byte_ec=0.0F==f4?0:1;                             // 00143070..0014308c
                }else{
                    f2=divide(mul(f4,sub(f0,f20)),rec.real(0xac));        // 00143090..0014309c
                }
                // 001430a0..001430b4: +e8 = min(max(+e8 + f2, f0), f20).
                f23=minimum(maximum(add(inv.value_e8,f2),f0),f20);
                inv.value_e8=f23;
            }else if(!in.byte_4ea140&&inv.byte_ee==0){                    // 001430b8..001430d4
                inv.value_e8=f20;
            }
            s0=rec.word(0xb0)!=0&&f0<f20;                                 // 001430d8..001430f8
        }
    }
    const float target=mul(degree,f23);                                   // 00143100..00143110
    if(s0||inv.byte_ee!=0){                                               // 0014310c..00143118
        // 00143120..00143160: f22 += 7 x (clamp(target - f22, -0.3, 0.3) x dt).
        const float clamped=minimum(maximum(sub(target,f22),fpu(-step_limit)),step_limit);
        const float step=mul(step_rate,mul(clamped,in.elapsed));
        if(std::fabs(step)<settled)inv.byte_ee=0;                         // 00143154..00143164
        f22=add(f22,step);
        if(!(std::fabs(step)<=moving)&&s0&&in.zoom!=0)s1=true;            // 00143168..00143198
    }else{
        f22=target;                                                       // 001431a0
    }
    if(s1){
        if(inv.byte_f0==0){                                               // 001431a8..00143224
            if(!in.zoom_sound)fail("the zoom sound start (0020f990 / 003c9420) has no host");
            in.zoom_sound(true);
            inv.byte_f0=1;                                                // 001431f8
        }
    }else if(inv.byte_f0!=0){                                             // 00143230..0014329c
        if(!in.zoom_sound)fail("the zoom sound stop (0020eba0 / 003c9420) has no host");
        in.zoom_sound(false);
        inv.byte_f0=0;                                                    // 00143270
    }
    return f22;
}

float pad_field_of_view_clamp(float fov){
    const float low=mul(degree,std::bit_cast<float>(0x3c23d70aU));       // 001f05d0(0.01)
    const float high=mul(degree,std::bit_cast<float>(0x4333fd71U));      // 001f05d0(179.99)
    return minimum(maximum(fpu(fov),low),high);                           // 00192740
}

}
