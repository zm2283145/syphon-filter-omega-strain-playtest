#pragma once

namespace omega {

// 00133420: after the world-update message, the multiplayer game's G+1f0
// accumulates its frame dt. A strict threshold sends 0012eee0(G, 1, 0)
// (statistics only) and clears the entire accumulator. The caller sends.
bool agent_update_timer_step(float& timer_1f0, float dt, bool multiplayer);

}
