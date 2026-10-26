#pragma once
// Port-only display settings (checkpoint 132win10, not the original: the PS2 game draws a 640 x 448 frame for a
// 4:3 television). Port settings keys (port_settings.hpp keeps them as unknown keys; display_settings reads them):
//   aspect=pillarbox|4:3|stretch|wide  how the 4:3 frame meets the window (frame_fit.hpp FrameAspect; default pillarbox,
//                                  the faithful 4:3 frame with black bars and the play's 3D over the whole window;
//                                  wide (132ws): true wide screen, the 2D frame over the whole window at its PS2 scale)
//   hud_margin=<0..20>             132ws: under wide, the HUD's safe-area inset from the window's left / right edges in
//                                  percent of the window's width (default 0)
//   fov=<70..150>                  132ws3: under wide, the drawn field of view in percent of the original's view tangent
//                                  (frame_fit.hpp frame_fov_scale; default 100, the original; draw only)
//   movie_fill=bars|zoom           132ws: under wide, the loading screen 4:3 with bars (default) or zoomed to fill the
//                                  window's width (the movies: movies=)
//   movies=fill|bars|stretch       132vt2a2: under wide, the movies scaled to the window's width keeping their aspect
//                                  with the top and bottom cut (fill, the default), 4:3 with bars, or stretched over
//                                  the window (frame_fit.hpp MovieFit)
//   render_scale=<0.25..4>         the internal render scale: the swap chain's buffers are the window's client size
//                                  times it (DXGI scales them to the window); default 1 (native). Below 1 renders
//                                  fewer pixels (faster, softer), above 1 supersamples.
//                                  132vp4, the PS Vita: the display is fixed at 960 x 544, so only the 3D scene is
//                                  scaled (vita_render_scale: 1, 0.75 or 0.5; the Port Settings page's 100 / 75 / 50 %),
//                                  drawn at that size and upscaled before the HUD / GUI (research/VITA.md "Render scale")
//   frame_overlay=0|1              the frame-time overlay in play (frame_overlay.hpp; default 0, off)
//   frame_overlay_key=<key>|none   its toggle key in play (input_bindings.hpp key names; default F3)
//   render_thread=0|1              checkpoint 132mt2: the play's GPU submission and present on a render thread, one
//                                  frame behind the simulation (gfx_render_thread.hpp; default 1); 0 runs the old
//                                  single-threaded order (the --render-thread 0 flag too)
//   worker_threads=auto|<0..64>    checkpoint 132mt3: worker threads for the CPU draw preparation (character skinning,
//                                  the particle programs; worker_pool.hpp, draw_prepare.hpp), merged in the original
//                                  order; default auto (hardware threads - 1, at most 7); 0 runs the old
//                                  single-threaded path (the --worker-threads 0 flag too)
// None of these changes the game's state or timing: they are the presentation of the frame only.

#include "frame_fit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace omega {

struct PortSettings;

struct DisplaySettings {
    FrameAspect aspect = FrameAspect::pillarbox;
    unsigned hud_margin = 0;  // 132ws: percent
    unsigned fov = frame_fov_original;  // 132ws3: percent
    bool movie_zoom = false;  // 132ws: movie_fill=zoom
    MovieFit movie_fit = MovieFit::fill;  // 132vt2a2: movies=
    float render_scale = 1.0F;
    bool frame_overlay = false;
    std::optional<std::uint8_t> frame_overlay_key = std::uint8_t{0x72};  // VK_F3; nullopt: no key
    bool render_thread = true;  // 132mt2
    int worker_threads = -1;    // 132mt3: -1 auto (WorkerPool::default_workers)
};

// Throws on a malformed value (the message names the key).
DisplaySettings display_settings(const PortSettings& settings);
// "pillarbox", "4:3" (or "4x3", "strict"), "stretch" or "wide" (or "16:9", "16x9"); nullopt otherwise.
std::optional<FrameAspect> parse_frame_aspect(std::string_view text);
// 132ws: a whole percent 0..frame_hud_margin_max (hud_margin=); nullopt otherwise.
std::optional<unsigned> parse_hud_margin(std::string_view text);
// 132ws3: a whole percent frame_fov_min..frame_fov_max (fov=); nullopt otherwise.
std::optional<unsigned> parse_fov(std::string_view text);
// 132ws: "bars" (false) or "zoom" (true) (movie_fill=); nullopt otherwise.
std::optional<bool> parse_movie_fill(std::string_view text);
// 132vt2a2: "fill", "bars" or "stretch" (movies=); nullopt otherwise.
std::optional<MovieFit> parse_movie_fit(std::string_view text);
// A render scale in [render_scale_min, render_scale_max]; nullopt otherwise.
std::optional<float> parse_render_scale(std::string_view text);
// 132vp4: the PS Vita's scene scale for a render scale: 1 (100 %) from 0.875 up, 0.75 (75 %) from 0.625, else 0.5.
float vita_render_scale(float render_scale);

}  // namespace omega
