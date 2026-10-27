#include "interact_gobj_pairing.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr float same_height = 5.0F; // 0x40a00000, c.olt.s on the absolute difference
bool level(float d) { return std::fabs(d) < same_height; }
}

std::vector<InteractPairingSend> interact_pairing_trigger(InteractPairingState& state, std::uint64_t actor,
                                                          std::optional<float> node_z,
                                                          const std::function<float(std::uint64_t)>& z_of) {
    std::vector<InteractPairingSend> sends;
    if (!actor) throw std::runtime_error("InteractGOBJ pairing 00216230: the actor +30 does not resolve");
    if (state.done_b0) return sends; // 0021624c
    if (actor == state.first_b4) {   // 0021626c: the first agent leaves
        state.first_b4 = 0;
        sends.push_back({actor, state.first_up_94 ? 0x36 : 0x32});
        state.first_up_94 = false;
        return sends;
    }
    // 002162d4: the zero-terminated list from +b8 (+bc is null: 002165c4 ignores triggers while a clone is held).
    if (state.second_b8 && actor == state.second_b8) return sends;
    if (!z_of) throw std::runtime_error("InteractGOBJ pairing 00216230: actor height hosting required");
    if (!state.first_b4) {
        if (!node_z) return sends; // 00216300: +a8 is null
        state.first_b4 = actor;    // 00216360 (delay slot)
        if (level(*node_z - z_of(actor))) {
            state.first_up_94 = true;
            sends.push_back({actor, 0x35});
        } else {
            sends.push_back({actor, 0x31});
        }
        return sends;
    }
    if (state.second_b8) return sends; // 002163bc
    if (!node_z) throw std::runtime_error("InteractGOBJ pairing 00216230: +a8 is null with a first agent (read unchecked at 002163e8)");
    const std::uint64_t first = state.first_b4;
    const float first_d = *node_z - z_of(first);  // f20
    const float actor_d = *node_z - z_of(actor);  // f2
    if (level(first_d) && level(actor_d)) return sends; // 00216464: both already up
    state.done_b0 = true;
    state.second_b8 = actor;
    if (level(actor_d)) {          // the new agent is up: it pulls the first one
        sends.push_back({actor, 0x38});
        sends.push_back({first, 0x37});
    } else if (state.first_up_94) { // the first agent is up
        sends.push_back({first, 0x38});
        sends.push_back({actor, 0x37});
        state.first_up_94 = false;
    } else {                        // both below: the first boosts the new agent
        sends.push_back({actor, 0x34});
        sends.push_back({first, 0x33});
    }
    return sends;
}

InteractPairingCodeResult interact_pairing_code(InteractPairingState& state, std::int32_t code, std::uint64_t sender) {
    InteractPairingCodeResult result;
    switch (code) {
    case 0xe:
        result.forward = state.clone_bc;
        state.clone_bc.reset();
        return result;
    case 0xd:
        state.clone_bc.reset();
        state.done_b0 = false;
        for (const auto a : {state.first_b4, state.second_b8})
            if (a && a != sender) result.interrupted.push_back(a);
        state.first_b4 = 0;
        state.second_b8 = 0;
        result.self_trigger = 3;
        return result;
    case 0xf:
        state.clone_bc.reset();
        for (auto* slot : {&state.first_b4, &state.second_b8}) // 002169b0 loop
            if (*slot == sender) *slot = 0;
        if (!state.first_b4 && !state.second_b8) state.done_b0 = false;
        result.self_trigger = 2;
        return result;
    default: // 00216580: any other CAS kind falls out of the switch and returns 1 (nothing)
        return result;
    }
}

std::vector<InteractPairingSend> interact_pairing_receive_trigger(InteractPairingState& state, std::int32_t action,
                                                                  std::uint64_t actor, std::optional<float> node_z,
                                                                  const std::function<float(std::uint64_t)>& z_of,
                                                                  std::uint32_t word_28, std::uint64_t sender_2c) {
    if (state.clone_bc || !actor) return {};
    if (action == 2 || action == 3) return {};
    if (action == 7) return interact_pairing_trigger(state, actor, node_z, z_of);
    state.clone_bc = InteractTriggerClone{action, word_28, sender_2c, actor};
    return {{actor, 0x25}};
}
}
