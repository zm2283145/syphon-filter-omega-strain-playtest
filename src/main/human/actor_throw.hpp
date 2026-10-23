#pragma once
// The actor side of the throw (research/COMBAT_ITEMS.md, C6b): the throw start
// for hold class 1 (00197560) and hold class 7 (00197140), the item triggers of
// the actor update 00197e00 (from 00198278: the throw release at phase 0.515,
// the IR switch and the melee hit), and the pad's throw power and range
// (0025d430 / 0025d470). Interpreter-verified (c6b/gen_throw.py). The release
// launches through the inventory launch 00141e20 (thrown_projectile.hpp); the
// one-shot's end (embedded event code 2, 001a7f40) runs the release helper
// 00141cd0 in actor_animation_receiver.
//
// The actor fields live in ActorAnimationReceiverState: +3210 (active_actions,
// bit 24 = action 24 armed by tokens 0x9e / 0x9f), +324c (flags_324c, bit 9),
// +14 (byte_14), +33cc (target_33cc, a named-object word: the object's +c, or
// the "none" word *004bdd00), +33d0 (aim_33d0), +33e0 (launch_33e0), +33e4
// (byte_33e4: 1 throwing, 2 launched, 0 released), +33e5 (IR switch state)
// and +33e8 (melee state).
#include "actor_animation_receiver.hpp"
#include "secondary_component.hpp"
#include "secondary_playback.hpp"
#include <cstdint>
#include <functional>

namespace omega {

// 0025d430(t): 0.1 + min(t, 1) x (0.7 - 0.1) (a NaN takes 1).
float throw_power(float hold_time);
// 0025d470(t): 5 + min(t, 1) x (100 - 5).
float throw_range(float hold_time);

struct ActorThrowStart {
    bool requested{};       // 001cae20 was called (the component exists, action 24 does not refuse)
    bool accepted{};        // 001cae20 accepted the one-shot (state 0x16 requested)
    bool star_message{};    // 00197140, local actor: message 004eeed0 (vtable 004da8d0, +24 the name word,
                            // +30 the point, +20 = 4) sent to the actor itself with transport 0
};
// 00197560(actor, &request name, &point, arc) in single player: refused while
// action 24 is active (a local actor always; a replica while +324c bit 9);
// without the component (nullptr) nothing. The one-shot request:
// "throw_grenade_forward_stand", kind 3, the embedded event (descriptor
// 00542b60) code 2, receiver `receiver`. Accepted: +33cc = the name word, +33d0
// = the point, +33e0 = arc, +33e4 = 1 and the inventory's +ef = 1.
ActorThrowStart start_actor_grenade_throw(ActorAnimationReceiverState& actor,SecondaryComponent* secondary,
    std::uint8_t& inventory_busy_ef,std::uint64_t receiver,std::uint32_t name_word,const RootVector& point,float arc);
// 00197140(actor, &request name, &point): "throw_chinese_star", no action-24
// test and no arc; a local actor (+14) then sends the star message.
ActorThrowStart start_actor_star_throw(ActorAnimationReceiverState& actor,SecondaryComponent* secondary,
    std::uint8_t& inventory_busy_ef,std::uint64_t receiver,std::uint32_t name_word,const RootVector& point);

// The calls 00197e00's item triggers make, in its order. Each one is required
// only when the original would call it.
struct ActorItemTriggerHost {
    // 001cab60(+3314, name).
    std::function<float(const SecondaryMotionName&)> phase;
    // 00141e20(+3524, arc, &+33cc, &+33d0): reads the actor's +33cc / +33d0.
    std::function<void(float arc,ActorAnimationReceiverState&)> launch;
    // The star sound: 001ca210 ("G_THROWING_STAR", bank 1), 0020f990 at the
    // actor, sent to the sound manager 004ffb50 with transport 0 (00198460).
    std::function<void()> star_sound;
    // 00199590(actor, +33e6).
    std::function<void(std::uint8_t argument)> switch_on_ir;
    // 0040c540(&+33cc) and, when it resolves, 00141880(+3524, target, 0).
    std::function<void(std::uint32_t name_word)> melee_hit;
    std::uint32_t none_word{};                    // *004bdd00
};
struct ActorItemTriggers {
    bool launched{},star{},ir{},melee{};
};
// 00197e00 from 00198278 (the gait / footstep part before it is not native).
// `secondary` is whether +3314 exists; `ir_argument` is +33e6.
ActorItemTriggers run_actor_item_triggers(ActorAnimationReceiverState& actor,bool secondary,std::uint8_t ir_argument,
    const ActorItemTriggerHost& host);

}
