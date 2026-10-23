#pragma once
// Actor message handler 001a92a0 for the pad actions sent by the Cross and
// Triangle senders (player_action_buttons) in a single-player game
// (multiplayer byte 005721c8 clear, so no network mirror writes).
#include "actor_edge_types.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace omega {
struct ActorPadActionState {
    std::uint32_t flags_3248{};
    std::uint32_t flags_3210{};
    std::uint32_t flags_324c{};
    std::int32_t mode_320c{};        // highest priority action
    bool byte_14{};                  // network-registered actor (set only in multiplayer, 001b5518)
    // Attached component +58: absent, or its virtual +54 (IsPlayer, 00175330
    // returns 1 for the player component).
    std::optional<bool> component_is_player;
    std::optional<bool> attachment_a0;   // resolved +3524 object's byte +a0
    bool byte_31b1{};                // Triangle also latches slot 4
    bool byte_31b5{};                // ladder slide (0x1c/0x1d)
    std::uint8_t life_state{};       // actor virtual +108
    bool linked_model_3478{};        // +3478 resolves (001adaf0)
    // Physical component +3310 fields used here.
    bool physical_present{};
    bool physical_dd{};
    bool physical_dc{};
    bool physical_9a5{};
    std::array<ActorEdgeSlotState,9>* slots{}; // physical +3e0 (required when present)
};
// Returns false for actions outside this handled set (the caller must not
// treat them as delivered). Handled: 1/2 (+3248 bit 0, set only with physical
// +dd), 9/10 (+3248 bit 1), 0x0f (slots 2, 3, 8 and 4 with +31b1), 0x10 (slot
// 1), 0x1a (slots 7, 8), 0x1b (slot 7), 0x1c/0x1d (+31b5), 0x2c/0x2d without a
// linked model (no effect). Gates follow 001ab050/001ab140/001ab7d0/001ab870:
// the local-player test returns early for a player component of a
// non-network actor on 0x10/0x1a/0x1b. 0x2c/0x2d with a linked model throw.
bool handle_actor_pad_action(ActorPadActionState&,std::int32_t action);
}
