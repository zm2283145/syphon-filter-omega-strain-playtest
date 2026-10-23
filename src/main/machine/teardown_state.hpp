#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// State 0x14 block 001690f0..001691a8 of the main update (G = 0x4ffb50), the
// level-end teardown. It runs after 00165610(app, 1) at 001690e8 and ends at
// the common continuation 001691a8 (the state-0x10 pause swap). app is the
// update's $s0; sp is its stack pointer at the 00166190 jal. Any other state
// returns untouched. The leaf 003e7cd0(app+0x2c) is inlined (+0x2d = 0,
// +0x2e = 1, +0x30 = -7); 00166190 goes through LevelLoadHost::call_framed
// (native run_app_teardown by default); 00134640(G), 0036c960(),
// 003f9470(G+0x3c), 0037e320(0x5381f0) and 0036b250(1), 0036b250(3) are
// LevelLoadHost calls, with *0x533ca0 = 0 between the two 0036b250 calls.
// Then G+51 = G+50 (reread) and G+50 = 1 when G+1c6 is set, else 0x0c.
void run_teardown_state(LevelLoadMemory& memory, LevelLoadHost& host,
                        std::uint32_t app, std::uint32_t sp);

}  // namespace omega
