#pragma once
// Port-only (checkpoint 132win10, not the original): the frame-time overlay and the 4:3 bar mask.
//
// FrameOverlay draws a few lines of text (the play loop's sim / script / render / present ms from FrameProfile's
// rolling averages, frame_profile.hpp) in the top-left corner of the back buffer, so a player can report the
// port's performance from real play. It is off by default (port settings frame_overlay=1, play's --frame-overlay,
// the F3 key in play). The text is rasterised on the CPU with a built-in 5 x 7 font into a BGRA image, uploaded to
// a texture and copied into the back buffer (a copy_region): no shader, blend or pipeline state is touched,
// so the frame under it is drawn exactly as without it. Its texture is re-made after a device loss (release()).
// Checkpoint 132mt1: both go through the gfx layer (gfx_device.hpp).
//
// mask_frame_bars paints the bars outside the fitted 4:3 frame black (frame_fit.hpp frame_bars) for the 4:3
// aspect setting (frame_masked); the play calls it before its capture / present, HostWindow before the front end's.
#include "gfx_device.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omega {

// Paints the bars outside the 4:3 frame of a width x height target black (no-op unless frame_masked()).
void mask_frame_bars(gfx::Device& device, const gfx::Texture* target, unsigned width, unsigned height);

// The overlay's text image: BGRA rows (width * 4 bytes each), the lines in the built-in font at `scale` pixels per
// font pixel on an opaque dark panel. Characters outside the font (lower case is drawn upper case) draw blank.
struct OverlayImage {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> bgra;
};
OverlayImage raster_overlay_text(const std::vector<std::string>& lines, unsigned scale);
// Whether the built-in font has a glyph for c (after upper-casing).
bool overlay_font_has(char c);

class FrameOverlay {
public:
    // Copies the lines' image into target's resource at (x, y) (clipped to width x height). The image is
    // re-rasterised only when the text or scale changes.
    void draw(gfx::Device& device, const gfx::Texture* target, unsigned width, unsigned height,
              const std::vector<std::string>& lines, unsigned x, unsigned y, unsigned scale);
    // Drops the texture (before a device re-creation); the next draw makes a new one on the new device.
    void release();
    // The font pixel scale for a target height (1 up to 540 lines, 2 up to 1080, ...).
    static unsigned scale_for(unsigned target_height) { return target_height < 540 ? 1u : target_height / 540u; }

private:
    std::shared_ptr<gfx::Texture> texture_;
    const gfx::Device* device_ = nullptr;  // the texture's device (not owned; compared only)
    unsigned texture_width_ = 0, texture_height_ = 0;
    std::vector<std::string> lines_;
    unsigned scale_ = 0;
    OverlayImage image_;
    bool uploaded_ = false;
};

}  // namespace omega
