#include "game_end_state.hpp"
#include <stdexcept>

namespace omega {

LevelEndRequestResult request_level_end(GameEndState& g, std::uint8_t kind, ActorFader& fader, bool multiplayer) {
    if (multiplayer)
        throw std::runtime_error("Level end: multiplayer (0012eee0, ENDLEVEL +20, the remote slots) is not hosted");
    bool completed = false;
    if (kind == 2 || kind == 3) {
        g.b1c4 = 1; g.b1c5 = kind == 3 ? 1 : 0; g.b1c6 = 1; g.b1c7 = 0;
    } else if (kind == 0) {
        g.b1c4 = 0; g.b1c5 = 0; g.b1c6 = 0; g.b1c7 = 0;   // +1c6 = *005721c8, 0 in solo
        completed = true;
    } else if (kind == 1) {
        g.b1c4 = 0; g.b1c5 = 1; g.b1c6 = 1; g.b1c7 = 1;
    }
    // 001314f0(G, -1) -> 00335830(agent, completed): +b74.
    g.agent_b74 = completed ? 1 : 0;
    ++g.agent_end_calls;
    if (g.end_agent) g.end_agent(completed);
    // 00136a90. 003caca0 first forwards the message to G's child list; the
    // port hosts no G child (its result is not used on the ENDLEVEL path).
    LevelEndRequestResult r;
    if (g.state_50 == 0x13 || g.state_50 == 0x14) return r;
    g.previous_51 = g.state_50;
    if (g.b1c4 && g.b1c5 && g.b1c6) g.state_50 = 0x14;
    else {
        g.state_50 = 0x13;
        start_actor_fade(fader, 2, 1.0F);
        r.fade_started = true;
    }
    r.handled = true;
    return r;
}

bool level_end_fade_check(GameEndState& g, const ActorFader& fader, bool remote_players_ready) {
    if (g.state_50 != 0x13 || fader.duration_04 != 0.0F || !remote_players_ready) return false;
    g.previous_51 = 0x13;
    g.state_50 = 0x14;
    return true;
}

std::optional<std::uint8_t> level_end_teardown(GameEndState& g) {
    if (g.state_50 != 0x14) return std::nullopt;
    g.previous_51 = 0x14;
    g.state_50 = g.b1c6 ? 1 : 0x0c;
    return g.state_50;
}

bool game_state_runs_gameplay(const GameEndState& g) { return g.state_50 == 0x0f || g.state_50 == 0x13; }

}
