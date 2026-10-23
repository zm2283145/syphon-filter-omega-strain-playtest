#pragma once
// The torch (hold class 10) branch of the weapon update 00144080 (0014432c ..
// 00144908; research/COMBAT.md "Checkpoint 120b"): while the held item is of
// hold class 10 and the actor's virtual +108 (life state) is not 3, a flickering
// point light and a flicker effect object follow attach point 0 of the actor;
// otherwise both are released.
//
// Light (+d4 == none): 00373e70(00535d20, 4) takes a point-pool entry; the
// inlined constructor writes +0 = *00489c08, +1 = 0, vtable 004d9280 ->
// 004d9240 -> 004d9200, +c = 0, +1c = 0, +1d = 3, +1e = 4, colour and position
// (0,0,0,1), +18 = 0 and the manager byte +42 (00535d62) = 1; then +10 = 0,
// +dc = 1.0, +e4 = 10000.0, +e0 = 1.0 and vcall +2c (00144b80: enable; +42 = 1
// when it was disabled). A full pool leaves +d4 empty (retried every frame).
// Flicker object (+d8 == 0): *00236f20(7) is its effect id; 0036b180(0, 0x74,
// line 0x533) + 0040c240(0xe7) + 003cf0e0(kind 4) + vtable 004df860 (+60 the
// effect id, +64 0, +68 0, +70 1.0, +2f 1), vcall +90 (003cdff0(identity
// 0055c9f0, 0): a 0x60 transform component) and 0040c9d0.
// The position is 00385330(actor + 0x60, 0): attach point 0 in the world.
// With a light: l = 0.5 + e0 (1.0 - 0.5); colour (0.7 l, 0.6 l, 0.2 l, 0);
// +18 = 1 / ((r r + g g) + b b); vcall +28 (00144b10: the position, +42 = 1
// when it changed); +14 = 20.0; e0 += e4 dt and, once e0 crosses dc in the
// direction of e4: e0 = dc, dc = 1.0 (rand / 2^31), e4 = 2.0 + (10.0 - 2.0)
// (rand / 2^31) (00127dd0, cvt.s.w), negated when dc < e0.
// With the object: the actor shown (+2d, +2c, !+2e) and the object hidden:
// vcall +5c (00394270: the emitter 0036b180(0, 0x70) + 003a2dd0(effect, 1.0)
// when +60 != -1 and +64 is empty; then +2d = 1); the actor hidden and the
// object shown: vcall +60 (003941a0: 003a2ba0(-1) + 0036b130 of the emitter,
// +64 = 0; +2d = 0). An emitter gets 003a2680(0.8 + e0 (1.3 - 0.8)); then
// vcall +9c (003cdd80 -> the component's +14, 001b4960: +40 = the position).
// Release (001448a8): the light's vtable +8 (00144a60(1): id -1, vcall +30
// 00144980 disabled, +42 = 1, vtable 004d9280, 00373d70 removal), the
// object's 0040c9a0 and 003cb230(0.1).
#include "level_lights.hpp"
#include "weapon_inventory.hpp"
#include "animation_root.hpp"
#include <cstdint>
#include <functional>

namespace omega {

inline constexpr std::int32_t torch_effect_index=7;                 // 00236f20(7)
inline constexpr std::uint32_t torch_object_id_category=0xe7;       // 0040c240
inline constexpr float torch_light_range=20.0F;                     // *00489ca0
inline constexpr float torch_release_delay=0.1F;                    // 003cb230

struct TorchLightHost {
    // The light manager's pools (00535d20); 00373e70 / 00373d70 act on the point pool.
    LevelLights* lights{};
    std::function<std::int32_t(std::int32_t index)> effect_id;         // *00236f20(index)
    std::function<std::uint32_t()> random;                             // 00127dd0
    std::function<RootVector()> attach_point;                          // 00385330(actor + 0x60, 0)
    std::function<bool()> actor_shown;                                 // actor +2d && +2c && !+2e
    // The flicker object (presentation): construction (0036b180 0x74, 0040c240,
    // 003cf0e0, 003cdff0); a non-zero handle.
    std::function<std::uint32_t(std::int32_t effect)> create_object;
    std::function<void(std::uint32_t object)> register_object;        // 0040c9d0
    std::function<void(std::uint32_t object,std::int32_t effect)> create_emitter;   // 0036b180 0x70 + 003a2dd0(effect, 1.0)
    std::function<void(std::uint32_t object)> destroy_emitter;         // 003a2ba0(-1) + 0036b130
    std::function<void(std::uint32_t object,float scale)> emitter_scale;   // 003a2680
    std::function<void(std::uint32_t object,const RootVector& p)> set_position;   // vcall +9c -> 001b4960
    std::function<void(std::uint32_t object)> unregister_object;       // 0040c9a0
    std::function<void(std::uint32_t object,float delay)> release;     // 003cb230
};

// A call of the flicker object's presentation as a play host records it (the
// object, its 0x60 transform component and the emitter 003a2dd0 are particle
// presentation that is not simulated natively; the light is).
struct TorchFlickerCall {
    enum class Kind : std::uint8_t { create,register_object,create_emitter,destroy_emitter,emitter_scale,set_position,unregister_object,release };
    Kind kind{};
    std::uint32_t object{};
    std::int32_t effect{-1};     // create / create_emitter
    float value{};               // emitter_scale: 003a2680's f12; release: the delay
    RootVector position{};       // set_position
};

// 00144330 .. 00144364: the low byte of the held class (001437a0) is 10 and
// the low byte of the actor's virtual +108 (life state) is not 3.
bool torch_lit(std::uint32_t held_class,std::uint32_t life_state);

// 0014432c .. 00144908. lit: hold class 10 and life state != 3. Returns whether
// the light manager's byte +42 (00535d62) was set.
bool update_torch_light(WeaponInventory& inventory,bool lit,float dt,const TorchLightHost& host);

}
