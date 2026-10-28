#include "actor_choke.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float checked(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor choke: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float phase_duration(const ActorChokeHost& host){
    if(!host.random)throw std::runtime_error("Actor choke: RNG host required");
    const auto draw=host.random();
    if(draw>0x7fffffffU)throw std::runtime_error("Actor choke: RNG value outside 0..2^31-1");
    const float fraction=checked(vu::div(game_int_to_float(static_cast<std::int32_t>(draw)),
        checked(1.0F+std::bit_cast<float>(0x4f000000U))));
    return checked(1.5F+checked(1.5F*fraction));                           // 00199a04..00199a54 / 00199cb0..00199d08
}
MotionRequest request(const ActorMotionRegistration& reg){
    if(!reg.node)throw std::runtime_error("Actor choke: required motion did not resolve");
    MotionRequest q;q.node=*reg.node;q.selector=reg.selector;
    constexpr float blend=0.5F;                                           // 0049d038 after game initialization
    q.width=blend;q.bias=blend;q.rate=2.0F;q.secondary_rate=2.0F;
    q.limit=-1.0F;q.initial_phase=0.0F;
    return q;
}
}

ActorChokeResult update_actor_choke(ActorAnimationReceiverState& actions,ActorChokeState& state,
    float elapsed,const ActorChokeHost& host){
    checked(elapsed);
    ActorChokeResult result;
    if(state.requested_3539||state.phase_353a)
        state.elapsed_353c=checked(state.elapsed_353c+elapsed);             // 001999a4..001999c8
    state.requested_3539=0;
    if(!(state.duration_3540<state.elapsed_353c))return result;             // c.ole.s elapsed, duration
    state.elapsed_353c=0.0F;
    state.phase_353a=state.phase_353a?0:1;
    state.duration_3540=phase_duration(host);
    result.toggled=true;
    if(state.phase_353a){
        if(actions.highest_action!=0)return result;
        if(!host.resolve||!host.install)throw std::runtime_error("Actor choke: tased_loop motion hosts required");
        set_actor_action(actions,0x19,true);
        host.install(request(host.resolve("tased_loop")),MotionInstallMode::replace);
        result.started=true;result.motion="tased_loop";
    }else{
        if(actions.highest_action!=0x19)return result;
        if(!host.retained||!host.install)throw std::runtime_error("Actor choke: retained motion hosts required");
        set_actor_action(actions,0x19,false);
        host.install(request(host.retained()),MotionInstallMode::queue);
        result.stopped=true;
    }
    return result;
}
}
