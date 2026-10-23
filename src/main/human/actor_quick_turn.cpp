#include "actor_quick_turn.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
ActorQuickTurnResult handle_actor_quick_turn(ActorAnimationReceiverState& actions,
    std::uint32_t flags_3204,std::uint64_t receiver,const ActorQuickTurnHost& host) {
    ActorQuickTurnResult result;
    if(actions.highest_action!=0)return result;
    if(!host.resolve||!host.retained||!host.install||!receiver)
        throw std::runtime_error("Actor quick-turn host is incomplete");
    // 001ae080(+3200, 4) then (+3200, 3) read +3204 bits 4 and 3; names at 0049e500/0049e520.
    const bool bit4=(flags_3204>>4)&1U,bit3=(flags_3204>>3)&1U;
    result.motion=bit4||bit3?"dataCrouchTurn180":"dataStandTurn180";
    const auto turn_transition=actor_motion_transition(std::bit_cast<float>(0x3e4ccccdU));
    const auto return_transition=actor_motion_transition(std::bit_cast<float>(!bit4&&bit3?0x3ecccccdU:0x3e4ccccdU));
    // Embedded event 0018a300 with code 0 and receiver = this actor (flags 5).
    MotionEmbeddedMessage completion;completion.id=0;completion.receiver=receiver;
    const auto request=[](const ActorMotionRegistration& r,const ActorMotionTransition& t){
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;
        q.limit=-1;q.initial_phase=0;   // 003ad500 f12 = -1, f13 = 0
        return q;
    };
    const auto turn=host.resolve(result.motion);
    result.turn_installed=turn.node.has_value();
    if(result.turn_installed) {
        auto q=request(turn,turn_transition);q.messages[0]=completion;
        host.install(q,MotionInstallMode::replace);
    }
    // The retained motion follows: queued after the turn, or installed in its
    // place (carrying the completion message) when the name did not resolve.
    const auto retained=host.retained();
    if(!retained.node)throw std::runtime_error("Actor retained motion +326c is unresolved");
    auto q=request(retained,result.turn_installed?return_transition:turn_transition);
    if(!result.turn_installed)q.messages[0]=completion;
    host.install(q,result.turn_installed?MotionInstallMode::queue:MotionInstallMode::replace);
    set_actor_action(actions,3,true);
    result.handled=true;
    return result;
}
}
