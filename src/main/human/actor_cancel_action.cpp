#include "actor_cancel_action.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
bool actor_cancel_request_admitted(std::int32_t mode_320c,std::uint32_t flags_324c,const ActorCancelRequest& m){
    if(mode_320c!=0x14&&mode_320c!=0x11&&mode_320c!=0x12&&mode_320c!=0x13)return false;   // 001ad0e8..001ad10c
    bool value=true;                                                                        // daddiu a0, 1
    if(m.kind_28==1)value=(m.word_2c&0xffU)!=0;                                             // lbu +2c
    else if(m.kind_28==2)value=m.word_2c!=0;                                                // sltu +2c
    return value||(flags_324c&1U)!=0;                                                       // 001ad150
}

ActorCancelResult handle_actor_cancel(std::uint8_t life_state_108,ActorMotionTransitionState& transition_2ef4,
    const ActorCancelHost& host){
    ActorCancelResult result;
    if(life_state_108==3||life_state_108==2){result.life_gated=true;return result;}        // 001ad1d0..001ad200
    if(!host.retained||!host.install)throw std::runtime_error("Actor cancel action 001ad1d0 host is incomplete");
    // 0018a380(*0049d038) + 001adff0: the override +2ef8..+2f08 with +2ef9 = 1.
    set_actor_motion_transition(transition_2ef4,actor_motion_transition(std::bit_cast<float>(0x3f000000U)));
    const auto r=host.retained();                                                            // 0018a420 tag 2
    if(!r.node)return result;                                                               // 001ad2a4
    const auto used=transition_2ef4.value;
    transition_2ef4.pending=false;                                                          // 001ad2ec
    MotionRequest q;q.node=*r.node;q.selector=r.selector;
    q.width=used.width;q.bias=used.bias;q.rate=used.rate;q.secondary_rate=used.secondary_rate;
    q.limit=-1;q.initial_phase=0;                                                           // +98 = -1, +9c = 0
    host.install(q,MotionInstallMode::replace);                                             // request +48 = 1
    result.installed=true;
    return result;
}
}
