#pragma once
// Port-only (checkpoint 132win2, not the original): the PS2 frame's place in a PC window. The game draws a
// 640 x 448 frame that a television shows at 4:3; the PC window may have any aspect (16:9, 16:10, ultrawide,
// a tall window), so the 4:3 frame is fitted whole and centred on whole pixels: pillarboxed (bars left and
// right) in a wider window, letterboxed (bars top and bottom) in a taller one, never stretched. The bars are
// the cleared black of the back buffer. Used by the GUI renderer's frame fit (gui_renderer.hpp): the front
// end, its movies, the loading screen and the play HUD (research/PLAY_SANDBOX.md "Host window and fullscreen").
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace omega {

struct FrameRect {
    unsigned x = 0, y = 0, width = 0, height = 0;
    friend constexpr bool operator==(const FrameRect&, const FrameRect&) = default;
};

inline constexpr unsigned frame_aspect_width = 4, frame_aspect_height = 3;

// The largest centred rectangle of aspect aw : ah inside width x height (rounded to the nearest pixel; the odd
// leftover pixel goes to the right / bottom bar). An empty target answers itself.
constexpr FrameRect fit_frame(unsigned width, unsigned height, unsigned aw = frame_aspect_width,
                              unsigned ah = frame_aspect_height) {
    if (width == 0 || height == 0 || aw == 0 || ah == 0) return {0, 0, width, height};
    const std::uint64_t w = width, h = height;
    if (w * ah > h * aw) {  // wider than the frame: pillarbox
        const auto fw = static_cast<unsigned>((2 * h * aw + ah) / (2 * ah));
        return {(width - fw) / 2, 0, fw, height};
    }
    const auto fh = static_cast<unsigned>((2 * w * ah + aw) / (2 * aw));  // taller (or equal): letterbox
    return {0, (height - fh) / 2, width, fh};
}

// Checkpoint 132win3: the play's 3D framing that matches the fitted HUD. The scene renders the whole window and
// keeps the 4:3 frame's view inside the fit rectangle: in a wider window its vertical field of view (Hor+, more
// scene left and right), in a narrower one its horizontal field of view (Vert-, more scene above and below), so
// the HUD drawn in fit_frame's rectangle always lies over the picture the PS2's 4:3 frame shows.
inline constexpr bool frame_narrower(unsigned width, unsigned height) {
    return width != 0 && height != 0 &&
           std::uint64_t(width) * frame_aspect_height < std::uint64_t(height) * frame_aspect_width;
}

// The scene's vertical half angle for the game camera's horizontal half angle (the 4:3 frame's; 00133190).
inline float frame_vertical_half_fov(float horizontal_half_fov, unsigned width, unsigned height) {
    const float t = std::tan(horizontal_half_fov) * 0.75F;  // the 4:3 frame's vertical half angle
    if (!frame_narrower(width, height)) return std::atan(t);
    return std::atan(t * (4.0F / 3.0F) * static_cast<float>(height) / static_cast<float>(width));
}

// The scale from the 4:3 frame's NDC to the whole window's NDC (the fit rectangle's share of each axis) for
// points the scene projects: the GUI's perspective batches (GuiViewProjection) land where the 3D draws them.
struct FramePerspectiveScale {
    float x = 1.0F, y = 1.0F;
};
inline FramePerspectiveScale frame_perspective_scale(unsigned width, unsigned height);

// Checkpoint 132win10 (port-only): the aspect setting (port settings aspect=, play's --aspect):
//   pillarbox  the default and the behaviour above: the 2D frame (front end, movies, loading, HUD) in the centred
//              4:3 rectangle with black bars, the play's 3D over the whole window (Hor+ / Vert-) matching it.
//   4:3        the PS2 picture only: as pillarbox, then everything outside the 4:3 rectangle painted black
//              (frame_masked: the play's wider 3D view and full-target fades are cut to the 4:3 frame).
//   stretch    the 4:3 frame stretched over the whole window: the 2D frame fills it and the 3D keeps the 4:3
//              view (the game's vertical field of view and a 4:3 projection aspect) over the whole viewport.
//   wide       checkpoint 132ws: true wide screen. The 3D as pillarbox (Hor+); the 2D frame keeps its PS2 scale
//              (the 4:3 frame's height, no horizontal stretch) but is drawn over the whole window: the HUD's
//              elements are anchored to the window's left / right edges (frame_wide_anchor_shift), full-frame
//              backdrops and tints cover the window, the front end's 4:3 pictures get a blurred edge fill in the
//              side areas, and movies / loading screens keep 4:3 with bars (or zoom to fill: movie_fill). A
//              target narrower than 4:3 (or exactly 4:3) is drawn as pillarbox. Platform-neutral: everything
//              here takes the target's width and height (the gfx device's back buffer, any backend).
enum class FrameAspect : unsigned { pillarbox, strict, stretch, wide };
inline FrameAspect& frame_aspect_setting() {
    static FrameAspect mode = FrameAspect::pillarbox;
    return mode;
}
inline const char* frame_aspect_name(FrameAspect a) {
    return a == FrameAspect::strict ? "4:3" : a == FrameAspect::stretch ? "stretch" : a == FrameAspect::wide ? "wide" : "pillarbox";
}

// Checkpoint 132ws (port-only): the wide setting's two companions (port settings hud_margin= / movie_fill=, play's
// --hud-margin / --movie-fill). hud_margin: the HUD's safe-area inset from the window's left and right edges in
// percent of the window's width (0..20, default 0: the elements keep their PS2 distance from the frame's edge, now
// measured from the window's edge). movie_fill zoom: loading / briefing screens scale up until they fill the
// window's width (the top and bottom of the 4:3 picture are cut); bars (the default) keeps them 4:3.
inline constexpr unsigned frame_hud_margin_max = 20;
inline unsigned& frame_hud_margin_setting() {
    static unsigned percent = 0;
    return percent;
}
inline bool& frame_movie_zoom_setting() {
    static bool zoom = false;
    return zoom;
}
// Port-only (132vt2a2, port settings movies=, play's --movies): how story/intro movies
// (GuiDrawBatch::port_movie) meet a wide target. fill (the default): the 4:3 picture scaled to the target's width keeping its PS2 display aspect, including
// the 448-to-480-line pixel-aspect correction, with the top and bottom cut equally (the disc's movies mostly carry
// letterbox bars of their own); bars: the native 4:3 picture with side bars;
// stretch: the 4:3 picture stretched over the whole target (anamorphic). Only under wide (frame_wide): the other
// aspects draw the movies as they draw the frame. Attract demo videos follow this same choice. The
// loading screen keeps movie_fill.
enum class MovieFit : unsigned { fill, bars, stretch };
inline MovieFit& frame_movie_fit_setting() {
    static MovieFit fit = MovieFit::fill;
    return fit;
}
inline const char* frame_movie_fit_name(MovieFit f) {
    return f == MovieFit::bars ? "bars" : f == MovieFit::stretch ? "stretch" : "fill";
}

// Whether the wide drawing applies to a width x height target: the wide setting and a target wider than 4:3.
constexpr bool frame_wide(unsigned width, unsigned height, FrameAspect a) {
    return a == FrameAspect::wide && width != 0 && height != 0 &&
           std::uint64_t(width) * frame_aspect_height > std::uint64_t(height) * frame_aspect_width;
}
inline bool frame_wide(unsigned width, unsigned height) { return frame_wide(width, height, frame_aspect_setting()); }
// The window's half width in GUI units (the 640 x 448 frame's units at its height: 224 per half height); 320 when
// not wider than 4:3. The wide draw maps GUI x to the window's NDC as x / frame_wide_half_width.
inline float frame_wide_half_width(unsigned width, unsigned height) {
    if (width == 0 || height == 0) return 320.0F;
    const float hw = 240.0F * static_cast<float>(width) / static_cast<float>(height);
    return hw > 320.0F ? hw : 320.0F;
}
// The x shift (GUI units) of a HUD element anchored to the window's left (side < 0) or right (side > 0) edge: the
// window's extra half width past the 4:3 frame less the safe-area margin, never inwards; 0 for side 0.
inline float frame_wide_anchor_shift(int side, unsigned width, unsigned height, unsigned margin_percent) {
    if (side == 0) return 0.0F;
    const float hw = frame_wide_half_width(width, height);
    const float margin = 2.0F * hw * static_cast<float>(margin_percent > frame_hud_margin_max ? frame_hud_margin_max
                                                                                              : margin_percent) / 100.0F;
    const float e = hw - 320.0F - margin;
    const float s = e > 0.0F ? e : 0.0F;
    return side < 0 ? -s : s;
}
// The side a HUD element anchors to from its bounds' centre x (GUI units): left of -frame_wide_anchor_band, right
// of +frame_wide_anchor_band, else the centre (it stays with the 4:3 frame).
inline constexpr float frame_wide_anchor_band = 80.0F;
constexpr int frame_wide_anchor_side(float centre_x) {
    return centre_x < -frame_wide_anchor_band ? -1 : centre_x > frame_wide_anchor_band ? 1 : 0;
}

// The 2D frame's rectangle in a width x height target under an aspect setting.
constexpr FrameRect frame_rect(unsigned width, unsigned height, FrameAspect a) {
    return a == FrameAspect::stretch ? FrameRect{0, 0, width, height} : fit_frame(width, height);
}
inline FrameRect frame_rect(unsigned width, unsigned height) { return frame_rect(width, height, frame_aspect_setting()); }
// Whether the bars outside frame_rect are painted black after the frame is drawn (the 4:3 setting).
inline bool frame_masked() { return frame_aspect_setting() == FrameAspect::strict; }
// The bars outside frame_rect (up to two rectangles: left / right or top / bottom; empty ones dropped).
struct FrameBars {
    FrameRect rects[2]{};
    unsigned count = 0;
};
constexpr FrameBars frame_bars(unsigned width, unsigned height) {
    FrameBars b;
    const FrameRect r = fit_frame(width, height);
    if (r.width == width && r.height == height) return b;
    if (r.width < width) {
        if (r.x > 0) b.rects[b.count++] = {0, 0, r.x, height};
        if (r.x + r.width < width) b.rects[b.count++] = {r.x + r.width, 0, width - r.x - r.width, height};
    } else {
        if (r.y > 0) b.rects[b.count++] = {0, 0, width, r.y};
        if (r.y + r.height < height) b.rects[b.count++] = {0, r.y + r.height, width, height - r.y - r.height};
    }
    return b;
}

// The 3D scene's vertical half angle under an aspect setting (stretch: the 4:3 frame's own).
inline float frame_vertical_half_fov(float horizontal_half_fov, unsigned width, unsigned height, FrameAspect a) {
    if (a == FrameAspect::stretch) return std::atan(std::tan(horizontal_half_fov) * 0.75F);
    return frame_vertical_half_fov(horizontal_half_fov, width, height);
}
// The 3D projection's aspect (x over y): the target's, or the 4:3 frame's when stretched.
inline float frame_projection_aspect(unsigned width, unsigned height, FrameAspect a) {
    if (a == FrameAspect::stretch || width == 0 || height == 0) return 4.0F / 3.0F;
    return static_cast<float>(width) / static_cast<float>(height);
}
inline float frame_projection_aspect(unsigned width, unsigned height) {
    return frame_projection_aspect(width, height, frame_aspect_setting());
}
inline FramePerspectiveScale frame_perspective_scale(unsigned width, unsigned height, FrameAspect a) {
    if (width == 0 || height == 0 || a == FrameAspect::stretch) return {};
    if (frame_narrower(width, height))
        return {1.0F, 0.75F * static_cast<float>(width) / static_cast<float>(height)};
    return {(4.0F / 3.0F) * static_cast<float>(height) / static_cast<float>(width), 1.0F};
}
inline FramePerspectiveScale frame_perspective_scale(unsigned width, unsigned height) {
    return frame_perspective_scale(width, height, frame_aspect_setting());
}
// Checkpoint 132ws (port-only): the factor on a 4:3 camera clip matrix's x row that widens it to the Hor+ view of
// a wider target under the wide setting (the draw-only culls: what is visible at the window's left and right
// edges is filed; the game's own +38 codes keep the 4:3 clip); 1 otherwise, so the other settings are unchanged.
inline float frame_wide_cull_x_scale(unsigned width, unsigned height, FrameAspect a) {
    return frame_wide(width, height, a) ? frame_perspective_scale(width, height, a).x : 1.0F;
}
inline float frame_wide_cull_x_scale(unsigned width, unsigned height) {
    return frame_wide_cull_x_scale(width, height, frame_aspect_setting());
}

// Checkpoint 132ws3 (port-only): the field of view (port settings fov=, play's --fov) in percent of the original's
// view tangent (70..150, default 100 = the original): the drawn view's half widths (tan of the half angles, both
// axes) scale by percent / 100, so 120 shows 20% more each way and every camera (the scope's zoom, cutscenes) keeps
// its ratio to the original. Draw only, like Hor+: the 3D projection, the draw-side culls and the HUD's 3D-to-2D
// projections follow it; the game's camera, its +38 visibility codes, LOD, AI and aim do not. Applies under the wide
// setting on a target wider than 4:3 only (frame_wide), so pillarbox, 4:3 and stretch keep the original view.
inline constexpr unsigned frame_fov_min = 70, frame_fov_max = 150, frame_fov_original = 100;
inline unsigned& frame_fov_setting() {
    static unsigned percent = frame_fov_original;
    return percent;
}
// The factor on the view's tangents (1 = the original view).
inline float frame_fov_scale(unsigned width, unsigned height, FrameAspect a, unsigned percent) {
    if (!frame_wide(width, height, a) || percent == frame_fov_original) return 1.0F;
    const unsigned p = percent < frame_fov_min ? frame_fov_min : percent > frame_fov_max ? frame_fov_max : percent;
    return static_cast<float>(p) / 100.0F;
}
inline float frame_fov_scale(unsigned width, unsigned height) {
    return frame_fov_scale(width, height, frame_aspect_setting(), frame_fov_setting());
}
// A camera half angle widened by the field of view: atan(tan(half) * scale); the angle itself at scale 1.
inline float frame_fov_half_angle(float half_angle, float scale) {
    return scale == 1.0F ? half_angle : std::atan(std::tan(half_angle) * scale);
}

// Checkpoint 132win10 (port-only): the internal render scale (port settings render_scale=, --render-scale): the
// swap chain's buffers are the client size times the scale (DXGI stretches them to the window), 1 by default.
inline constexpr float render_scale_min = 0.25F, render_scale_max = 4.0F;
inline constexpr unsigned render_buffer_limit = 16384;  // D3D11's texture limit
inline unsigned scaled_extent(unsigned client, float scale) {
    if (client == 0) return 0;
    const double v = std::floor(double(client) * double(scale) + 0.5);
    return v < 1.0 ? 1U : v > double(render_buffer_limit) ? render_buffer_limit : static_cast<unsigned>(v);
}

// Issue #4: a native-resolution target keeps the original 640 x 448 buffer's 4:3 picture density while matching
// the client aspect so the swap chain's final stretch does not distort it. The native 640 x 448 target contains a
// 597 x 448 fitted 4:3 picture. Wider clients keep 448 rows and add side pixels; narrower clients keep 597 columns
// and add top/bottom pixels. Thus fit_frame() is always the same native 597 x 448 picture.
inline std::pair<unsigned, unsigned> native_buffer_size(unsigned client_width, unsigned client_height) {
    if (client_width == 0 || client_height == 0) return {client_width, client_height};
    constexpr unsigned picture_width = 597, picture_height = 448;
    const std::uint64_t w = client_width, h = client_height;
    if (w * frame_aspect_height >= h * frame_aspect_width) {
        const std::uint64_t native_width = (std::uint64_t(picture_height) * w + h / 2) / h;
        return {static_cast<unsigned>(std::min<std::uint64_t>(native_width, render_buffer_limit)), picture_height};
    }
    const std::uint64_t native_height = (std::uint64_t(picture_width) * h + w / 2) / w;
    return {picture_width, static_cast<unsigned>(std::min<std::uint64_t>(native_height, render_buffer_limit))};
}

}  // namespace omega
