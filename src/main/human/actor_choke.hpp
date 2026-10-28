#pragma once
// cHuman::Choke_NextFrame 00199980. Action 6 sets +3539. The next actor
// logic updates +353c while that request or the alternating +353a phase is
// active. At +3540 it toggles the phase: the on phase sets action 0x19 and
// installs tased_loop, while the off phase clears action 0x19 and queues the
// retained motion. Each phase lasts a random 1.5..3 seconds.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace omega {
struct ActorChokeState {
    std::uint8_t requested_3539{};
    std::uint8_t phase_353a{};
    float elapsed_353c{};
    float duration_3540{};
};
struct ActorChokeHost {
    std::function<std::uint32_t()> random;                                   // 00127dd0
    std::function<ActorMotionRegistration(std::string_view)> resolve;        // 0018c5a0
    std::function<ActorMotionRegistration()> retained;                       // +326c
    std::function<void(const MotionRequest&,MotionInstallMode)> install;     // 003ad500
};
struct ActorChokeResult {
    bool toggled{},started{},stopped{};
    std::string motion;
};
ActorChokeResult update_actor_choke(ActorAnimationReceiverState& actions,ActorChokeState& state,
    float elapsed,const ActorChokeHost& host);
}
