#pragma once
#include "actor_fader.hpp"
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

// The game object G = 004ffb50: the state byte +50 (004ffba0; 15 = play),
// the previous state +51, the end bytes +1c4..+1c7 that 0012fa60 writes, and
// the local AgentData record's +b74 that 00335830 writes. When the play host
// has the front end's agent record (--frontend) `end_agent` runs the native
// 00335830 (agent_refill.hpp end_agent_level) on every 0012fa60 call; without
// an agent record (direct play) only +b74 is mirrored here and
// agent_end_calls counts the calls.
struct GameEndState {
    std::uint8_t state_50{15};
    std::uint8_t previous_51{};
    std::uint8_t b1c4{}, b1c5{}, b1c6{}, b1c7{};
    std::uint8_t agent_b74{};
    unsigned agent_end_calls{};
    std::function<void(bool completed)> end_agent;   // 001314f0(G, -1) -> 00335830
};

struct LevelEndRequestResult {
    bool handled{};        // 00136a90 returned 1 (not the 0x13/0x14 re-entry guard)
    bool fade_started{};   // 0025e6e0(*(G+e8), 2, 1.0)
};

// Explicit boundaries of the multiplayer request. The original receiver calls
// 0012eee0(G, 0, 1) before changing state, then 003c9420 continues to 003c94a0
// after local delivery (even when the receiver's re-entry guard rejects it).
// continue_message must respect the routing flags: client flag 1 is local-only.
// These callbacks must provide the real network services, not silent no-ops.
struct LevelEndNetworkHost {
    bool host_role{};       // *005721c0 != 0
    std::function<void()> send_agent_data;   // 0012eee0(G, 0, 1)
    std::function<void(std::uint8_t flags_20, std::uint8_t kind_24, std::uint32_t quit_28)> continue_message;
};

// 0012fa60(G, kind) and the synchronous MSGGAME_ENDLEVEL delivery (key
// 004f5398, 003c9420 transport 1) to G's receiver 00136a90. `fader` is
// *(G+e8) = *004ffc38, the fader the death code 16 (00176d30) also starts.
// Multiplayer requires both network services; otherwise throws before writes.
// This does not supply remote readiness, incoming delivery, or debrief transport.
LevelEndRequestResult request_level_end(GameEndState& g, std::uint8_t kind, ActorFader& fader, bool multiplayer,
                                       const LevelEndNetworkHost* network = nullptr);

// 00168f10, before the gameplay block of the main update 00167c40: state
// 0x13 with fader +4 == 0.0f and 0012ed60(G) (every active remote player
// ready; solo: no remote player) moves to 0x14. Returns true on the move.
bool level_end_fade_check(GameEndState& g, const ActorFader& fader, bool remote_players_ready);

// 001690f0, after the gameplay block of the same call: in state 0x14 the
// teardown runs (00166190, 00134640, ...; not hosted) and +50 becomes 1 when
// +1c6 is set, otherwise 0x0c, with +51 = 0x14. Returns the next state.
std::optional<std::uint8_t> level_end_teardown(GameEndState& g);

// 00168dc8: the gameplay block (message pump, world update, actors) runs only
// in states 0x0f and 0x13.
bool game_state_runs_gameplay(const GameEndState& g);

}
