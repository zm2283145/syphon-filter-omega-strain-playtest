#include "frame_overlay.hpp"
#include "frame_fit.hpp"
#include "gfx_device_lost.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace omega {
namespace {

struct Glyph {
    char c;
    std::array<std::uint8_t, 7> rows;  // bit 4 is the leftmost of the 5 columns
};

// A plain 5 x 7 font: digits, capitals and the punctuation the overlay prints.
constexpr Glyph font[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

const Glyph* glyph(char c) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& g : font)
        if (g.c == u) return &g;
    return nullptr;
}

constexpr unsigned cell_width = 6, cell_height = 9, padding = 3;
constexpr std::uint8_t panel[4]{0x18, 0x10, 0x10, 0xff};  // B, G, R, A: a dark panel
constexpr std::uint8_t ink[4]{0xf0, 0xf8, 0xf8, 0xff};

}  // namespace

bool overlay_font_has(char c) { return glyph(c) != nullptr; }

OverlayImage raster_overlay_text(const std::vector<std::string>& lines, unsigned scale) {
    OverlayImage image;
    if (lines.empty() || scale == 0) return image;
    std::size_t columns = 0;
    for (const auto& line : lines) columns = std::max(columns, line.size());
    if (columns == 0) return image;
    image.width = (static_cast<unsigned>(columns) * cell_width + 2 * padding - 1) * scale;
    image.height = (static_cast<unsigned>(lines.size()) * cell_height + 2 * padding - 2) * scale;
    image.bgra.resize(std::size_t(image.width) * image.height * 4);
    for (std::size_t i = 0; i < image.bgra.size(); i += 4) std::copy(panel, panel + 4, image.bgra.begin() + std::ptrdiff_t(i));
    for (std::size_t l = 0; l < lines.size(); ++l)
        for (std::size_t k = 0; k < lines[l].size(); ++k) {
            const Glyph* g = glyph(lines[l][k]);
            if (!g) continue;
            const unsigned x0 = (padding + static_cast<unsigned>(k) * cell_width) * scale;
            const unsigned y0 = (padding + static_cast<unsigned>(l) * cell_height) * scale;
            for (unsigned row = 0; row < 7; ++row)
                for (unsigned col = 0; col < 5; ++col) {
                    if (!(g->rows[row] & (0x10u >> col))) continue;
                    for (unsigned sy = 0; sy < scale; ++sy)
                        for (unsigned sx = 0; sx < scale; ++sx) {
                            const std::size_t at =
                                (std::size_t(y0 + row * scale + sy) * image.width + x0 + col * scale + sx) * 4;
                            std::copy(ink, ink + 4, image.bgra.begin() + std::ptrdiff_t(at));
                        }
                }
        }
    return image;
}

void mask_frame_bars(gfx::Device& device, const gfx::Texture* target, unsigned width, unsigned height) {
    if (!frame_masked() || !target) return;
    const auto bars = frame_bars(width, height);
    if (bars.count == 0) return;
    gfx::Rect rects[2]{};
    for (unsigned i = 0; i < bars.count; ++i) {
        const auto& r = bars.rects[i];
        rects[i] = {int(r.x), int(r.y), int(r.x + r.width), int(r.y + r.height)};
    }
    const float black[]{0, 0, 0, 1};
    gfx::CommandList list;
    list.clear_rects(target, black, rects, bars.count);
    device.submit(list);
}

void FrameOverlay::release() {
    texture_.reset();
    device_ = nullptr;
    texture_width_ = texture_height_ = 0;
    uploaded_ = false;
}

void FrameOverlay::draw(gfx::Device& device, const gfx::Texture* target, unsigned width, unsigned height,
                        const std::vector<std::string>& lines, unsigned x, unsigned y, unsigned scale) try {
    if (!target || device_lost_pending()) return;
    if (lines != lines_ || scale != scale_) {
        lines_ = lines;
        scale_ = scale;
        image_ = raster_overlay_text(lines_, scale_);
        uploaded_ = false;
    }
    if (image_.width == 0 || x >= width || y >= height) return;
    if (&device != device_ || image_.width > texture_width_ || image_.height > texture_height_) {
        release();
        gfx::TextureDesc d;
        d.width = std::max(image_.width, 256u);
        d.height = std::max(image_.height, 64u);
        d.format = gfx::Format::bgra8;
        d.usage = gfx::Usage::gpu;
        gfx::TextureLabels labels;
        labels.texture = "Create frame overlay texture";
        texture_ = device.create_texture(d, nullptr, labels);
        device_ = &device;
        texture_width_ = d.width;
        texture_height_ = d.height;
    }
    gfx::CommandList list;
    if (!uploaded_) {
        list.update_texture(texture_.get(), {0, 0, 0, image_.width, image_.height, 1}, image_.bgra.data(), image_.width * 4);
        uploaded_ = true;
    }
    const gfx::Box box{0, 0, 0, std::min(image_.width, width - x), std::min(image_.height, height - y), 1};
    list.copy_region(target, 0, x, y, texture_.get(), 0, &box);
    device.submit(list);
} catch (const DeviceLost&) {
}

}  // namespace omega
