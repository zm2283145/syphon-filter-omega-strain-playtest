#pragma once
// The actor ignition 00186360(actor, on a1, source a2, a3, t0, f12): sets or
// clears the burning byte +326a and switches the fire effects. See
// research/COMBAT.md "Checkpoint 120b".
//
// +326a = 0 when the fire immunity +33b8 or the invulnerable byte +33a4 is
// set, else the low byte of a1. Then, with b = the old +326a:
//   b == 0 and +326a != 0 (ignite): 00185d40(actor, 0, 1, 0.4) and
//     00185d40(actor, 1, 1, 0.4) (the fire and its smoke); +31f4 = f12,
//     +3480 = a2, +3484 = a2 +c (-1 for a null a2), +3488 = a3's low byte;
//     the sound 001ca210(desc, 1, 0049d8e0 "G_TASER_BODY", 0, 0) + 0020f990
//     (+20 = 1) sent to 004ffb50 with transport 0.
//   +326a == 0 (whatever b; also for an actor that never burned): 00185d40
//     (actor, 0, 0, 1.0), 00185d40(actor, 1, 0, 1.0) and the sound stop
//     (0020eba0, +20 = 1) of the same name to 004ffb50, transport 0.
//   b != 0 and +326a != 0: nothing.
// Then, with the multiplayer byte *005721c8 set and t0 (the whole register)
// 0: the replication message 003c9e30(0055d480) with vtable 004e0180, +1c
// 0x40, +20 4, +24 0x44, +28 6, +2c the a2 key (-1 for a null a2), +30 +326a,
// sent to the actor with transport 0.
#include <cstdint>
#include <functional>
#include <string_view>

namespace omega {

inline constexpr std::string_view actor_fire_sound="G_TASER_BODY";      // 0049d8e0
inline constexpr float actor_ignite_effect_f12=0.4F;                    // 0x3ecccccd
inline constexpr float actor_extinguish_effect_f12=1.0F;

struct ActorIgnitionState {
    std::uint8_t burning_326a{};
    std::uint8_t invulnerable_33a4{};
    std::uint8_t fire_immune_33b8{};
    float f31f4{-1.0F};
    std::uint64_t source_3480{};                                          // the source object (a2)
    std::uint32_t source_key_3484{0xfffffffeU};
    std::uint8_t byte_3488{};
};

struct ActorIgnitionHost {
    // 00185d40(actor, kind, on, f12); it sees the new +326a (not +3480..+3488).
    std::function<void(std::uint32_t kind,std::uint32_t on,float f12)> effect;
    std::function<std::uint32_t(std::uint64_t source)> key_of;            // a2 +c
    std::function<void(std::string_view name)> start_sound;               // 001ca210 + 0020f990 to 004ffb50
    std::function<void(std::string_view name)> stop_sound;                // 001ca210 + 0020eba0 to 004ffb50
    std::function<bool()> networked;                                      // *005721c8
    std::function<void(std::uint32_t key,std::uint32_t burning)> replicate;   // 004e0180 to the actor
};

// 00186360. a1 / a3 / t0 are the whole registers; source is a2 (0 or an object).
void ignite_actor(ActorIgnitionState& state,std::uint64_t on,std::uint64_t source,std::uint64_t a3,std::uint64_t t0,
    float f12,const ActorIgnitionHost& host);

}  // namespace omega
