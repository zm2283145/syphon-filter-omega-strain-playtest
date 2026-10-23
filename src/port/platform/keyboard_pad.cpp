#include "keyboard_pad.hpp"

#include "input_bindings.hpp"

namespace omega {

std::uint16_t keyboard_pad_mask(const KeyboardState& keys) {
    static const InputBindings menu = default_input_bindings(BindProfile::menu);
    return bound_buttons(menu, keys);
}

}  // namespace omega