#pragma once
// Actor message 001a92a0 action 8 (0x001aa468): the directional roll the NPC
// RequestAction 0014d900 sends (TakeCover 001564c0, eAction 2). In actor mode
// 0, with both action-history tops clear and no crouched ceiling hit, it picks
// one of the roll roots +3270..+3284 from model channel 15 and the running
// test, installs it (replace) with the roll tokens and two phase hooks, and
// queues the retained CoreMotions root +326c behind it. Both installs go
// through the actor override +2ef4 (0.5 s, 0049d038). The dispatcher returns 1
// on every path; the action writes nothing else on the actor (EE goldens).
#include "actor_motion.hpp"
#include <cstdint>
#include <functional>

namespace omega {
struct ActorRollInput {
    std::int32_t mode_320c{};           // +320c
    std::int32_t top_3234{},top_3220{}; // the two action-history tops (+3234, +3220)
    std::uint32_t flags_3204{};         // +3204: 001ae0d0(actor, 4) and 001ae080(+3200, 3 / 4)
    bool physical{};                    // +3310 non-null
    std::uint8_t byte_3d2{};            // 001ae0b0(+3310, 0): physical byte +3d2
    float accumulation_309c{};          // +309c
    std::uint64_t receiver{};           // the actor (token / hook receiver)
};
struct ActorRollHost {
    // Actor +326c + 4 * class: the registrations 001ae7d0 saves (0
    // CoreMotions, 1 RunRollForw, 2 RunRollBackLeft, 3 RunRollBackRght,
    // 4 RunRollLeft, 5 RunRollRght, 6 RollBack); the node may be absent.
    std::function<ActorMotionRegistration(std::uint32_t roll_class)> registration;
    // 003ad500 with request +48: 1 replace, 0 queue.
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
    // 0019d7b0(+2ed0, 15): model channel 15 current, read only past the gates.
    std::function<float()> channel_15;
};
struct ActorRollResult {
    bool gated{};                       // a gate returned before any write
    std::uint32_t roll_class{};         // 1..6 when not gated
    bool roll_installed{},core_installed{};
};
// 001aa4f4..001aa564: !(+309c < 1) && !(+3204 bit 3) && !(+3204 bit 4).
// A nonfinite +309c throws (the EE FPU has no NaN or infinity).
bool actor_roll_running(float accumulation_309c,std::uint32_t flags_3204);
// 001aa568..001aa618: strict c.olt.s chain on channel 15 (0.125, 0.3, 0.7,
// 0.875). A nonfinite value throws.
std::uint32_t actor_roll_class(float channel_15,bool running);
ActorRollResult handle_actor_roll(const ActorRollInput& in,ActorMotionTransitionState& transition_2ef8,
    const ActorRollHost& host);
}
