#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// The state-0x14 teardown's callees after World_Teardown (teardown_state.hpp),
// natively over LevelLoadMemory; every callee they reach is a LevelLoadHost
// call (the default host throws naming it). The fourth, 0036c960, is
// deferred_free_pump_0036c960 (video_frame.hpp). The inlined allocator lock is
// the one of World_Teardown: byte 00533880 set -> 0010dc40(9) / 0010dca8(9),
// word 00533888 counted up around each free and back down after it.

// 003f9470(manager) (the archive manager G+3c): walks the list whose sentinel
// is manager+4 (003f9520 is the identity) from the sentinel's +4. A node whose byte +170 and
// archive byte +8+14c are both set (a persistent, open archive) is kept;
// any other node's open archive (+8+14c) is closed by 0036cd80(node+8) and
// the node is erased by 003f9a30(&next, manager, &node) (frame sp-0x50: node
// at +48, the iterator it returns at +4c), which gives the next node.
void archive_release_003f9470(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t manager,
                              std::uint32_t sp = 0x1f00000);

// 0037e320(video), the renderer teardown of the video object 005381f0 (the
// counterpart of renderer_init_0037e780). A word +0 of -1 returns at once.
// Otherwise 00375cd0(00537f80) (the DMA object); unless +0 bit 3, for each of
// the six texture slots +1068 + 0x14 i: 003813a0(+1058 the slot manager, slot
// +0, 0) and, with its byte +10, 003813a0(+1058, slot +0 + 1, 0). Then the two
// packet buffers +fb0 + 0x38 j:
// their words +0, +8, +c and +10 freed (0036b130(0, word, 004bc200, line
// 0x23e / 0x242 / 0x243 / 0x244)) and the five words +0..+10 cleared; +103c
// freed (0x24c) and cleared; +1058 passed to 00383020(word, -1), freed (0x24f)
// and cleared; the +105c listener list's destructor *(*(+105c +10) +8)(object,
// -1), freed (0x251) and cleared; 0037b0c0(video, 1); +0 = -1. Each free is
// under the lock.
void renderer_teardown_0037e320(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t video);

// 0036b250(slot): the heap slot slot (0x818 bytes from 005344b4). With its
// word +10 (005344c4) at -1 it returns. Otherwise, under the lock, the base
// block +0 (when nonzero) is freed by 0036bc10(heap, block, 004bbe50, 0x244)
// in the pool 00533cb0 + (sbyte(block-c) & f) * 818 as 0036c960 does; then
// +4, +0, +8 and +c are cleared and +10 = -1.
void heap_slot_release_0036b250(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t slot);

}  // namespace omega
