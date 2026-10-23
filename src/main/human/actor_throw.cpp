#include "actor_throw.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Actor throw: "+what);}
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)fail(std::string("host ")+what+" required");
    return f;
}
constexpr float release_phase=std::bit_cast<float>(0x3f03d70aU);   // 0048a250 (0.515)
constexpr float star_arc=std::bit_cast<float>(0x3d23d70aU);        // 0048a280 (0.04)
constexpr float ir_phase=std::bit_cast<float>(0x3ecccccdU);        // 0048a258 (0.4)
constexpr float knife_phase=std::bit_cast<float>(0x3e4ccccdU);     // 0048a260 (0.2)
constexpr float baton_phase=std::bit_cast<float>(0x3e4ccccdU);     // 0048a270 (0.2)
float clamp_hold(float t){return t<=1.0F?t:1.0F;}                  // c.ole.s: NaN takes 1
ActorThrowStart start(ActorAnimationReceiverState& a,SecondaryComponent* sec,std::uint8_t& busy,std::uint64_t receiver,
    std::uint32_t name_word,const RootVector& point,const char* motion){
    ActorThrowStart out;
    if(!sec)return out;                                            // +3314
    out.requested=true;
    SecondaryOneShot event;                                        // 0018a300 on descriptor 00542b60
    event.code=2;event.action=0;event.receiver=receiver;event.posted=0;event.value=0.0F;
    out.accepted=request_secondary_one_shot(*sec,secondary_motion_name(motion),3,event);   // 001cae20(+3314, name, 3, &event)
    if(!out.accepted)return out;
    a.target_33cc=name_word;a.aim_33d0=point;a.byte_33e4=1;busy=1;
    return out;
}
}

float throw_power(float t){
    constexpr float low=std::bit_cast<float>(0x3dcccccdU),high=std::bit_cast<float>(0x3f333333U);   // 0048af70 / 0048af78
    return check(low+check(clamp_hold(t)*check(high-low)));
}
float throw_range(float t){
    constexpr float low=5.0F,high=100.0F;                          // 0048af60 / 0048af68
    return check(low+check(clamp_hold(t)*check(high-low)));
}

ActorThrowStart start_actor_grenade_throw(ActorAnimationReceiverState& a,SecondaryComponent* sec,std::uint8_t& busy,
    std::uint64_t receiver,std::uint32_t name_word,const RootVector& point,float arc){
    if(a.active_actions&(1U<<24)){                                 // action 24 (tokens 0x9e / 0x9f)
        if(a.byte_14)return {};
        if(a.flags_324c&0x200U)return {};
    }
    auto out=start(a,sec,busy,receiver,name_word,point,"throw_grenade_forward_stand");
    if(out.accepted)a.launch_33e0=arc;
    return out;
}

ActorThrowStart start_actor_star_throw(ActorAnimationReceiverState& a,SecondaryComponent* sec,std::uint8_t& busy,
    std::uint64_t receiver,std::uint32_t name_word,const RootVector& point){
    auto out=start(a,sec,busy,receiver,name_word,point,"throw_chinese_star");
    if(out.accepted&&a.byte_14)out.star_message=true;              // 0019726c
    return out;
}

ActorItemTriggers run_actor_item_triggers(ActorAnimationReceiverState& a,bool secondary,std::uint8_t ir_argument,
    const ActorItemTriggerHost& h){
    ActorItemTriggers out;
    if(!secondary)return out;
    const auto phase=[&](const char* name){return need(h.phase,"001cab60")(secondary_motion_name(name));};
    if(a.byte_33e4==1){                                            // 00198284
        const float p=phase("throw_grenade_forward_stand");
        if(p!=-1.0F){
            if(p>release_phase){
                need(h.launch,"00141e20")(a.launch_33e0,a);
                a.target_33cc=h.none_word;a.byte_33e4=2;out.launched=true;
            }
        }else{
            const float q=phase("throw_chinese_star");
            if(q!=-1.0F&&q>release_phase){
                need(h.launch,"00141e20")(star_arc,a);
                a.target_33cc=h.none_word;a.byte_33e4=2;
                need(h.star_sound,"the star sound (001ca210 / 0020f990)")();
                out.launched=out.star=true;
            }
        }
    }
    if(a.byte_33e5==1){                                            // 001984bc
        const float p=phase("switch_on_ir");
        if(p!=-1.0F&&p>ir_phase){
            need(h.switch_on_ir,"00199590")(ir_argument);
            a.byte_33e5=2;out.ir=true;
        }
    }
    if(a.byte_33e8==1){                                            // 00198584
        const float p=phase("knife_stabbing");
        bool hit=false;
        if(p!=-1.0F)hit=p>knife_phase;
        else{
            const float q=phase("stun_baton");
            hit=q!=-1.0F&&q>baton_phase;
        }
        if(hit){
            need(h.melee_hit,"0040c540 / 00141880 (melee, C5)")(a.target_33cc);
            a.byte_33e8=2;out.melee=true;
        }
    }
    return out;
}

}
