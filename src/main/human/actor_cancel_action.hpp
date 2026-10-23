#pragma once
// The object-action cancel. In actor mode 0x14 (the object action
// PerformAction_WithObject, 0018d2c0) a Triangle edge of the input helper
// 0024a1c0 sends the actor action 0x28 (vtable 004d9320, kind +28 = 1, +20 =
// 1, +2c = the byte 0048aec8: 0 in the boot snapshot, no writer found) with
// transport 1 after its 0x2c / 0x2d and swallows the press.
//
// 001a92a0 action 0x28 (001ad0e8): only in modes 0x11..0x14. The message's
// value is byte +2c for kind 1, (word +2c != 0) for kind 2 and 1 for any other
// kind; with a zero value it also needs +324c bit 0 (set by the object action
// for kind 7 with byte +38, the timed action that shows "Cancel"). Then it
// sends action 0x29 to the actor (vtable 004e0840, kind 0, +20 = 5, +1c =
// 0x40, +2c not written) with transport 0. Nothing on the actor changes.
//
// 001a92a0 action 0x29 (001ad1d0): nothing when the actor's virtual +108 (the
// life state, low byte) is 2 or 3. Otherwise the retained CoreMotions root
// (+326c, 0018a420 tag 2) replaces the current motion through the override
// +2ef4 (0018a380(*0049d038 = 0.5) + 001adff0; byte +5 cleared before
// 003ad500 when the root resolves), limit -1, phase 0, empty events, no tokens
// and no phase hooks. The message's kind, value, mode and +324c are not read;
// the priority actions and +324c are not changed (EE goldens).
#include "actor_motion.hpp"
#include <cstdint>
#include <functional>

namespace omega {
inline constexpr std::int32_t actor_cancel_request_action=0x28;
inline constexpr std::int32_t actor_cancel_action=0x29;
// The Triangle sender's +2c (0024a2f4: lbu 0048aec8).
inline constexpr std::uint8_t actor_cancel_request_byte_0048aec8=0;
struct ActorCancelRequest {
    std::uint8_t kind_28{1};
    std::uint32_t word_2c{actor_cancel_request_byte_0048aec8};
};
// 001ad0e8: true when action 0x29 is sent (transport 0).
bool actor_cancel_request_admitted(std::int32_t mode_320c,std::uint32_t flags_324c,const ActorCancelRequest&);

struct ActorCancelHost {
    std::function<ActorMotionRegistration()> retained;                 // +326c via 0018a420 tag 2
    std::function<void(const MotionRequest&,MotionInstallMode)> install; // 003ad500 on +2ef4
};
struct ActorCancelResult {
    bool life_gated{};   // virtual +108 returned 2 or 3
    bool installed{};    // the retained root replaced the motion
};
// 001ad1d0.
ActorCancelResult handle_actor_cancel(std::uint8_t life_state_108,ActorMotionTransitionState& transition_2ef4,
    const ActorCancelHost&);
}
