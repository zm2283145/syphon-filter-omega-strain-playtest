#include "actor_roll_action.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float finite(float v,const char* what){
    if(!std::isfinite(v))throw std::runtime_error(std::string("Actor roll action: nonfinite ")+what);
    return v;
}
// 0049d038 after static initialization (verified in the boot snapshot).
const float crossfade=bits(0x3f000000U);
// 001aa708..001aa774: roll token block (0018a910 / 0018a8c0 / 001ae050) with ids at
// slots 0,1,3,5,6 = 1,2,5,6,7; slots 2 and 4 stay {-1, 0}.
constexpr std::array<std::int32_t,7> roll_tokens{1,2,-1,5,-1,6,7};
// 001ae020: max.s against 0, then min.s against 1.0; byte +8 = a1 (0 here).
float clamp_phase(float v){const float lo=v<0.0F?0.0F:v;return 1.0F<lo?1.0F:lo;}
MotionPhaseHook hook(std::uint32_t phase_bits,std::int32_t id,std::uint64_t receiver){
    const auto p=clamp_phase(bits(phase_bits));
    return {p,p,{id,receiver}};                           // hook byte +8 stays 0
}
}

bool actor_roll_running(float accumulation_309c,std::uint32_t flags_3204){
    finite(accumulation_309c,"accumulation +309c");
    // 001aa4f4..001aa510: c.olt.s +309c, 1.0 (lui 0x3f80); bc1t -> not running.
    if(accumulation_309c<1.0F)return false;
    // 001ae080(+3200, 3) / (+3200, 4): bits of the word at +3204.
    return (flags_3204&(1U<<3))==0&&(flags_3204&(1U<<4))==0;
}

std::uint32_t actor_roll_class(float channel_15,bool running){
    finite(channel_15,"model channel 15");
    if(channel_15<bits(0x3e000000U))return running?2U:6U;  // 0.125
    if(channel_15<bits(0x3e99999aU))return 4U;             // 0.3
    if(channel_15<bits(0x3f333333U))return 1U;             // 0.7
    if(channel_15<bits(0x3f600000U))return 5U;             // 0.875
    return running?3U:6U;
}

ActorRollResult handle_actor_roll(const ActorRollInput& in,ActorMotionTransitionState& transition_2ef8,
    const ActorRollHost& host){
    ActorRollResult result;
    // 001aa468: actor mode +320c must be 0.
    if(in.mode_320c!=0){result.gated=true;return result;}
    // 001aa474 / 001aa480: both action-history tops must be clear.
    if(in.top_3234!=0||in.top_3220!=0){result.gated=true;return result;}
    // 001aa4a0..001aa4e0: 001ae0d0(actor, 4) && +3310 && 001ae0b0(+3310, 0) (+3d2) refuses.
    if((in.flags_3204&(1U<<4))!=0&&in.physical&&in.byte_3d2!=0){result.gated=true;return result;}
    if(!host.registration||!host.install||!host.channel_15)throw std::runtime_error("Actor roll action host incomplete");
    // 001aa4ec: 0019d7b0(+2ed0, 15) -> channel 15 current (f22).
    const auto channel_15=host.channel_15();
    const auto running=actor_roll_running(in.accumulation_309c,in.flags_3204);
    result.roll_class=actor_roll_class(channel_15,running);
    // 001aa620 switch (0049e660): class 6 (RollBack, 001aa6e8) gets the late contact phase 0.425, the
    // forward / side rolls 0.325; both get token 4 at 0.55.
    const auto contact=result.roll_class==6?0x3ed9999aU:0x3ea66666U;
    const auto submit=[&](std::uint32_t roll_class,MotionInstallMode mode,bool tokens,
        std::vector<MotionPhaseHook> hooks)->bool{
        // 0018a380(0.5) + 001adff0 (001aa828 / 001aa834): override +2efc..+2f08, +2ef9 = 1.
        set_actor_motion_transition(transition_2ef8,actor_motion_transition(crossfade));
        // 0018a420 (001aa858) tag 2 on the registration object at +326c + 4 * class.
        const auto r=host.registration(roll_class);
        if(!r.node)return false;
        const auto used=transition_2ef8.value;
        transition_2ef8.pending=false;                    // cleared before 003ad500
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=used.width;q.bias=used.bias;q.rate=used.rate;q.secondary_rate=used.secondary_rate;
        q.limit=-1;q.initial_phase=0;                     // +98 = -1, +9c = 0
        if(tokens){
            q.notifications_active=true;
            for(std::size_t k=0;k<roll_tokens.size();++k)
                if(roll_tokens[k]>=0)q.notifications[k]={roll_tokens[k],in.receiver};
        }
        q.phase_hooks=std::move(hooks);
        host.install(q,mode);
        return true;
    };
    // Roll root (001aa7f4 +44 = +326c[class], 001aa808 +48 = 1 replace), tokens, hooks {contact, 3} then {0.55, 4}.
    result.roll_installed=submit(result.roll_class,MotionInstallMode::replace,true,
        {hook(contact,3,in.receiver),hook(0x3f0ccccdU,4,in.receiver)});
    // CoreMotions +326c[0] queued behind it (request +48 = 0), token block byte 0 = 0.
    result.core_installed=submit(0,MotionInstallMode::queue,false,{});
    return result;
}
}
