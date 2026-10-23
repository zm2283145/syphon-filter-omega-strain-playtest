#pragma once
// Actor message 001a92a0 actions 0x40 and 0x41 (cases 001ad5b0 / 001ad5c0):
// the flashbang hide of an AI actor (sent by its AI component, eActions 8 and
// 0xb of npc_request_action) on the secondary (upper-body) component +3314.
// Both cases return 1 from the dispatcher whatever they do.
//   0x40 -> 0018d1a0: with a component and actor mode +320c 0 or 3, the
//           one-shot request 001cae20(+3314, "flashbanghide" (0049dc20, copied
//           with a 47-byte bound), kind 1, event) with an event built by
//           003c9e30(ev, 00542b60), vtable 004da930 and +24, +28, +2c, +30,
//           +34 all zero (no completion receiver); then 003c9d60(ev, 0) and
//           +324c |= 0x100 whether or not the component accepted the request.
//   0x41 -> 0018d140: with a component and +324c bit 8, 001cae10 (+312 = +313,
//           the mode request in place of the one-shot) and +324c &= ~0x100.
// Interpreter goldens: tools/ee/gen_flashbang_hide.py.
#include "secondary_component.hpp"
#include <cstdint>

namespace omega {
struct ActorFlashbangHideResult {
    bool requested{};    // 001cae20 was called
    bool accepted{};     // its result (unused by 0018d1a0)
};
// `secondary` is actor +3314 (null: no component).
ActorFlashbangHideResult handle_actor_flashbang_hide(std::uint32_t& flags_324c,std::int32_t mode_320c,
    SecondaryComponent* secondary);
// Returns whether +312 was restored (the component present and +324c bit 8 set).
bool handle_actor_flashbang_hide_end(std::uint32_t& flags_324c,SecondaryComponent* secondary);
}
