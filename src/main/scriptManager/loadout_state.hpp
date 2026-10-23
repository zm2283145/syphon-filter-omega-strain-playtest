#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Original 003e1a20(script): with the scripts-loaded byte 004938d0 set it runs
// init::ClientStartLevel (004bd8e0) through the script function runner
// 003e1b60, then, when 005721c8 (re-read) is clear or 005721c0 is set,
// init::StartLevel (004bd900). The runner stays a host service.
void run_start_level_scripts(LevelLoadMemory& memory, LevelLoadHost& host,
                             std::uint32_t script);

// State 0x12 block 00168550..00168bc8 of the main update (G = 0x4ffb50). app is
// the update's $s0 (passed to 00166190); sp is its stack pointer, which owns
// the message objects at sp+b0 and sp+e0 and the string objects at sp+190 and
// sp+1a0. Any other state returns untouched. The leaf helpers 00414790,
// 004147a0, 001692d0, 001692f0, 00169330, 00169360 and the u64-to-float
// conversion 001002d0 (with 0011b888) are native; 00166190 goes through
// LevelLoadHost::call_framed (native run_app_teardown by default); every other
// callee, including the virtual GUI-manager and actor methods, is a
// LevelLoadHost call.
void run_loadout_state(LevelLoadMemory& memory, LevelLoadHost& host,
                       std::uint32_t app, std::uint32_t sp);

// 001002d0: the unsigned 64-bit to float conversion the block uses for the
// elapsed time (exposed for tests).
float loadout_u64_to_float(std::uint64_t value);

}  // namespace omega
