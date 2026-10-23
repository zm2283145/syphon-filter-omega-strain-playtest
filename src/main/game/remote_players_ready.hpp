#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Port of 0012ed60(G), the remote-players-ready check of the 0x13 level-end
// fade (00168f10). For player slots 0..3 it skips the local slot (00494178 when
// the MP byte 005721c8 is set, lazily initialised by 002ebaf8(00494178) while
// it is -1; slot 0 otherwise). A slot whose 003fb8d0(slot) (network player map
// lookup) is nonzero and whose agent record *(G + 1c8 + 4 * slot) (001314f0,
// inlined: the slot is never -1) is nonzero must have the agent's +b70 byte
// set, otherwise it returns 0; else 1. The original also loads 005721a8 into
// $a1 before each 003fb8d0 call; 003fb8d0 overwrites $a1 with its second
// instruction (lui a1 0x56) before reading it, so that load is dead and the
// port does not perform it. 002ebaf8 and 003fb8d0 are host services.
std::uint32_t remote_players_ready_0012ed60(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t game);

}  // namespace omega
