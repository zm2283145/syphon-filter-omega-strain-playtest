#include "movie_subtitle_overlay.hpp"

#include "corona_draw.hpp"
#include "level_load.hpp"
#include "video_frame.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace omega::movie {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u32 video = 0x5381f0;

std::string hex(u32 v) {
    char t[12];
    std::snprintf(t, sizeof t, "%08x", v);
    return t;
}
[[noreturn]] void fail(u32 address, const std::string& what) {
    throw std::runtime_error("movie subtitles " + hex(address) + ": " + what);
}

float ftz(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float neg(float a) { return -a; }
// div.s as the EE interpreter evaluates it (corona_draw.cpp dvd).
float dvd(float a, float b) {
    if (b == 0)
        return std::copysign(3.4028235e38F, a != 0 && std::signbit(a) != std::signbit(b) ? -1.0F : 1.0F);
    return ftz(a / b);
}
float madd(float acc, float a, float b) { return ftz(acc + a * b); }
float cvt_s_w(s32 v) { return static_cast<float>(v); }
// cvt.w.s and 00190e50: truncation, saturating.
s32 cvt_w_s(float f, u32 where) {
    const double d = f;
    if (std::isnan(d)) fail(where, "cvt.w.s of NaN");
    if (d >= 2147483647.0) return 0x7fffffff;
    if (d <= -2147483648.0) return static_cast<s32>(0x80000000U);
    return static_cast<s32>(d);
}
u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

std::uint8_t at(SubtitleText t, std::size_t i, u32 where) {
    if (i >= t.s.size()) fail(where, "string read past its terminator");
    return t.s[i];
}

// The glyph record +a0 + 16 * (c - 0x21): u0, u1, v0, v1.
float glyph(const GuiFont& font, s64 idx, u32 field) {
    const s64 off = 0xa0 + 16 * idx + field;
    if (off < 0) fail(0x410f90, "glyph record before the font object");
    return font.f(static_cast<std::size_t>(off));
}

// The kerning of `c` (glyph `idx`) against the byte after it, as 00411600 /
// 00410e50 apply it to the advance: none for a zero distance or a space.
float kerned(const GuiFont& font, float advance, s64 idx, std::uint8_t next) {
    if (!font.kerning()) return advance;
    const s64 k = static_cast<std::int8_t>(static_cast<std::uint8_t>(next - 0x21));
    if (k == 0 || next == 0x20) return advance;
    return sub(advance, cvt_s_w(font.kern(k + static_cast<s64>(font.count()) * idx)));
}

std::array<float, 4> transform(const std::array<float, 16>& m, const std::array<float, 4>& v) {
    std::array<float, 4> r{};
    for (int k = 0; k < 4; ++k)
        r[k] = madd(madd(madd(mul(m[k], v[0]), m[4 + k], v[1]), m[8 + k], v[2]), m[12 + k], v[3]);
    return r;
}

// The level load's renderer init as the front end runs it: allocations answer tokens.
class InitHost final : public LevelLoadHost {
public:
    u32 call(LevelLoadMemory&, u32 address, std::initializer_list<u32> args) override {
        switch (address) {
        case 0x36b180: {
            const u32 p = next_;
            next_ += (args.begin()[1] + 0xff) & ~0xffU;
            return p;
        }
        case 0x3831d0: return args.begin()[0];
        case 0x103710: return 1;
        case 0x102a28: case 0x102888: case 0x10d3e0: case 0x1037a8: case 0x3bb1c0: case 0x379fe0: return 0;
        default: fail(0x412600, "the renderer mirror reached " + hex(address));
        }
    }
private:
    u32 next_ = 0x0c000000;
};

float rf(const LevelLoadMemory& m, u32 a) { return std::bit_cast<float>(m.read(a, 4)); }
}  // namespace

