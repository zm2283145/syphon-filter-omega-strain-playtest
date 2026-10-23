#pragma once
// Checkpoint 132cp3 / 132cp4: the vision effects manager cVisionFxMan (*004ffc30 = G+e0, constructor 00260b90).
// Mode 0 carries the flair FLAIR_DAZZLE (1), the full-screen white-out of the flash grenade (00186830), which the
// scene frame draws additively over the world (0025f930 at 0013ea14). Checkpoint 132cp4 adds the goggles' modes 1
// (night vision) and 2 (thermal): the texture animation and scan band of the update 002604c0 (00260818..00260924),
// the scene frame's prepare 0025fbb0 (the ambient light and a point light over the player, 00373be0 / 00373e70,
// the frame-buffer write mask, the renderer flags) and the draw 0025f930 (restore / release, the quad, then the
// noise tiles 0025ec20, the scan band 0025e750 and the frame blur 0037b770). Goldens: tools/ee/gen_vision_fx.py and
// tools/ee/gen_vision_modes.py. Modes 3 / 4 (x-ray, motion; no code sets them) run the shared tail only.
#include "level_lights.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace omega {

// The flair's +c..+20 and the vision fade's +30..+44 have one shape: a delay, then in / hold / out.
struct VisionFxTimer {
    bool started{};      // +c  / +30
    bool done{};         // +d  / +31
    float elapsed{};     // +10 / +34
    float delay{};       // +14 / +38
    float in{};          // +18 / +3c
    float hold{};        // +1c / +40
    float out{};         // +20 / +44
    bool operator==(const VisionFxTimer&) const = default;
};

// The constructor's per-mode block (+50 + 0x40 * mode, 00260c10..00260c4c, then 00260cd4..00260d44 for modes 1 / 2).
struct VisionModeBlock {
    std::array<float, 4> ambient{0.2F, 0.2F, 0.2F, 0.0F};   // +0: the ambient light's colour while the mode draws
    std::array<float, 4> point{0.8F, 0.8F, 0.8F, 0.8F};     // +10: the player point light's colour
    float inner_20{};                                        // +20: its inner distance
    float outer_24{};                                        // +24: its range, times the fade +2c
    float tint_28{};                                         // +28: the quad level +24 = +28 * +2c
    std::uint8_t aux8_2c{};                                  // +2c: light +1d |= 8
    std::uint8_t aux10_2d{};                                 // +2d: light +1d |= 0x10
    float scale_30{0.98F};                                   // +30: the quad colour 255 * +30 (0x3f7ae148)
    bool operator==(const VisionModeBlock&) const = default;
};
std::array<VisionModeBlock, 5> vision_fx_default_blocks();   // 00260b90

struct VisionFx {
    std::int32_t mode_00{};        // cVisionFxMan::eMode: 0 normal, 1 night, 2 thermal, 3 x-ray, 4 motion
    std::int32_t pending_04{};     // the mode to commit, -1 while the fade-out to mode 0 runs
    std::uint8_t flair_08{};       // cVisionFxMan::eFlair: 0 none, 1 dazzle, 2 / 3 motion blurs
    VisionFxTimer flair;           // +c..+20
    float level_24{};              // 0025fbb0: +78 [mode] * +2c
    float level_28{};              // 0025fbb0: the dazzle level
    float fade_2c{1.0F};           // 002604c0: the vision fade level
    VisionFxTimer fade;            // +30..+44
    std::array<VisionModeBlock, 5> blocks{vision_fx_default_blocks()};   // +50..+18f
    std::optional<std::size_t> ambient_190;   // the ambient pool entry 00373be0 gave (prepare to draw)
    std::optional<std::size_t> point_194;     // the point pool entry 00373e70(mgr, 4) gave (prepare to draw)
    float band_198{};              // the scan band's top (00260818's tail)
    std::int32_t noise_19c{-1};    // the noise texture's handle (00260a50: effect table entry 0x21)
    float noise_w_1a0{}, noise_h_1a4{};   // the noise tile size: the texture's header +4 / +6
    float light_1a8{1.0F};         // 002604c0: 0.65 * +2c (the noise alpha)
    std::uint32_t frame_1ac{};     // the noise animation frame
    float frame_time_1b0{};        // its clock
    float frame_period_1b4{0.1F};  // its period (0x3dcccccd)
    std::array<float, 4> saved_ambient_1c0{};   // the ambient colour prepare replaced
    std::uint32_t saved_1d0{};     // the renderer flags word [005392d0] at prepare
    // 00260a50's handles (not written by the constructor): +1d4..+1f4 the effect table entries 0x1b, 0x1e, 0x1c,
    // 0x20, 0x1d, 0x1f, 0x1f, 0x1d, 0x1d (read only by the GOB glow 0025f270, gob_glow.hpp), +1f8 entry 0x21.
    std::array<std::int32_t, 9> flares_1d4{};
    std::int32_t noise_1f8{};
    bool operator==(const VisionFx&) const = default;
};

