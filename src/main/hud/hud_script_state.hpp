#pragma once
// The HUD state the mission scripts set (not drawn): the callout list at HUD
// +0x6e8 (*004ffc2c) kept by 00242fa0 / 00242b40, the player's location
// string id (004ffb50 +0x130, 0012f5c0) and the reload's TransferAmmo notice
// text (0013fdb8). The widgets those calls build, scale and fade (003e7ec0,
// 003e7ee0, 003d0810, 003f45a0, 003e8de0 ...) are presentation and are not
// modelled; the multiplayer messages (byte 005721c8) are not sent in solo.
#include "localization.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// A callout record (0x1c bytes, 00242fa0): +0 the object's +c key, +4 the
// value f12, +8 the slot a3, +c the text widget (its text here), +18 the t1
// byte (written only when the record is made).
struct HudCallout {
    std::uint32_t key{};
    float value{};
    std::uint32_t slot{};
    std::string text;
    bool flag_18{};
    bool operator==(const HudCallout&) const = default;
};

class HudScriptState {
public:
    // 00242fa0(hud, &key, text, a3, t0, t1, f12, f13): the first record
    // (list order) with +0 == key and +8 == a3 gets the text and +4 = f12;
    // without one a record is appended (00243430 -> 00138ea0). Returns true
    // when a record was made. f13 and t0 only choose widget templates and
    // effects.
    bool set_callout(std::uint32_t key, std::uint32_t slot, std::string text, float value, bool flag_18);
    // 00242b40(hud, &key, a2, a3, t0): the first record with +0 == key and +8
    // == a2 is freed and erased (00138da0); returns whether one was.
    bool clear_callout(std::uint32_t key, std::uint32_t slot);
    const std::vector<HudCallout>& callouts() const { return callouts_; }
    // 002b0f00 in solo: 0012f5c0(004ffb50, 0, 003fdb40(string)) stores the
    // string id (003fe650: -1 for a missing key) at 004ffb50 +0x130.
    void set_location(std::int32_t id) { location_ = id; }
    std::optional<std::int32_t> location() const { return location_; }
private:
    std::vector<HudCallout> callouts_;
    std::optional<std::int32_t> location_;
};

// 0013fdb8..0013fe00: sprintf(0049b1b8 "%s %s", 003fdb50(record +80),
// 003fdb50(0049b1c0 "TransferAmmo")) handed to the HUD ticker 00242b00. An
// unresolved string would be printed from NULL, which is not verified: throws.
std::string transfer_ammo_notice(const Localization& localization, std::string_view record_name_80);

} // namespace omega
