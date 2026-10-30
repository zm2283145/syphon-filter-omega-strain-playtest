#pragma once
// PORT-ONLY: compose the retail state-0x14 teardown mirror with one live LAN
// game's native DME memory. The front end supplies its real debrief actions
// and binds LanNetCore's 0044e340 callback to the lobby's exit-game service.

#include "play_level_teardown.hpp"

namespace omega::lan {
class LanNetCore;
}

namespace omega {

// Explicitly opt into G+1c7's retained DME session. The live play uses the
// default exit path until its front end can carry the core into another level.
enum class LanTeardownSession { Exit, Retain };

PlayTeardownReport run_lan_play_level_teardown(lan::LanNetCore& core, const GameEndState& game,
                                               const PlayTeardownActions& actions,
                                               LanTeardownSession session = LanTeardownSession::Exit);

}  // namespace omega