// The renderer globals 0025fbb0 / 0025f930 write.
struct VisionRenderState {
    std::uint32_t flags_5392d0{};         // renderer flags: bits 0x60 cleared in modes 1 / 2 between prepare and draw
    std::uint32_t fbmsk_5382e8{0xff000000U};   // FBMSK, ORed into the FRAME_1 / FRAME_2 images 00538280 / 00538288
    std::uint8_t byte_5382c0{};           // 1 from prepare (modes 0 / 1 / 2) and draw
    std::uint32_t word_538314{};          // 1 from draw
    std::uint32_t night_53b350{};         // mode 1 (Skm_Draw's night ghosts 0038804c); prepare mode 0 clears it
    std::uint32_t thermal_53b358{};       // mode 2 (Skm_Draw's thermal rows 00387c00); mode 0 clears
    std::uint8_t lights_535d62{};         // the light manager's dirty byte (00144a60 / 00144b80 / 00144980)
    std::uint8_t gather_535d63{};         // the light gather's vision byte (00372120): 0 from prepare, 1 from draw
    bool operator==(const VisionRenderState&) const = default;
};

// The noise texture object *(*(00539248)+8)[+19c]: header (+44) +4 / +6 size and +22 frames, the current frame +4c
// and the upload bytes +60 / +61.
struct VisionNoiseTexture {
    std::uint16_t width_04{}, height_06{}, frames_22{1};
    std::uint32_t frame_4c{};
    std::uint8_t byte_60{}, byte_61{};
    bool operator==(const VisionNoiseTexture&) const = default;
};

// 00260a50's effect table indices (bytes of 004a7db0, stride 4) and the noise entry.
inline constexpr std::array<std::uint8_t, 9> vision_fx_flare_indices{0x1b, 0x1e, 0x1c, 0x20, 0x1d, 0x1f, 0x1f, 0x1d, 0x1d};
inline constexpr std::uint8_t vision_fx_noise_index = 0x21;
// 00260a50(fx) from the level load (001c1e5c): the ids of 00236f20(index) (a null entry would fault: the caller
// throws), then +1f8 = +19c = the noise entry's id, +1ac = 0 and the tile size from the noise texture's header.
void vision_fx_set_noise(VisionFx& fx, const std::array<std::int32_t, 9>& flare_ids, std::int32_t noise_id,
                         const VisionNoiseTexture& noise);

// 00260990(fx, flair, f12 delay, f13 in, f14 hold, f15 out): the flair setter. Flair 0 also clears +28.
void vision_fx_set_flair(VisionFx& fx, std::uint8_t flair, float delay, float in, float hold, float out);
// 002609e0(fx, mode, force): the pending mode (and with force the mode itself); a new mode after a fade-out
// (pending -1) restarts the fade with in = 1.0.
void vision_fx_set_mode(VisionFx& fx, std::int32_t mode, bool force);
// 002604c0(fx, dt) from the world update at 001337dc (after the HUD update 00244fa0): the fade and flair timers,
// the mode commit (fades 00260950 over 00260970), +2c / +1a8 and the end of a finished flair. Modes 1 / 2 go on
// (00260818) to the noise animation (+1b0 / +1ac against `noise`, which must be given: the original reads the
// texture object unconditionally) and the scan band +198 (down by dt * h, h = 0.5 * the viewport record's height
// V+1fc, 005381f0 + 0x110 * [00538c60]; back to h below -h).
void vision_fx_update(VisionFx& fx, float dt, VisionNoiseTexture* noise = nullptr, float viewport_h_1fc = 448.0F);

