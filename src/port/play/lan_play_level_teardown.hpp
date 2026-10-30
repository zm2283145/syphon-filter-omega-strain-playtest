#pragma once
// PORT-ONLY: compose the retail state-0x14 teardown mirror with one live LAN
// game's native DME memory. The front end supplies its real debrief actions
// and binds LanNetCore's 0044e340 callback to the lobby's exit-game service.

#include "play_level_teardown.hpp"

namespace omega::lan {
class LanNetCore;
}

namespace omega {

PlayTeardownReport run_lan_play_level_teardown(lan::LanNetCore& core, const GameEndState& game,
                                               const PlayTeardownActions& actions);

}  // namespace omega