SubtitleProjection subtitle_projection() {
    LevelLoadMemory m;
    const auto w32 = [&](u32 a, u32 v) { m.write(a, 4, v); };
    const auto w8 = [&](u32 a, u32 v) { m.write(a, 1, v); };
    w32(0x4e00b0, 0x3d27e0);
    w32(video, 0xffffffffU);
    // 0037f740's display modes (the ELF's six {w, h} float pairs at 004bc1d0).
    constexpr u32 modes[12] = {0x44a00000, 0x44340000, 0x44200000, 0x44000000, 0x44200000, 0x43f00000,
                               0x44200000, 0x43b40000, 0x43a00000, 0x43700000, 0, 0};
    for (u32 k = 0; k < 12; ++k) w32(0x4bc1d0 + 4 * k, modes[k]);
    InitHost host;
    renderer_init_0037e780(m, host, video, 640, 448, 0x20, 0x10, 0, 0xfa0, 0x1f4);
    // 00412600's push (0x412a68..0x412bc8).
    const u32 slot = m.read(video + 0xa70, 4) + 1;
    w32(video + 0xa70, slot);
    const u32 rec = video + 0x1f0 + 0x110 * slot;
    w8(rec + 0x106, 1);
    for (u32 k = 0; k < 4; ++k) w32(rec + 4 * k, std::bit_cast<u32>(subtitle_rect[k]));
    w8(rec + 0x105, 1);
    w8(rec + 0x106, 1);
    w32(rec + 0xf0, 0);
    w32(rec + 0xf8, 0x3f800000);
    w8(rec + 0x106, 1);
    w8(rec + 0x104, 1);
    const u32 top = m.read(video + 0xa74, 4);
    for (u32 k = 0; k < 64; k += 4) w32(video + 0xa80 + 64 * (top + 1) + k, m.read(video + 0xa80 + 64 * top + k, 4));
    w32(video + 0xa74, top + 1);
    for (u32 k = 0; k < 16; ++k) w32(video + 0xa80 + 64 * (top + 1) + 4 * k, k % 5 == 0 ? 0x3f800000U : 0U);
    w8(video + 0xa78, 1);
    // 00411550 / 00411580 / 004115c0 (each refreshes the record first).
    renderer_view_projection_0037d9a0(m, host, video);
    camera_record_refresh_00374df0(m, host, rec);
    SubtitleProjection p;
    for (u32 k = 0; k < 16; ++k) p.m[k] = rf(m, video + 0xec0 + 4 * k);
    p.scale_x = rf(m, rec + 0x90);
    p.scale_y = rf(m, rec + 0x94);
    p.offset_x = rf(m, rec + 0xa0);
    p.offset_y = rf(m, rec + 0xa4);
    return p;
}

SubtitleSize subtitle_measure_00411600(const GuiFont& font, std::uint16_t tex_width, SubtitleText text) {
    if (at(text, 0, 0x411600) == 0) return {};
    const float h = cvt_s_w(static_cast<s32>(font.height()) * static_cast<s32>(font.image[0x90]));
    const float w = cvt_s_w(tex_width);
    float total = 0.0F;
    for (std::size_t i = 0; at(text, i, 0x411700) != 0 && i != text.end; ++i) {
        const std::uint8_t c = text.s[i];
        float advance = 2.0F;
        if (c == 0x20) {
            advance = font.space();
        } else {
            const s64 idx = static_cast<s64>(c) - 0x21;
            // mula.s / msub.s: ACC = u1 * W, then ACC - u0 * W.
            const float u1w = mul(glyph(font, idx, 4), w);
            total = add(total, ftz(u1w - glyph(font, idx, 0) * w));
            advance = kerned(font, advance, idx, at(text, i + 1, 0x4116a0));
        }
        total = add(total, advance);
    }
    return {total, h};
}