// What 0025fbb0 reads from the local player's actor (*004ffc0c +30): vcall +f4 (00132280, the 'flor' root world
// matrix, rows r0..r3 as root_placement_matrix gives them) and vcall +e0 (00133e00, the body centre). 002602e0..
// 00260308 replace the matrix's row 3 with the +e0 vector before the transform.
using VisionPlayerMatrix = std::array<std::array<float, 4>, 4>;
struct VisionPlayer {
    VisionPlayerMatrix matrix_f4{};
    ShadowVector position_e0{};
};
// 0025fbb0(fx) at the start of the scene frame (0013e818): +1d0 = [005392d0]; mode 0: 005382c0 = 1, FBMSK
// 0xff000000, +24 = 0, the dazzle level +28 while the flair is 1, 0053b350 = 0053b358 = 0; modes 1 / 2:
// 00535d63 = 0, +190 = the ambient pool's entry 0 (00373be0), +194 = the first point entry with id -1 (00373e70,
// which writes its id = index + 1) set up as a dynamic point light (vtable 004d9200, aux 3, type 4), [005392d0] = +1d0 &
// ~0x60, FBMSK 0xffff00ff (mode 1: green only) / 0xff00ffff (mode 2: blue only), 005382c0 = 1, the dazzle level,
// 0053b350 (mode 1) / 0053b358 (mode 2) = 1. Then (all modes, 002601e8) the block of the mode: the ambient light's
// colour saved to +1c0 and replaced, the point light at r1 * 5 + the body centre (vcall +28 00144b10) with the
// block's colour / inner / range * +2c and enabled (00144b80), and +24 = +28 * +2c. `lights` may be null only when
// no light is taken (mode 0 or 3 / 4); `player` must be given when a point light is taken.
void vision_fx_prepare(VisionFx& fx, VisionRenderState& render, LevelLights* lights,
                       const VisionPlayer* player);
// Mode-0 shorthand (the teardown and the goldens of gen_vision_fx.py): no lights, the render state discarded.
void vision_fx_prepare(VisionFx& fx);

// 0025f930's full-screen quad: when +24 + +28 > 0, level = a + w * (1 - a) (a = +24, w = +28),
// 0037a1c0(video, {R, G, B, A}, 0, 2) with R = G = B = cvt.w.s(255 * +80 [mode]) and A = cvt.w.s(128 * level),
// drawn by 0037a370 in slot 2 (ALPHA 0x48: Cd + (Cs * As >> 7), COLCLAMP), under the mode's FBMSK.
struct VisionFxQuad {
    std::int32_t r{}, g{}, b{}, a{};
    bool operator==(const VisionFxQuad&) const = default;
};
std::optional<VisionFxQuad> vision_fx_quad(const VisionFx& fx);

// 0025f930(fx) (0013ea10): the ambient colour restored from +1c0 (weight 1 / |c|^2), the point light released
// (vcall +8(1) = 00144a60: id -1, disabled, vtable 004d9280), the quad, then [005392d0] = +1d0, FBMSK 0xff000000,
// 005382c0 = 1, 00538314 = 1; modes 1 / 2: 00535d63 = 1 and the passes below (after the masks are restored).
struct VisionFxDraw {
    std::optional<VisionFxQuad> quad;
    std::uint32_t fbmsk{0xff000000U};   // the mask the quad (and the scene before it) was drawn under
    bool layers{};                      // modes 1 / 2: 0025ec20, 0025e750, 0037b770
    // 0025ec20: the noise texture +19c at its current frame (texture object +4c, which the update advances with
    // +1ac) in w x h tiles (+1a0 x +1a4) from (-W/2, H/2) right and down
    // while x < W/2 / y > -H/2 (W x H = the viewport record's +1f8 x +1fc, 640 x 448), corners (x, y) and
    // (x + w, y - h), UVs 2048 + (0, 0) / 2048 + (1, 1) (0025f220 returns 2048: the wrapped unit square), vertex
    // colour (1, 1, 1, +1a8) (1 = 0x80), GUI mode word 005392d0 = 5 and ALPHA 005392d8 = the 004bc1b0 slot 0 word
    // 0x44 ((Cs - Cd) * As + Cd, the normal blend), flushed by 00370550.
    std::int32_t noise_texture{-1};
    float noise_w{}, noise_h{};
    float noise_alpha{};
    // 0025e750: an untextured band, colour (0, 0, 0, 0.14) (0x3e0f5c29), from (-W/2, +198) to (W/2, +198 + 4),
    // ALPHA 0x44 ((Cs - Cd) * As + Cd; 005381f8 = 2 << 32 before the flush).
    float band_y0{}, band_y1{};
    float band_alpha{};
    // 0037b770 (always in modes 1 / 2, no fields): 0037bd40(video, 0) copies the frame buffer (+12c[+140], TEX0
    // 1024 x 1024, TCC 0, DECAL, TEX1 0x60 bilinear, CLAMP 5) with one context-2 sprite (PRIM 0x356, RGBAQ
    // 0x80808080, TEST_2 0x31001: colour only, ALPHA slot 0 with As = 1) into ScreenFxBuf (video +10cc, 256 x 256);
    // then 0037b810 draws ScreenFxBuf back over the whole screen in context 1 with the same settings (PRIM
    // 0x156): a bilinear downsample to 256 x 256 and a bilinear upsample, drawn opaque.
};
VisionFxDraw vision_fx_draw(VisionFx& fx, VisionRenderState& render, LevelLights* lights);

}