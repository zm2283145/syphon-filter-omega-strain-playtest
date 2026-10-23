#pragma once
// Actor message 001a92a0 action 7 (001a9bc8): the stunned action the stun
// 00184ce0 delivers immediately (transport 1) with code 1 when the stun
// starts and code 0 when its countdown ends. Message +28 is ignored. Nothing
// happens (either code) while actor +3210 (the active priority actions) has
// bit 27 or bit 31 set.
//
// Code != 0: priority action 0x1a is enabled, the name
// "tased_loop" is resolved (0018c5a0 on the model +2ec0) and, when it
// resolves, installed in place of the current motion (003ad500 mode 1) with
// 0.1 s transitions, the node's own selector, empty events and no tokens.
//
// Code 0: nothing unless +320c (the highest action) is 0x1a. Otherwise action
// 0x1a is disabled and the retained motion +326c (001ae270 / 001ae260) is
// queued (003ad500 mode 0) with transitions of *0049d038 (0.5 s, read from
// the boot snapshot), empty events and no tokens.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include <functional>
#include <string_view>

namespace omega {
inline constexpr unsigned actor_stunned_action_index=0x1a;
inline constexpr std::string_view actor_stunned_loop_motion="tased_loop";
struct ActorStunnedActionHost {
    // 0018c5a0 + 0018c620: the node (absent on a miss) and its selector.
    std::function<ActorMotionRegistration(std::string_view)> resolve;
    // Actor +326c via 001ae270 / 001ae260.
    std::function<ActorMotionRegistration()> retained;
    // 003ad500 on actor +2ef4.
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
struct ActorStunnedActionResult {
    bool handled{};          // the action changed (code != 0 admitted, or code 0 at highest 0x1a)
    bool loop_installed{};   // code != 0: tased_loop resolved and installed
    bool retained_queued{};  // code 0: the retained motion was queued
};
ActorStunnedActionResult handle_actor_stunned_action(ActorAnimationReceiverState& actions,std::uint32_t code_2c,
    const ActorStunnedActionHost& host);
}
