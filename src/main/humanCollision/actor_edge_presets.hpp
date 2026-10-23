#pragma once
#include "actor_edge_types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {
// Physical edge state retained by 001dfb80: the nine slots at +3e0 (slot 6
// request/query bytes are +7a0/+7a1), the ladder contact fields +990..+9a5
// and the slot-6 edge direction copied to +a10.
struct ActorEdgePresetState {
    std::array<ActorEdgeSlotState,9> slots{};
    ActorLadderContactFields ladder;
    RootVector direction_a10{};
    bool operator==(const ActorEdgePresetState&) const=default;
};
struct ActorEdgePresetHost {
    // Detection body 1dfcb8..1e0ddc (detect_actor_edges with the live inputs);
    // it publishes slots, ladder fields and +a10 into the state.
    std::function<void(ActorEdgePresetState&)> detect;
    // 001e1600 request handling: select_actor_edge_request and immediate
    // delivery of its messages. Called only when some slot is requested.
    std::function<void(ActorEdgePresetState&)> report_requests;
};
// 001dfb80 entry. Mode and bits are physical +20/+24 after the history
// advance. Bits24 bit 1 latches slot 6 (+7a0 = 1, +7a1 = 0). Every slot's
// +02 result and +70 mask are cleared. Detection runs for mode 0 with any slot
// request, for bits24 bit 1, or for mode 9. Then 001e1600 runs when any slot
// is requested (without one it has no effect) and every request byte is
// cleared. An empty host callback that is required throws. Returns whether the
// detection body was required.
bool run_actor_edge_presets(ActorEdgePresetState&,std::int32_t mode_20,std::uint32_t bits_24,
    const ActorEdgePresetHost& host={});
}
