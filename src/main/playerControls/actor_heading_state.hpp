#pragma once
#include "actor_animation.hpp"
#include "animation_rotation_curve.hpp"

namespace omega {
// The heading stack +3088 (eight-byte entries: byte +0 active, angle +4) and
// its retained scalar curve +3030. `active` holds the entry bytes (empty when
// a caller tracks angles only); `selected` is +3098, the index the
// constructor 001b6be0 stores (001b6be8) from its first push 001b7940 (count
// - 1 = 0, byte 1). Nothing else in the image stores +3098. No initial stack
// entry or actor damping is invented here.
struct ActorHeadingState {
    std::vector<float> history;
    AnimationRotationCurveState curve;
    std::vector<std::uint8_t> active;
    std::size_t selected{};
};
// Handles history/target commands and returns true. Other commands return false
// without inspecting state, for the world-placement/latch host to apply in order.
// Target requests read the live final history value, as00186b50 does; the
// producer's command.value is a snapshot annotation, not an independent target.
// Each admitted command commits atomically, not an entire host command sequence.
bool apply_actor_heading_state_command(ActorHeadingState&,const ActorHeadingCommand&);
// 00247410(input, actor, byte, yaw), called by the pad 0024b5d0 at 0x0024eb78
// with byte sp+1b0 (1 only in the aim view, 0024e258) and f21. entry_20 is
// input +20 (constructor 002504e0: -1). With in58(x) = x is 5 or 8:
//  - entering (!in58(previous_30) && in58(current_38)) with byte set and
//    entry_20 == -1: push {1, yaw} (002477e0 / 002477c0), 00186b50 with
//    duration 0x3ea8f5c3 (selector 8) or 1.0 (selector 5), entry_20 = count - 1;
//  - leaving (in58(previous) && !in58(current)) with entry_20 != -1: 00247720
//    clears entry[entry_20]'s byte and resizes to one past the highest entry
//    with a nonzero byte (00250e70), then 00186b50(-1), entry_20 = -1;
//  - otherwise, with in58(current), byte set and entry_20 != -1:
//    entry[entry_20].angle = yaw (00186be0), then 00186b50(-1).
// 00186b50 targets the curve at the last entry's angle; an empty stack (which
// the original would read below its array) throws. Requires active.size() ==
// history.size().
void update_actor_aim_heading(ActorHeadingState&,std::int32_t& entry_20,std::int32_t previous_30,
    std::int32_t current_38,bool byte,float yaw);
// The late actor curve stage receives state.curve by reference. Its new current
// value is read by the next heading producer; this helper never publishes a root
// quaternion or advances time during a history/target command.
}
