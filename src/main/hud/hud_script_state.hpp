#pragma once
// The HUD state the mission scripts set (not drawn): the callout list at HUD
// +0x6e8 (*004ffc2c) kept by 00242fa0 / 00242b40, the player's location
// string id (004ffb50 +0x130, 0012f5c0) and the reload's TransferAmmo notice
// text (0013fdb8). The widgets those calls build, scale and fade (003e7ec0,
// 003e7ee0, 003d0810, 003f45a0, 003e8de0 ...) are presentation and are not
// modelled; the multiplayer messages (byte 005721c8) are not sent in solo.
#include "localization.hpp"

#include <array>
#include <cstddef>
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
    void set_location(std::int32_t id) { locations_[0] = id; }
    std::optional<std::int32_t> location() const { return locations_[0]; }
    // 134on3: cGameSystem_SetClientPlayerLocation 0012f5c0(004ffb50, index, id) / Get 0012f5b0 for the four
    // clients of a network game (+0x130 + 4 * index): 002b0f00 on a player GOBJ and the receive 00323ad0.
    void set_location(std::size_t index, std::int32_t id) { locations_.at(index) = id; }
    std::optional<std::int32_t> location(std::size_t index) const { return locations_.at(index); }
private:
    std::vector<HudCallout> callouts_;
    std::array<std::optional<std::int32_t>, 4> locations_{};
};

// 0013fdb8..0013fe00: sprintf(0049b1b8 "%s %s", 003fdb50(record +80),
// 003fdb50(0049b1c0 "TransferAmmo")) handed to the HUD ticker 00242b00. An
// unresolved string would be printed from NULL, which is not verified: throws.
std::string transfer_ammo_notice(const Localization& localization, std::string_view record_name_80);

// 134on3: the quick-chat line of 002b1190(a, n, id) and GuiQuickChat 002b1350's item: sprintf(004abec8 "%s %s",
// 003fdb50(sprintf(004abeb8 "%d_%d_%d", 30, a, n)), id > 0 ? 003fdb80(id) : "" (004abed0)) (n = the sender's
// client index + 1). An unresolved first string would print NULL: throws naming 002b1190.
std::string quick_chat_line(const Localization& localization, std::int32_t a, std::int32_t n, std::int32_t location);
// The ticker colour 00587c90 the line is posted with (003fa3f0(hud + 100, text, 00587c90, 1, 0)); the booted EE
// snapshot's value.
inline constexpr std::array<float, 4> quick_chat_colour{0.6509804F, 0.7019608F, 1.0F, 1.0F};
// 0048bd10: GuiQuickChat's menus, 20 bytes each: a submenu byte, then the four D-pad entries (Up 0x1000, Down
// 0x4000, Left 0x8000, Right 0x2000). A submenu row's entries are menu numbers, labelled "QC_MENU_%d" (004abee8);
// an item row's entries are line numbers, labelled "QC_ITEM_%d" (004abef8).
struct QuickChatMenu {
    bool submenu{};
    std::array<std::int32_t, 4> entries{};
};
inline constexpr std::array<QuickChatMenu, 5> quick_chat_menus{{
    {true, {1, 2, 3, 4}}, {false, {12, 13, 14, 19}}, {false, {5, 6, 4, 7}}, {false, {8, 11, 10, 9}},
    {false, {15, 16, 17, 18}}}};

} // namespace omega
