#pragma once
// Checkpoint 121k: the HUD's IE screen draw 003e7670(screen), as the HUD draw
// 00244b90 calls it with *(hud + 0x54) (0x00244cb8, the path without the
// open map; 0x00244c90 with it). The screen's root draws through the IE child
// draw 003ed2e0 (ie_draw, Checkpoint 122b), so the reticle elements the
// crosshair switch shows (fptarget's +68, the +84 reticles) are drawn here.
//
// 003e7670: nothing when byte [00493948] is 0 (the screen-draw switch: 1 at
// boot, written only by 003e75e4 / 003e7614 and the data words 004d7908..
// 004d7968); otherwise V+128 = 0, V+10e8 = [004bc1b0], the matrix push
// (V+a80 + 64 * (V+a74 + 1) = the top, V+a74 + 1), the new top set to the
// identity (vf0 and its shuffles) times the rows (s, 0, 0, 0), (0, 0, s, 0),
// (0, 1, 0, 0), (0, 0, z, 1) (sp+40..+7c, s = [00493950], z = [00493958],
// both 1.0 at boot; VU0 vmula/vmadd at 003e7900, as the interpreter's macro
// mode: binary32 products and sums, denormal results flushed to signed zero), V+a78 = 1, the 2D camera
// record (V+1f0 + 0x110 * V+a70: +0..+c = V+f80, +f0 = 0.0, +f8 = 1000.0,
// +104 = +106 = 1; restored after) and 003ed2e0(*screen); then V+10e0 is
// restored, V+a78 = 1 and V+a74 - 1. Of the camera record only +104 / +106
// are modelled (IeDrawState::record_104 / record_106, which a +61 node's
// perspective switch reads, Checkpoint 132r); its viewport and ortho near /
// far and V+128 / V+10e8 are the host 2D mapping (gui_renderer).
// Goldens: tools/ee/gen_hud_screen_draw.py (tests/hud_screen_draw_goldens.hpp).
#include "ie_draw.hpp"
#include "ie_screen.hpp"

#include <cstdint>
#include <functional>

namespace omega {

struct HudScreenDrawGlobals {
    std::uint8_t enabled_493948 = 1;   // byte 00493948
    float scale_493950 = 1.0F;         // 00493950
    float depth_493958 = 1.0F;         // 00493958
};

// The top 003e7670 pushes (row-major as IeDrawState stores it).
std::array<float, 16> hud_screen_draw_matrix(const HudScreenDrawGlobals& g);

// 003e7670 with the child draw 003ed2e0 supplied by the caller (the goldens hook it).
bool hud_screen_draw_003e7670(IeDrawState& st, const HudScreenDrawGlobals& g, const std::function<void()>& child_003ed2e0);

// 003e7670(screen): ie_draw_child(screen.root()) under the pushed matrix.
// Returns false (nothing drawn, state unchanged) when [00493948] is 0.
bool hud_screen_draw_003e7670(GuiDrawList& out, IeDrawState& st, IeScreen& screen, const HudScreenDrawGlobals& g);

}  // namespace omega
