#pragma once
// The actor effect slots 00185d40(actor, kind a1, on a2, f12): a particle
// effect object per kind attached to the actor (the fire 0, kind 1, the shock
// 2 of the stun 00184ce0, kind 3), held as a weak handle +354c + 8k (object)
// / +3550 + 8k (its key, object +c; the spawn constructor writes 0 / -2). See
// research/COMBAT.md "Checkpoint 120b".
//
// k is a1 & 0xff and indexes the descriptor table 0048a2d8 and the count table
// 0048a2e8 (k 0: 0049d600 / 10, the "FireHuman" emitters; 1: 0049d730 / 1,
// "FireHumanSmoke"; 2: 0049d760 / 13; 3: 0 / 0). A handle is valid when the
// object is non-null and its +c equals the stored key.
//
// On (a2 != 0), unless the slot is valid:
//   0036b180(0, 0x88, "human.cc", 0x43cd), then 00236810(object, actor,
//   descriptor[k], count[k], f12) (the effect object, vtable 004dbd80);
//   slot = object, key = object +c (-1 for a null allocation);
//   k == 0: 0036b180(0, 0xa0, "human.cc", 0x43d1), 002624a0(light, actor,
//   f12 = *0048a1e0 = 5.0, f13 = *0048a1d8 = 10.0), +34e0 = light (the old
//   value is overwritten unread);
//   0040c9d0(the valid handle or 0) (the world-object list 00571748), then
//   the valid handle's virtual +5c(1) (00394270).
// Off (a2 == 0):
//   the valid handle: virtual +60(1) (003941a0), 0040c9a0(object), 003cb230
//   (object, 0.1), then slot = 0, key = -1 (a stale handle is left as is);
//   k == 0 (also with an empty slot): +326a = 0, 001ca210(descriptor, 1,
//   0049d8e0 "G_TASER_BODY", 0, 0) and 0020eba0(request, descriptor, actor)
//   (+20 = 1) sent to 004ffb50 with transport 0 (the sound stop); a non-null
//   +34e0: 0040c9a0, 003cb230(+34e0, 0.1), +34e0 = 0.
//
// The 0x10dc40 / 0x10dca8 debug counters around the allocations (only with
// *00533880) and the 00533888 depth counter have no lasting effect.
#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace omega {

struct ActorEffectSlot {
    std::uint32_t object_354c{};
    std::uint32_t key_3550{0xfffffffeU};     // the spawn constructor's -2
};

struct ActorEffectSlotState {
    std::array<ActorEffectSlot,4> slots{};
    std::uint32_t light_34e0{};
    bool burning_326a{};
};

inline constexpr std::array<std::uint32_t,4> actor_effect_descriptors{0x0049d600U,0x0049d730U,0x0049d760U,0};   // 0048a2d8
inline constexpr std::array<std::uint32_t,4> actor_effect_counts{10,1,13,0};                                     // 0048a2e8
inline constexpr float actor_effect_light_f12=5.0f;     // *0048a1e0
inline constexpr float actor_effect_light_f13=10.0f;    // *0048a1d8
inline constexpr float actor_effect_release_delay=0.1f; // 0x3dcccccd
inline constexpr std::string_view actor_effect_stop_sound="G_TASER_BODY";   // 0049d8e0

struct ActorEffectSlotHost {
    // The object's +c (the handle key).
    std::function<std::uint32_t(std::uint32_t object)> key_of;
    // 0036b180(0, 0x88, ...) + 00236810(object, actor, descriptor, count, f12): the new object.
    std::function<std::uint32_t(std::uint32_t descriptor,std::uint32_t count,float f12)> create_effect;
    // 0036b180(0, 0xa0, ...) + 002624a0(light, actor, f12, f13): the new light.
    std::function<std::uint32_t(float f12,float f13)> create_light;
    std::function<void(std::uint32_t object)> register_object;      // 0040c9d0
    std::function<void(std::uint32_t object)> unregister_object;    // 0040c9a0
    std::function<void(std::uint32_t object)> activate;             // virtual +5c(1)
    std::function<void(std::uint32_t object)> deactivate;           // virtual +60(1)
    std::function<void(std::uint32_t object,float delay)> release;  // 003cb230
    // 001ca210(descriptor, 1, name, 0, 0) + 0020eba0, +20 = 1, to 004ffb50 with transport 0.
    std::function<void(std::string_view name)> stop_sound;
};

bool actor_effect_slot_valid(const ActorEffectSlot& slot,const ActorEffectSlotHost& host);
// 00185d40. kind / on are the whole a1 / a2 registers.
void set_actor_effect(ActorEffectSlotState& state,std::uint64_t kind,std::uint64_t on,float f12,const ActorEffectSlotHost& host);

}  // namespace omega
