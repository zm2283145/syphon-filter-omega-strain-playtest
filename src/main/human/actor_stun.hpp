#pragma once
// The actor stun 00184ce0(actor, lethal, notify, duration, f13) (stun baton,
// hand / air taser, knockout gas) and its countdown in the actor logic
// 001b1710 (001b23c8..001b255c, after the +33b0 decay and before the +352c
// decay). See research/COMBAT.md "Checkpoint 120".
//
// State: +33bc the elapsed time (< 0: not stunned; the spawn reset 001b61d8
// writes it), +33c0 the duration, +33c8 the lethal byte.
//
// Start, idle (+33bc < 0; c.olt.s, so a NaN counts as stunned):
//   notify != 0: the message 004ef078 (vtable 004da9f0, +24 the lethal byte,
//     +28 the duration, +2c f13) to the actor with transport 0 (network: its
//     receiver 00184f90 replays 00184ce0 with notify 0);
//   00185d40(actor, 2, 1, 1.0) (the shock effect slot 2);
//   0020f990(request, 004f12d0 "G_TASER_BUZZ", actor), +20 = 1, sent to
//     004ffb50 with transport 0;
//   the action message 0055d480 (vtable 004e01e0, +20 = 1, +24 = 7, +28 = 2,
//     +2c = *0048a308 = 1) to the actor with transport 1;
//   then +33bc = 0, +33c0 = duration, +33c8 = (byte) lethal.
// Start, stunned: +33c0 = duration unless duration <= +33c0; lethal != 0
//   (the whole register) sets +33c8 = 1.
// Countdown (stunned: !(+33bc < 0)): +33bc += dt; past the duration
// (!(+33bc <= +33c0)): +33bc = -1, 00185d40(actor, 2, 0, 1.0), the action
// message with +2c = *0048a230 = 0, the sound stop 0020eba0(request,
// 004f12d0, actor) (+20 = 1, 004ffb50, transport 0), then the actor's
// virtual +100(actor, actor, 1, type, 0, 0, 0, amount): +33c8 set: type 4,
// 10000.0; else type 7, 0.0.
#include <cstdint>
#include <functional>

namespace omega {

struct ActorStunState {
    float elapsed_33bc{-1.0f};
    float duration_33c0{};
    std::uint8_t lethal_33c8{};
    bool stunned() const{return !(elapsed_33bc<0.0f);}
};

inline constexpr std::uint32_t stun_replicate_key=0x004ef078;
inline constexpr std::uint32_t stun_replicate_vtable=0x004da9f0;
inline constexpr std::uint32_t stun_sound=0x004f12d0;          // 0020f990 descriptor "G_TASER_BUZZ" (+10)
inline constexpr std::uint32_t stun_action_key=0x0055d480;
inline constexpr std::uint32_t stun_action_vtable=0x004e01e0;
inline constexpr std::int32_t stun_action_24=7;
inline constexpr std::uint8_t stun_action_kind_28=2;
inline constexpr std::int32_t stun_start_code=1;               // *0048a308
inline constexpr std::int32_t stun_end_code=0;                 // *0048a230
inline constexpr std::uint32_t stun_effect_slot=2;             // 00185d40's kind
inline constexpr float stun_lethal_damage=10000.0f;            // 0x461c4000
inline constexpr float shock_to_death_duration=0.5f;           // ShockToDeath 00185280: 0x3f000000

struct ActorStunHost {
    // The message 004ef078 to the actor with transport 0.
    std::function<void(std::uint8_t lethal_24,float duration_28,float value_2c)> replicate;
    // 00185d40(actor, kind, on, f12).
    std::function<void(std::uint32_t kind,std::uint32_t on,float f12)> effect;
    // 0020f990 / 0020eba0(request, descriptor, actor), +20 = 1, to 004ffb50 with transport 0.
    std::function<void(std::uint32_t descriptor)> sound;
    std::function<void(std::uint32_t descriptor)> stop_sound;
    // 0055d480 (+20 = 1) to the actor with transport 1.
    std::function<void(std::int32_t action_24,std::uint8_t kind_28,std::int32_t code_2c)> action;
    // Actor virtual +100(actor, actor, category, type, 0, 0, 0, amount).
    std::function<void(std::uint8_t category,std::uint8_t type,float amount)> damage;
};

// 00184ce0. lethal / notify are the whole a1 / a2 registers.
void start_actor_stun(ActorStunState& state,std::uint64_t lethal,std::uint64_t notify,float duration,float f13,
    const ActorStunHost& host);
// 001b23c8..001b255c with dt (f20). Returns true when the stun ended.
bool update_actor_stun(ActorStunState& state,float dt,const ActorStunHost& host);

}  // namespace omega
