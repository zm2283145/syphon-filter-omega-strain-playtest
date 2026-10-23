#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Original World_Teardown 00134640..00134df4, called as 00134640(G) by the
// state-0x14 block (teardown_state.hpp) after 00166190. sp is the caller's $sp
// at the jal; the function's frame is sp-0x80, where it keeps the two range
// iterators it hands to 00138c60 (frame+5c/+60 for G+9c, frame+50/+54 for
// G+c4). Every callee, including the two destructor calls through vtable +8,
// is a LevelLoadHost call in the original order; the function's own stores are
// written where it writes them: G+ec, G+8c, G+dc, G+1f4, G+1f8 = 0 after their
// releases, 0053b350 / 0053b358 = 0, the three words at *(G+e8) = 0, 004ea0b8 =
// 0 after its destructor, 005721d8 = 0 when set (MP only), 005721c8 / 005721b8
// = 0, and the inlined allocator lock count 00533888 around each free
// (00533880 set: 0010dc40(9) / 0010dca8(9)). The vtable call on 003cab50's
// result has no null test in the original; a null result throws here.
void run_world_teardown(LevelLoadMemory& memory, LevelLoadHost& host,
                        std::uint32_t game = 0x4ffb50, std::uint32_t sp = 0x1f00000);

}  // namespace omega
