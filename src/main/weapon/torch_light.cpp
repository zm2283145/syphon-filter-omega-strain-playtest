#include "torch_light.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// Interpreter model (as flashlight.cpp): binary32, round to nearest, denormal
// results flushed to zero; results must stay finite.
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Torch light: "+what);}
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){
    if(!std::isfinite(v))fail("value not finite");
    return ftz(v);
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float fdiv(float a,float b){
    if(b==0.0F)fail("division by zero");
    return check(vu::div(a,b));
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)fail(std::string(what)+" is not hosted");
    return f;
}
LevelLights& pool(const TorchLightHost& h){
    if(!h.lights)fail("the light manager 00535d20 (00373e70 / 00373d70) is not hosted");
    return *h.lights;
}
LevelLight& entry(const TorchLightHost& h,std::size_t index){
    auto& p=pool(h).point;
    if(index>=p.size())fail("light index outside the point pool");
    return p[index];
}
// rand / 2^31 (cvt.s.w toward zero, then div.s by 2^31).
float unit(const TorchLightHost& h){
    return fdiv(game_int_to_float(static_cast<std::int32_t>(need(h.random,"00127dd0")())),2147483648.0F);
}
}

bool torch_lit(std::uint32_t held_class,std::uint32_t life_state){
    return (held_class&0xffU)==10&&(life_state&0xffU)!=3;
}

bool update_torch_light(WeaponInventory& inv,bool lit,float dt,const TorchLightHost& h){
    bool changed=false;
    auto& obj=inv.torch_flicker_d8;
    if(!lit){                                                            // 001448a8
        if(inv.torch_light_d4){
            auto& l=entry(h,*inv.torch_light_d4);                        // vtable +8: 00144a60(1)
            l.id=-1;
            l.enabled=false;                                             // vcall +30: 00144980
            l.vtable=level_light_vtable_released;
            changed=true;
            inv.torch_light_d4.reset();
        }
        if(obj.handle){
            need(h.unregister_object,"0040c9a0")(obj.handle);
            need(h.release,"003cb230")(obj.handle,torch_release_delay);
            obj=TorchFlickerObject{};
        }
        return changed;
    }
    if(!inv.torch_light_d4){                                             // 00144370
        auto& p=pool(h).point;
        for(std::size_t i=0;i<p.size();++i){                             // 00373e70(00535d20, 4)
            if(p[i].id!=-1)continue;
            p[i].id=static_cast<std::int32_t>(i+1);
            inv.torch_light_d4=i;
            break;
        }
        if(inv.torch_light_d4){
            auto& l=p[*inv.torch_light_d4];
            l.vtable=level_light_vtable_point;
            l.intensity=0.0F;l.enabled=false;l.aux=3;l.type=4;
            l.colour={0.0F,0.0F,0.0F,0.0F};l.position={0.0F,0.0F,0.0F,1.0F};
            l.weight=0.0F;
            changed=true;
            l.inner=0.0F;                                                // 00144420
            inv.torch_target_dc=1.0F;inv.torch_rate_e4=10000.0F;inv.torch_level_e0=1.0F;
            if(!l.enabled){l.enabled=true;changed=true;}                 // vcall +2c: 00144b80
        }
    }
    if(!obj.handle){                                                     // 00144450
        const auto effect=need(h.effect_id,"*00236f20")(torch_effect_index);
        const auto o=need(h.create_object,"the flicker object (0036b180 / 003cf0e0)")(effect);
        if(!o)fail("a null flicker object (00144530 calls its vcall +90)");
        obj=TorchFlickerObject{o,false,false,effect};
        need(h.register_object,"0040c9d0")(o);
    }
    const RootVector position=need(h.attach_point,"00385330")();
    if(inv.torch_light_d4){                                              // 0014456c
        auto& l=entry(h,*inv.torch_light_d4);
        const float level=add(0.5F,mul(inv.torch_level_e0,sub(1.0F,0.5F)));
        l.colour={mul(0.7F,level),mul(0.6F,level),mul(0.2F,level),0.0F};
        const float sum=add(add(mul(l.colour[0],l.colour[0]),mul(l.colour[1],l.colour[1])),mul(l.colour[2],l.colour[2]));
        l.weight=fdiv(1.0F,sum);
        bool moved=false;                                                // vcall +28: 00144b10 (vsub.xyzw, MAC zero flags)
        for(std::size_t i=0;i<4;++i){
            if(!std::isfinite(position[i])||!std::isfinite(l.position[i]))fail("light position not finite");
            const float d=l.position[i]-position[i];
            if(std::fpclassify(d)==FP_SUBNORMAL)fail("denormal position difference (VU zero flag not established)");
            if(d==0.0F&&std::signbit(d))fail("-0 position difference (VU sign flag not established)");
            if(d!=0.0F)moved=true;
        }
        if(moved){l.position=position;changed=true;}
        l.outer=torch_light_range;
        inv.torch_level_e0=add(inv.torch_level_e0,mul(inv.torch_rate_e4,dt));
        const bool rising=!(inv.torch_rate_e4<=0.0F);
        const bool above=!(inv.torch_level_e0<=inv.torch_target_dc);
        if(rising==above){                                               // the target crossed
            inv.torch_level_e0=inv.torch_target_dc;
            inv.torch_target_dc=mul(1.0F,unit(h));
            const float span=sub(10.0F,2.0F);
            inv.torch_rate_e4=add(2.0F,mul(span,unit(h)));
            if(inv.torch_target_dc<inv.torch_level_e0)inv.torch_rate_e4=mul(inv.torch_rate_e4,-1.0F);
        }
    }
    if(obj.handle){                                                      // 00144730
        const bool actor=need(h.actor_shown,"the actor's +2d / +2c / +2e")();
        if(actor&&!obj.shown_2d){                                        // vcall +5c: 00394270
            if(obj.effect_60!=-1&&!obj.emitter_64){
                need(h.create_emitter,"00394270's emitter (003a2dd0)")(obj.handle,obj.effect_60);
                obj.emitter_64=true;
            }
            obj.shown_2d=true;                                           // 003ce980
        }else if(!actor&&obj.shown_2d){                                  // vcall +60: 003941a0
            if(obj.emitter_64){
                need(h.destroy_emitter,"003941a0's emitter release (003a2ba0)")(obj.handle);
                obj.emitter_64=false;
            }
            obj.shown_2d=false;                                          // 003ce790
        }
        if(obj.emitter_64){
            // Without a light yet (full pool) +e0 is still the heap content (weapon_inventory.hpp).
            need(h.emitter_scale,"003a2680")(obj.handle,add(0.8F,mul(inv.torch_level_e0,sub(1.3F,0.8F))));
        }
        need(h.set_position,"vcall +9c (003cdd80)")(obj.handle,position);
    }
    return changed;
}

}
