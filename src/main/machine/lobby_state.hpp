#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// G+50 state-5 and state-6 blocks 00168bcc..00168dc8 of the main update
// 00167e60 (G = 0x4ffb50). app is the update's $s0; sp is its stack pointer,
// whose frame holds the 0x10-byte discovery record at sp+170 (sp+180 = 0xe4a).
//
// State 5 (multiplayer lobby, entered from the state-3 multiplayer path): on
// first entry (byte 0048a008) video setup 0037e780(0x5381f0, app+254, app+258,
// 0x20, 0x10, app+250, -1, -1), the 005383d4/005383c0 video stores and
// 00165950(app); then always the lobby update 00167630(app). The block falls
// through to the state-6 test, which re-reads G+50.
//
// State 6 (BOOT_AS_SERVER wait for players; no store of 6 to G+50 exists in the
// executable): on first entry (byte 0048a020) 00165bc0(), the
// 003cba90("BOOT_AS_SERVER") / 00121bc8 (atoi) server flag 004ee488, the
// create-game 0042a870(0) when set, 001674e0(app); then the client discovery
// 0042aec0 / 0042a520 / 0042a780 (connected flag 004ee490), the
// "Waiting for %d more %s to connect." draw 00166fe0(app, 005723f0, 0048a028)
// and, once 0048a028 - 1 == 005723f0, 00166f20(app), 0037e320(0x5381f0) and
// G+51 = G+50, G+50 = 9 (intro movie). Any other state returns untouched.
//
// Every callee is a LevelLoadHost::call boundary (the defaults throw).
void run_lobby_states(LevelLoadMemory& memory, LevelLoadHost& host,
                      std::uint32_t app, std::uint32_t sp);

}  // namespace omega
