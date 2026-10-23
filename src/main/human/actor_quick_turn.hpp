#pragma once
// Actor message 001a92a0 action 14 (0x001aac10): the quick turn sent by the
// pad helper 00249d00. In actor mode 0 it installs a turn motion and queues
// the actor's retained motion (+326c, the CoreMotions node), then enables
// priority action 3 so +320c becomes 3. The turn motion carries embedded
// message code 0 to the actor; its finish or cancel (actions 4/5, receiver
// 001a7f40) clears action 3 again.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include <functional>
#include <string>
#include <string_view>

namespace omega {
struct ActorQuickTurnHost {
    // 001add90: 0018a670 name lookup (ID 0 on a miss) and 0018a660 node
    // pointer, with the node's own selector; the node may be absent.
    std::function<ActorMotionRegistration(std::string_view)> resolve;
    // Actor +326c: node and selector saved by 001ae7d0 for CoreMotions.
    std::function<ActorMotionRegistration()> retained;
    // 003ad500 -> 003ad680 on actor +2ef4.
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
struct ActorQuickTurnResult {
    bool handled{};          // false: actor mode +320c was not 0
    std::string motion;      // dataStandTurn180 or dataCrouchTurn180
    bool turn_installed{};   // the name resolved to a node
};
// flags_3204 is actor word +3204 (001ae080 is called with actor+0x3200 and
// reads the word at +4): bit 4 selects the crouch turn with 0.2 s
// transitions, bit 3 the crouch turn with a 0.4 s return, otherwise the
// standing turn with 0.2 s transitions.
ActorQuickTurnResult handle_actor_quick_turn(ActorAnimationReceiverState& actions,
    std::uint32_t flags_3204,std::uint64_t receiver,const ActorQuickTurnHost& host);
}
