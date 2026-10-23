#include "actor_stunned_action.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
namespace {
MotionRequest stunned_request(const ActorMotionRegistration& r,const ActorMotionTransition& t) {
    MotionRequest q;q.node=*r.node;q.selector=r.selector;
    q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;
    q.limit=-1;q.initial_phase=0;   // 003ad500 f12 = -1, f13 = 0
    return q;                       // events 0018a300 with code 0 / no receiver, token byte 0 = 0
}
}
ActorStunnedActionResult handle_actor_stunned_action(ActorAnimationReceiverState& actions,std::uint32_t code_2c,
    const ActorStunnedActionHost& host) {
    ActorStunnedActionResult result;
    if(actions.active_actions&((1U<<27)|(1U<<31)))return result;   // both codes
    if(code_2c!=0) {
        if(!host.resolve||!host.install)throw std::runtime_error("Actor stunned action 001a9bc8 host is incomplete");
        set_actor_action(actions,actor_stunned_action_index,true);
        result.handled=true;
        const auto node=host.resolve(actor_stunned_loop_motion);
        if(!node.node)return result;
        host.install(stunned_request(node,actor_motion_transition(std::bit_cast<float>(0x3dcccccdU))),
            MotionInstallMode::replace);
        result.loop_installed=true;
        return result;
    }
    if(actions.highest_action!=static_cast<std::int32_t>(actor_stunned_action_index))return result;
    if(!host.retained||!host.install)throw std::runtime_error("Actor stunned action 001a9bc8 host is incomplete");
    set_actor_action(actions,actor_stunned_action_index,false);
    result.handled=true;
    const auto retained=host.retained();
    if(!retained.node)throw std::runtime_error("Actor stunned action 001a9bc8: retained motion +326c is unresolved");
    host.install(stunned_request(retained,actor_motion_transition(std::bit_cast<float>(0x3f000000U))),
        MotionInstallMode::queue);
    result.retained_queued=true;
    return result;
}
}
