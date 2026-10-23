#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// The gameplay block 00168dc8..001690f0 of the main update 00167c40 (G =
// 0x4ffb50), run in G+50 states 0x0f (play) and 0x13 (level-end fade); any
// other state returns untouched. app is the update's $s0; sp is its stack
// pointer, whose frame holds the message object at sp+80.
//
// Multiplayer (byte 005721c8): the network pump 003fbec0(); unless G+50 (reread)
// is 0x13, the quit request 005721d8 (00169360: read and cleared) opens the
// quit dialog 002cae30(*(G+b4), 2, title, name, *0x571fb0, 0x668 when byte
// 005723b0 == 1 else 0x667), the title being 00169330() when nonzero (called
// again), else 003fdb50("QuitTitle"), and the name 001692f0(); then the
// disconnect byte 005723b8 (read and cleared) calls 002ca640(*(G+b4), 0).
//
// 00168f10: in state 0x13 with fader *(G+e8) +4 == 0.0f (c.eq.s; -0 counts) and
// 0012ed60(G) != 0: G+51 = G+50 (reread), G+50 = 0x14.
//
// The frame: 0037a9f0(0x5381f0); the video record 0x5381f0 + *0x538c60 * 0x110
// gets the 16 bytes at 0x539170 at +1f0 and bytes +2f5/+2f6 = 1; 003c8de0();
// 0040c5c0(); the message sp+80 (003c9e30(sp+80, 0x5436b0); vtable 0x4d9160
// before both 003c9420(sp+80, G, 1) and 003c9d60(sp+80, 0)); 0x533870 +=
// T1_COUNT (0x10000800), T1_COUNT = 0; 00132e50(G); the u64 0x5392d8 = the u64
// 0x4bc1b0; the record (index reread) +2e0 = 0, +2e8 = 1.0f, +2f6 = 1, +2f4 = 1;
// the identity matrix at 0x5381f0 + 0xa80 + *0x538c64 * 0x40 (rows written
// 3, 1, 2, 0); byte 0x538c68 = 1; 003d0c10(003d1020()); and the trailing
// 00165610(app, 1), the leaf *0x55a818 += 1.
//
// The leaves 00169360, 00169330, 001692f0, 004147a0 and 00165610 are inlined;
// every other callee is a LevelLoadHost::call boundary (the defaults throw).
// A null fader or a denormal fader +4 (c.eq.s operand semantics) throws.
void run_gameplay_state(LevelLoadMemory& memory, LevelLoadHost& host,
                        std::uint32_t app, std::uint32_t sp);

}  // namespace omega
