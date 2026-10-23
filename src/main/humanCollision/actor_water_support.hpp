#pragma once
#include "actor_physical_state.hpp"
#include "water_effects.hpp"
#include <functional>
#include <optional>

namespace omega {
// Actor reads made by 001e5cf0 through physical +4, in original order.
struct ActorWaterSupportHost {
    std::function<bool()> class_key_matches;            // actor +4c == 0049d010
    // Controller at actor +58: nullopt when the pointer is null, else the
    // result of virtual +58 / +54. +54 is called only when +58 is false.
    std::function<std::optional<bool>()> controller_58,controller_54;
    std::function<void(bool)> set_in_water_346d;        // actor byte +346d
};
struct ActorWaterSupportResult {
    ActorSupportHeightChange change;
    bool published{};   // byte +346d written
    enum class Effect:std::uint8_t {none,ensure,ensure_and_update,remove} effect{};
};
// 001e5cf0. `previous_capture` is the value captured at 001e9980 entry (before
// prediction); `height`/`normal_z` are physical +e4 and +1a8 after the response
// (the water probe 001e5f30). Water support uses the same strict 0.974 normal
// test as the entry capture. Effects go to the world's water-effect manager
// (004ffc34): ensure every supported frame, update_height when the height
// changed, remove when support was lost.
ActorWaterSupportResult notify_actor_water_support(float previous_capture,float height,float unavailable_height,
    float normal_z,std::uint32_t actor,const ActorWaterSupportHost&,WaterEffectManager&,
    WaterEffectActorHost&,WaterEffectSink&);
}
