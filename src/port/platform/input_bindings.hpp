#pragma once
// Port-only keyboard and mouse bindings (checkpoint 132win6, not the original: the PS2 game reads only the
// DualShock 2 pad, plus the USB keyboard in text entry). Every binding produces the bits of the pad word
// 0055a44c (ds2 masks, gamepad_reports.hpp), the same bits a controller's button sets, so a key reaches
// the game exactly as the pad button it stands for.
//
// Two profiles:
//   menu  the front end (title, menus, loading screens, the level end)
//   play  the level (the world's input and the in-level start / pause / loadout menus)
// Defaults (menu):
//   Up/Down/Left/Right arrows  D-pad         Enter, Space  Cross       Esc, Backspace  Triangle
//   Shift                      Square        Ctrl          Circle      Q / E           L1 / R1
//   1 / 3                      L2 / R2       Z / C         L3 / R3     Tab             Select
//   P                          Start
// Defaults (play; the sandbox keys of earlier checkpoints, plus the mouse buttons and L3 / R3):
//   W A S D, arrows  left stick (Shift: walk, 0.6 of full deflection)    T F G H  right stick
//   Space Cross   R Triangle   J, left mouse Square   K Circle   I, right mouse L1   U R1
//   Q L2 (pressure)   E R2 (pressure)   X / M L3 / R3   Z Select   P Start
//   Y / B / V / N  D-pad Up / Down / Left / Right; in the in-level menus the arrows are the D-pad as well
// Port settings keys (port_settings.hpp keeps them as unknown keys; parse_input_bindings reads them):
//   bind.<profile>.<button>=<key>[,<key>...]   button: cross circle square triangle l1 r1 l2 r2 l3 r3
//                                              select start up down left right
//   bind.play.stick_left.<up|down|left|right>=...   bind.play.stick_right.<...>=...   bind.play.walk=...
//   bind.play.menu_dpad.<up|down|left|right>=...     (the in-level menus' extra D-pad keys)
//   A value replaces the default list; "none" leaves the input unbound. A key named for a button leaves the
//   default list of any other button in that profile (so bind.play.cross=MouseLeft moves the left mouse button
//   off Square); the stick, walk and menu_dpad lists are separate and keep their keys. Key names: A-Z, 0-9, F1-F24
//   (not F11), Up Down Left Right Enter Space Esc Backspace Tab Shift LShift RShift Ctrl LCtrl RCtrl
//   Alt Home End PageUp PageDown Insert Delete Num0-Num9 NumAdd NumSub NumMul NumDiv NumDecimal
//   Comma Period Slash Semicolon Quote LBracket RBracket Backslash Minus Equals Grave
//   MouseLeft MouseRight MouseMiddle Mouse4 Mouse5. Reserved for the window (host_window.hpp): F11,
//   Alt+Enter, Alt+F4. The mouse's motion is not bound (the wheel is the sandbox camera zoom).

#include "keyboard_pad.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

struct PortSettings;

enum class BindProfile { menu, play };

struct InputBindings {
    using Keys = std::vector<std::uint8_t>;   // Windows virtual-key codes (mouse buttons are VK 1, 2, 4, 5, 6)
    std::array<Keys, 16> buttons;             // by ds2 bit index: buttons[k] sets 1 << k
    std::array<Keys, 4> stick_left;           // up, down, left, right
    std::array<Keys, 4> stick_right;
    Keys walk;                                // scales the key stick to 0.6
    std::array<Keys, 4> menu_dpad;            // play: extra D-pad keys of the in-level menus
};

struct BindingSet {
    InputBindings menu, play;
    std::size_t overridden = 0;   // bind.* keys read from the settings
};

// A key's result: the pad word bits and the two sticks (x right, y up, each -1..1).
struct BoundInput {
    std::uint16_t held = 0;
    std::array<float, 2> left{}, right{};
};

InputBindings default_input_bindings(BindProfile profile);
BindingSet default_binding_set();
// The bind.* keys of the settings over the defaults; throws on an unknown key, button or key name.
BindingSet parse_input_bindings(const PortSettings& settings);

std::uint16_t bound_buttons(const InputBindings& b, const KeyboardState& keys);
BoundInput bound_input(const InputBindings& b, const KeyboardState& keys);
// The in-level menus' pad word: the play buttons and the menu_dpad keys.
std::uint16_t bound_menu_buttons(const InputBindings& play, const KeyboardState& keys);
std::uint16_t bound_menu_dpad(const InputBindings& play, const KeyboardState& keys);   // the menu_dpad bits only
// Any bound button or stick key down (the controller-removed state's keyboard take-over, pad_connection.hpp).
bool any_bound_key(const InputBindings& b, const KeyboardState& keys);

std::optional<std::uint8_t> key_code(std::string_view name);   // case-insensitive
std::string key_name(std::uint8_t vk);
const char* ds2_button_name(std::size_t bit);                  // "l2" .. "left"
// One line per input: "cross: Enter, Space".
std::vector<std::string> describe_input_bindings(const InputBindings& b, BindProfile profile);

// The host keyboard and mouse buttons (GetAsyncKeyState); Alt suppresses Enter (Alt+Enter is the fullscreen
// toggle). All released off Windows.
KeyboardState host_keyboard_state();

}  // namespace omega
