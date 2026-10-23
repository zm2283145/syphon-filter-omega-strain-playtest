#pragma once
// The secondary (upper-body) component, actor +3314 (class vtable 004dacf0,
// constructor 001d81a0), as far as the firing path needs it (research/COMBAT.md,
// "C1d: secondary component"). Interpreter-verified:
//   001977f0  the mode request from the held item (+31b2 and 001cb120's mode);
//   001cb120  +313 = mode, +312 = mode unless a one-shot state 0x15/0x16/0x18
//             holds it (apply_actor_secondary_mode, actor_death.hpp);
//   001cae20  a one-shot request (reload: state 0x15, kind 1; kind 3: 0x16);
//   001cc8d4..001ccb18 in the per-frame update 001cb160 (vtable +0c): the
//             state pair +310 (current) / +311 (next) moved toward the target
//             derived from +312, with the blend +318 over +320 seconds.
// The fire gate (actor_fire.hpp, secondary_state_allows_fire) reads +310/+311.
// The part of the update before 001cc8d4 (the upper-body layers and the aim
// solve that gives the step its angle) is secondary_aim.hpp; the motion
// playback after the step (001d7480, 001d6120's timing, which restores
// +312 = +313 near the end of a one-shot motion so that the step blends out and
// posts the completion with action 4) is secondary_playback.hpp. Not ported:
// the IK after the state step; the layer +830 is idle (no instruction
// activates it).
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {

// 001977f0(actor, raise). Inputs as the function reads them.
struct SecondaryModeInput {
    bool raise{};                    // argument a1 (nonzero: raise / aim)
    std::uint8_t item_kind{};        // 00143510(+3524): item kind of the held item
    std::uint8_t hold_class{};       // 001437a0(+3524)
    std::int32_t inventory_18{};     // +3524 +18 (-1: no selection)
    std::uint8_t inventory_84{};     // +3524 +84 (6: nothing held)
    std::int32_t state_320c{};       // actor +320c
    std::optional<std::uint32_t> component_810;   // actor +3310 (nullopt: none) +810 (bit 9 read)
    std::uint32_t flags_3204{};      // actor +3204 (bits 0, 3, 4)
    std::uint32_t flags_3248{};      // actor +3248 (bits 3, 10, 12)
    std::optional<std::uint8_t> byte_33ec;        // *(+33ec) (nullopt: null pointer)
    float value_31c4{}, value_31d0{};
};
struct SecondaryModeChoice {
    std::uint8_t flag_31b2{};        // stored at +31b2
    std::uint8_t mode{};             // passed to 001cb120 when +3314 is present
};
// Item kinds 10 and above leave the mode register unset in the original: throws.
SecondaryModeChoice choose_secondary_mode(const SecondaryModeInput& input);

// The component fields the firing path needs.
struct SecondaryStates {
    std::uint8_t current_310{}, next_311{};     // constructor: 0 / 0
    std::uint8_t request_312{}, request_313{};
    std::uint8_t target_314{};
    float blend_318{};                          // 0
    float duration_320{0.4F};                   // 0x3ecccccd
};
// A one-shot slot (kind 1: +c0 name, message +32c..; kind 3: +c8, +364..):
// the motion name and the embedded event (descriptor 00542b60) the component
// posts when the one-shot ends: code +24, action +28, receiver +2c, posted
// byte +30, value +34.
struct SecondaryOneShot {
    std::optional<std::array<std::uint8_t,48>> name;
    std::int32_t code{};
    std::uint8_t action{};
    std::uint64_t receiver{};
    std::uint8_t posted{};
    float value{};
};
struct SecondaryComponent {
    SecondaryStates states;
    std::array<SecondaryOneShot,2> one_shots;   // [0]: state 0x15, [1]: state 0x16
    // +480 / +490, the left-hand grip 001d3e30 reads (constructor: vf0 /
    // 004e1d90's zero); actor logic moves them toward the held gun
    // (001b1c28, upper_body_grip_step in upper_body_combine.hpp).
    std::array<float,4> grip_rotation_480{0.0F,0.0F,0.0F,1.0F},grip_offset_490{};
};
// 001cae20(component, name, kind, descriptor): kind 1 asks for state 0x15, kind 3
// for 0x16; refused (false) when +310, +311 or +312 already is that state. On
// success +312 = the state, the name is copied (when the buffer is new, or
// either first byte is nonzero) and the descriptor fields are stored. Other
// kinds return false without effect.
bool request_secondary_one_shot(SecondaryComponent& component,const std::array<std::uint8_t,48>& name,
    std::uint8_t kind,const SecondaryOneShot& descriptor);

// What a completed transition out of a one-shot state does (001d79b0 for 0x15,
// 001d7940 for 0x16): with a receiver and nothing posted yet, action = 4,
// value = dt and the event is posted (003c9420); then posted = 1, receiver = 0.
struct SecondaryEventPost {
    std::size_t slot{};
    std::int32_t code{};
    std::uint8_t action{};
    std::uint64_t receiver{};
    float value{};
    bool operator==(const SecondaryEventPost&) const=default;
};
struct SecondaryStep {
    bool snapped{};                  // s2: the pair was set or completed this frame
    bool changed{};                  // fp: +311 (or the pair) changed this frame
    std::vector<SecondaryEventPost> posts;
};
// 001cc8d4..001ccb18 with dt (f23) and the aim angle f0 (00121ab8 of the
// update's aim vector, an input here). Target state from +312: 2 -> 3 when
// -0.2618 <= angle <= 1.30899, else 4; 8 / 9 -> 4 when angle < (target 4 ?
// -0.2618 : -0.7854), else 0xa / 0xb; other modes as requested. A zero
// duration throws (the original divides by it).
SecondaryStep step_secondary_states(SecondaryComponent& component,float aim_angle,float dt);

}