void subtitle_glyphs_00410e50(std::vector<GsPair>& out, const GuiFont& font, std::uint16_t tex_width,
                              std::uint16_t tex_height, const SubtitleProjection& p, float x, float y,
                              SubtitleText text, const std::array<float, 4>& colour,
                              const std::array<float, 4>& rect) {
    if (at(text, 0, 0x410ec0) == 0) return;
    const float left = mul(0.5F, neg(rect[2])), right = mul(0.5F, rect[2]);
    const float bottom = mul(0.5F, neg(rect[3])), top = mul(0.5F, rect[3]);
    const float y1 = sub(y, cvt_s_w(static_cast<s32>(font.height()) * static_cast<s32>(font.image[0x90])));
    const float tw = cvt_s_w(tex_width), th = cvt_s_w(tex_height);
    const auto vertex = [&](float vx, float vy, float u, float v) {
        const auto q = transform(p.m, {vx, vy, 0.0F, 1.0F});
        const float r = dvd(1.0F, q[3]);
        const float px = add(mul(q[0], mul(p.scale_x, r)), p.offset_x);
        const float py = add(mul(q[1], mul(p.scale_y, r)), p.offset_y);
        const s32 sxv = cvt_w_s(px, 0x190e50), syv = cvt_w_s(py, 0x190e50);
        s32 c[4];
        for (int k = 0; k < 4; ++k) c[k] = cvt_w_s(mul(128.0F, colour[k]), 0x190e50);
        const s32 su = cvt_w_s(mul(16.0F, u), 0x190e50), sv = cvt_w_s(mul(16.0F, v), 0x190e50);
        out.push_back({sx(su) | sx(sv) << 16, 3});
        out.push_back({(sx(c[3]) << 24 | sx(c[2]) << 16 | sx(c[1]) << 8 | sx(c[0])) | u64{0x3f800000} << 32, 1});
        out.push_back({sx(sxv) | sx(syv) << 16, 5});
    };
    for (std::size_t i = 0; at(text, i, 0x411480) != 0 && i != text.end; ++i) {
        const std::uint8_t c = text.s[i];
        float advance = 2.0F;
        if (c == 0x20) {
            advance = font.space();
        } else {
            const s64 idx = static_cast<s64>(c) - 0x21;
            const float u0 = mul(glyph(font, idx, 0), tw), u1 = mul(glyph(font, idx, 4), tw);
            const float v0 = mul(glyph(font, idx, 8), th), v1 = mul(glyph(font, idx, 12), th);
            const float width = sub(u1, u0);
            // Inside the rect: the glyph is drawn and advances by its width
            // (0x411184); a culled glyph only advances by f26.
            if (!(x < left) && add(x, width) <= right && !(y1 < bottom) && y <= top) {
                vertex(x, y, u0, v0);
                x = add(x, width);
                vertex(x, y1, u1, v1);
            }
            advance = kerned(font, advance, idx, at(text, i + 1, 0x41141c));
        }
        x = add(x, advance);
    }
}

SubtitleOverlayState subtitle_overlay_state(const SubtitleTrack& track, const std::array<float, 4>& colour) {
    SubtitleOverlayState s;
    s.visible = track.visible();
    s.flags = track.flags();
    s.lines = track.lines();
    s.colour = track.has_cue() ? colour : std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};
    for (std::size_t i = 0; i < s.lines.size(); ++i)
        s.ends.push_back((track.flags() & subtitle_type) && track.started()[i] ? track.revealed()[i] : no_end);
    return s;
}

namespace {
// 0x411a24..0x411bdc: the lines' origins, then their glyphs.
void subtitle_lines(std::vector<GsPair>& out, const SubtitleOverlayState& st, const GuiFont& font,
                    std::uint16_t tex_width, std::uint16_t tex_height, const SubtitleProjection& projection) {
    if (!st.font || !st.visible) return;
    if (st.lines.size() > 3) fail(0x4118e0, "a cue with more than three lines (the object holds three)");
    if (st.ends.size() != st.lines.size()) fail(0x4118e0, "line ends disagree with the lines");
    std::vector<std::vector<std::uint8_t>> bytes;
    for (const auto& l : st.lines) {
        bytes.emplace_back(l.begin(), l.end());
        bytes.back().push_back(0);
    }
    std::size_t n = st.lines.size();
    if (st.flags & 8) {
        n = 0;
        for (const std::size_t e : st.ends) n += e != no_end;   // +6c[i] non-null
    }
    std::array<float, 3> xs{}, ys{};
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = mul(0.5F, neg(st.rect[2]));
        ys[i] = mul(0.5F, neg(st.rect[3]));
        const SubtitleSize size = subtitle_measure_00411600(font, tex_width, {bytes[i], st.ends[i]});
        ys[i] = add(ys[i], mul(add(4.0F, size.h), cvt_s_w(static_cast<s32>(n - i))));
        if (st.flags & 4) {
            float d = sub(st.rect[2], size.w);
            if (!(0.0F <= d)) d = 0.0F;
            xs[i] = add(xs[i], mul(0.5F, d));
        }
    }
    for (std::size_t i = 0; i < n; ++i)
        subtitle_glyphs_00410e50(out, font, tex_width, tex_height, projection, xs[i], ys[i], {bytes[i], st.ends[i]},
                                 st.colour, st.rect);
}
}  // namespace

