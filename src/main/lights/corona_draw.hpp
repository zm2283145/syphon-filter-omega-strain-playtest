#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Port of 003781b0, the light corona / lens-flare sprite draw on the video
// object (005381f0 in the game), with its callees: 00379ac0/00379b00/
// 00379ab0 (current camera record), 00374df0 (record refresh: viewport
// scale/offset +90..+ac and the scissor +108), 003754a0 (perspective
// +50..+ec), 0037d9a0 (the +ec0/+f00 view-projection and the +1b0/+1c0 depth
// references, when +a78 asks for it), 00379e20 (clip code), 003798a0 (the
// render-state restore packet), 003797e0/00379840 (TEST and PRMODE words),
// 003bb620 (TEX1), 00375c30/00375ba0/00375ac0 (packet allocation on the DMA
// object 00537f80, channel 1) and the pure helpers 001d3e20, 00133300,
// 0023e0a0, 0023e150, 00132000, 00183350, 00190e50, 00121a10 (tanf),
// 00379760, 00379700..00379740, 003796c0, 003796d0, 00379780 and
// 003797b0/003797d0/00379790.
//
// The packet is written to EE memory exactly where the original writes it, so
// a caller sees every GS register value as a memory effect. 00379e20 also
// stores the clip-space point into VU0 data memory 11004000 (VSQI through
// VI00); that store is reproduced as the same memory effect.
//
// Arguments are the EE registers: a0 renderer, a1 position (vec4), a2 colours
// (vec4 per element), a3 texture ids (int per element), t0 offsets and t1
// scales (float per element), t2 count, t3 glare flag (byte), $f12 half size.
// The byte at the caller's $sp (read as 3a0($sp)) enables the full-screen
// glare sprite; it is passed as `glare_sprite`.
//
// Host boundaries: 00382540 (texture slot upload of the +1058 manager, after
// the ids are queued at +1c). The orthographic record refresh 00375390 is
// ported (checkpoint 132cp11: the movie subtitle camera).
struct CoronaDrawCall {
    std::uint32_t renderer{};
    std::uint32_t position{};
    std::uint32_t colours{};
    std::uint32_t ids{};
    std::uint32_t offsets{};
    std::uint32_t scales{};
    std::uint32_t count{};
    std::uint8_t glare{};
    float half_size{};
    std::uint8_t glare_sprite{};
};
void corona_draw_003781b0(LevelLoadMemory& memory, LevelLoadHost& host, const CoronaDrawCall& call);

void camera_record_refresh_00374df0(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t record);
void camera_perspective_003754a0(LevelLoadMemory& memory, std::uint32_t record);
void camera_orthographic_00375390(LevelLoadMemory& memory, std::uint32_t record);
void renderer_view_projection_0037d9a0(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t renderer);
void renderer_state_packet_003798a0(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t renderer);

}  // namespace omega
