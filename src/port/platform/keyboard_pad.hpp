#pragma once
// Host keyboard to DualShock 2 button mask (the layout of the pad word
// 0x55a300 + 14c, ds2 masks in gamepad_reports.hpp). A host choice, not game
// data: the PS2 game reads only the pad (and the USB keyboard in text entry).
// Checkpoint 132win6: the front end's default keys, now the "menu" profile of the configurable bindings
// (input_bindings.hpp, which lists them and the play profile).
#include <array>
#include <cstdint>

namespace omega {

using KeyboardState = std::array<bool, 256>;   // indexed by Windows virtual-key code

// The default menu bindings' mask (bound_buttons(default_input_bindings(BindProfile::menu), keys)).
std::uint16_t keyboard_pad_mask(const KeyboardState& keys);

}  // namespace omega