SubtitlePacket subtitle_packet_004118e0(const SubtitleOverlayState& state, const GuiFont& font,
                                        std::uint16_t tex_width, std::uint16_t tex_height,
                                        const SubtitleProjection& projection, std::uint64_t tex0,
                                        std::uint64_t tex1) {
    SubtitlePacket p;
    p.pairs = {{tex0, 6}, {tex1, 0x14}, {0x44, 0x42}, {0x1d6, 0}};
    subtitle_lines(p.pairs, state, font, tex_width, tex_height, projection);
    // The buffer is 0x7280 bytes from an allocation rounded up to 128.
    if (32 + 16 * p.pairs.size() + 16 > 0x7280 - 0x7f) fail(0x4118e0, "packet past the 0x7280-byte buffer");
    const u32 q = static_cast<u32>(p.pairs.size()) + 2;
    p.qwc = q - 1;
    p.nloop = q - 2;
    return p;
}

std::uint64_t subtitle_tex1_003bb620(std::uint64_t tex1_58, std::uint32_t render_mode) {
    const u64 mag = (render_mode & 0xc) != 0 ? 1 : 0;
    const u64 min = (render_mode & 4) != 0 ? 4 : (render_mode & 8) != 0 ? 5 : 0;
    return (tex1_58 & ~u64{0x1e0}) | mag << 5 | min << 6;
}

static float narrow_0011c928(double d) {
    const double a = std::fabs(d);
    // Magnitudes in [FLT_MAX, 2^128) still truncate to FLT_MAX; from 2^128 the
    // results are irregular (EE goldens: 1e39 gives inf, -3.5e38 ffc3a7c6).
    if (std::isnan(d) || a >= std::ldexp(1.0, 128)) fail(0x11c928, "a Color number outside the float range");
    if (a < static_cast<double>(std::numeric_limits<float>::min())) return std::signbit(d) ? -0.0F : 0.0F;
    float f = static_cast<float>(d);
    if (std::fabs(static_cast<double>(f)) > a) f = std::nextafter(f, 0.0F);
    return f;
}

std::array<float, 4> subtitle_colour_words(const std::string& value) {
    std::array<float, 4> c{};
    const char* s = value.c_str();
    for (float& k : c) {
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        if (end == s) fail(0x410c20, "Color \"" + value + "\" has fewer than four numbers");
        k = narrow_0011c928(d);
        s = end;
    }
    return c;
}

std::array<float, 4> subtitle_colour(const SubtitleFile& file) {
    const auto value = file.init_value("Color");
    if (!value) return {1.0F, 1.0F, 1.0F, 0.5F};   // the feed object's default colour
    return subtitle_colour_words(*value);
}

GuiDrawBatch subtitle_batch(const SubtitlePacket& packet, std::uint32_t slot, std::uint16_t tex_width,
                            std::uint16_t tex_height) {
    GuiDrawBatch b;
    b.flush = 0x4118e0;
    b.render_mode = 5;
    b.slot = slot;
    if (packet.pairs.size() >= 2) b.filter = static_cast<std::uint32_t>(packet.pairs[1].data & 0x1e0);
    if (packet.pairs.size() < 4 || (packet.pairs.size() - 4) % 6 != 0) fail(0x4118e0, "packet is not whole sprites");
    for (std::size_t k = 4; k < packet.pairs.size(); k += 3) {
        const auto& uv = packet.pairs[k];
        const auto& rgba = packet.pairs[k + 1];
        const auto& xyz = packet.pairs[k + 2];
        if (uv.reg != 3 || rgba.reg != 1 || xyz.reg != 5) fail(0x410e50, "vertex pairs out of order");
        GuiVertex v;
        const float gx = static_cast<float>(xyz.data & 0xffff) / 16.0F;
        const float gy = static_cast<float>((xyz.data >> 16) & 0xffff) / 16.0F;
        v.pos = {gx - 2048.0F, 2048.0F - gy, 0.0F, 1.0F};
        for (int c = 0; c < 4; ++c) v.colour[c] = static_cast<std::int32_t>((rgba.data >> (8 * c)) & 0xff);
        const float u = static_cast<float>(uv.data & 0x3fff) / 16.0F, w = static_cast<float>((uv.data >> 16) & 0x3fff) / 16.0F;
        v.uv = {2048.0F + u / static_cast<float>(tex_width), 2048.0F + w / static_cast<float>(tex_height), 1.0F, 0.0F};
        b.vertices.push_back(v);
    }
    if (!b.vertices.empty()) b.strips = {static_cast<std::int32_t>(b.vertices.size())};
    return b;
}

}  // namespace omega::movie
