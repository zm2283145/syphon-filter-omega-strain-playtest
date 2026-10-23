#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Port of 0037a9f0, the per-frame video begin on the video object (005381f0
// in the game), with its whole callee set except the allocator and interrupt
// services: 0037a750 (flag 8 path), 003bafc0/00382c30/00383560 (release of the
// six +1068 texture slots), 0037a370 (fade quad), 00375650/00375770/00375c30
// (VIF1 packet flip and kick on the DMA object 00537f80), 0037b5c0/0037b330
// (display registers), 00382060/003820c0, 0037b0c0/0037fba0 (deferred buffer
// list), 0037b000, 0037a5c0 (frame clear packet), 003d24b0 (listener update)
// and 0036c960 (below).
//
// The +1058 object that 00382060/003820c0/00382c30 work on is the texture slot
// manager itself: in the game +1058 = 00539248, the slot manager pointer.
//
// Hardware registers are EE memory like any other address: the timer window
// 10000800/10000810, the DMA channel and controller registers 1000xxxx and the
// GS privileged registers 12000000..12001000 are read and written exactly where
// the original does, so a caller sees every register store as a memory effect.
// The original busy-waits on DMA/FIFO status (10009000, 10003c00, 1000a000,
// 10003020) and on the GS CSR; a status that is busy in memory can never change
// here, so the port throws instead of returning. The same holds for the frame
// limiter loop (+1e4) when its condition is unmet after one iteration.
//
// Host boundaries: 0036b130 (free), 0036bc10 (pool free), 0010dc40/0010dca8
// (_DisableIntc/_EnableIntc 9), 0010d3e0 (FlushCache 0), and the listener
// methods of the +105c list, called through their vtables (method +c with the
// listener, method +8 with the listener and $f12 = the +1e0 frame time).
void video_frame_begin_0037a9f0(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t video);

// The +1e8 store of 0037a9f0 (0037ab70, 005383d8 in the game): the IEEE bits
// of div.s(576000.0f, cvt.s.w(t1_count)), t1_count being T1_COUNT (10000800)
// plus 00533870 since the previous frame's tick accumulation. Between two
// 0037a9f0 calls nothing else clears T1 or 00533870 in play (the other writers
// are 0037a750's and 0037a9f0's own accumulations, the delay loop 002c84b0 and
// the fold 00169040/0016904c, which keeps the sum), so the value is the frame
// rate over that interval: 9600 counts (1/60 s at 576 kHz) give 60.0f.
std::uint32_t video_rate_1e8_0037ab70(std::uint32_t t1_count);

// Port of 0036c960: drains the deferred-free queue (count 00533c90, entries
// 00533890), returning each block to its pool 00533cb0 + (sbyte(block-c) & f) *
// 818 through 0036bc10, inside the 00533880/00533888 interrupt lock.
void deferred_free_pump_0036c960(LevelLoadMemory& memory, LevelLoadHost& host);

// Port of 0037e780, the renderer init on the video object (005381f0), with its
// pure callees 0037f740 (display mode from the 004bc1d0 table and the view
// constants), 0037f670 (GS page layout +12c..+140), 003d28d0/00139070 (the
// +105c listener list constructor), 001260f0 (zero fill) and 0037b000. The
// arguments are the EE registers a1..t3: width, height, colour bits (0x20,
// 0x18 or 0x10), depth bits, flags (+0; bit 0 interlace, 1/2/4 video mode, 8
// no packet kick), the packet quadword capacity +1024 and the record count
// +1028 (-1 selects 0xdac and 0x15e). The +0x2f6 record byte, GS register
// images and statistics roll are the ones 0037a9f0 uses.
//
// The original busy-waits on 00103710 until it returns nonzero; the port calls
// it the same way. A zero +10 and +14 leave 0037f740 without a display mode
// (it would index the table with a stale register), so the port throws there.
//
// Host boundaries: 0037e320 (teardown, when +0 is not -1), 00102a28,
// 00102888, 0010d3e0 (FlushCache), 00103710, 001037a8 (GS library),
// 0010dc40/0010dca8 (_DisableIntc/_EnableIntc 9), 0036b180 (allocator, file
// 004bc200), 003831d0 (texture slot manager constructor), 003bb1c0 (the six
// +1068 texture slots) and 00379fe0 (the initial packet kick, then VIF0_ERR /
// VIF1_ERR = 6).
void renderer_init_0037e780(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t video,
                            std::uint32_t width, std::uint32_t height, std::uint32_t color_bits,
                            std::uint32_t depth_bits, std::uint32_t flags, std::uint32_t qwords,
                            std::uint32_t records);

}  // namespace omega
