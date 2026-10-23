#pragma once
// The aiming reticle switch 00241ec0 (Checkpoint 121d), called from the pad
// producer at 0x0024f940 with the HUD *004ffc2c, a1 = sp+1e0, a2 = sp+200,
// a3 = the held record (001437e0) and t0 = the goggles record (0013fb50 when
// actor +346f is set, else 0). It picks the reticle index s2 and, when it
// differs from the HUD's +8c, switches the reticle elements; its return
// (the held record's signed byte +100 when s2 >= 0) is stored to actor
// +346e. Only the index, the return and the ordered HUD / GUI element calls
// are computed; the elements are not drawn.
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {

struct HudCrosshairRecord {
    std::uint8_t class_f0{};      // hold class byte +f0
    std::int32_t index_130{};     // reticle index word +130
    std::int8_t value_100{};      // signed byte +100 (the return)
};

// 0x00241ec0..0x00241fc8. s2 = -1 without a1; with !a2 and a goggles record
// s2 = goggles +130 + 1, final when >= 0. Otherwise (s2 < 0) it stays as it
// is with no held record, a hold class 3, 4 or 10 ((1 << (f0 - 3)) & 0x83)
// or when the camera's object is not the local player (camera_matches_ffc10:
// 00159570(0x004ffc14) ? *(00159560(0x004ffc14)) : 0, compared with
// *0x004ffc10); else s2 = held +130 + 1 when !a2 and +130 >= 0, else 0.
std::int32_t hud_crosshair_index(bool aim_1e0,bool far_200,const std::optional<HudCrosshairRecord>& held,
    const std::optional<std::int32_t>& goggles_130,bool camera_matches_ffc10);

// The HUD object's reticle part: +80 count, +84 elements (null = nullopt;
// otherwise the element's +4 value), +8c the current index.
struct HudCrosshairState {
    std::int32_t index_8c{-1};
    std::vector<std::optional<float>> elements_84;
};

struct HudCrosshairEvent {
    enum class Kind : std::uint8_t {
        default_a10=1,   // 00244a10(hud): 00244280, clone +58 into +68, 003e8de0(+68, 1), 003f45a0(+68, 0), 003f44e0(+68, 17)
        release_a00,     // 00244a00(hud) = 00244280
        clear_9a0,       // 002449a0(hud): 003e8de0(+78, 0)
        visible_de0,     // 003e8de0(element, value)
        alpha_5a0,       // 003f45a0(element, value): +4 = value, +10 = 0, virtual +10
        fade_4e0,        // 003f44e0(element, value): +8 = +c = value, +10 = (+4 <= value ? 1 : -1), +14 = 0
    } kind{};
    std::int32_t element{-1};     // +84 index; -1 for the HUD helpers
    std::uint32_t value{};        // visibility byte or the float's bits
    bool operator==(const HudCrosshairEvent&) const=default;
};

struct HudCrosshairResult {
    std::int8_t value_346e{};
    std::int32_t index{};
    std::vector<HudCrosshairEvent> events;
};

// 0x00241fc8..0x0024217c on the HUD state for the index s2 (above): the
// switch when s2 != +8c (new s2 == 0: 00244a10; s2 > 0: the element s2 - 1
// is shown (003e8de0 1, 003f45a0 1.0, 003f44e0 10.0) when present, then
// 00244a00; then the old +8c: 0 -> 00244a00, > 0 -> 003f44e0(old - 1, 0)
// when present); 002449a0 when s2 < 0 and again for a held hold class 1;
// every element k != s2 - 1 of the +80 count whose +4 is 0.0 is hidden
// (003e8de0(e, 0), 00242180); +8c = s2. The element +4 values evolve in the
// screen update of the 003f44e0 fades (003e79c0 / 003f43d0, HudObject::update_screen): here they are inputs.
HudCrosshairResult hud_crosshair_update(HudCrosshairState& hud,std::int32_t s2,
    const std::optional<HudCrosshairRecord>& held);

}