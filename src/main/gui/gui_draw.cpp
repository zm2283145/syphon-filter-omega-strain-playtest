#include "gui_draw.hpp"

#include "frontend_formats.hpp"
#include "gui_decor.hpp"
#include "gui_runtime.hpp"
#include "ie_draw.hpp"
#include "ie_screen.hpp"
#include "zone_profile.hpp"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace omega {
using gui::flag_enabled;
using gui::flag_visible;
using gui::GuiRuntime;
using gui::TextObject;
using gui::Widget;
using gui::WidgetClass;
namespace {

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui draw: unhosted " + what + " (" + buf + ")");
}

// EE COP1 as the interpreter models it: binary32 round-to-nearest, results
// flushed to zero when denormal, x / 0 = +-FLT_MAX.
float ftz(float v) {
    if (v != 0.0F && std::fabs(v) < FLT_MIN) return std::copysign(0.0F, v);
    return v;
}
float fadd(float a, float b) { return ftz(a + b); }
float fsub(float a, float b) { return ftz(a - b); }
float fmul(float a, float b) { return ftz(a * b); }
float fdiv(float a, float b) {
    if (b == 0.0F) {
        const float sign = a != 0.0F ? a * b : 1.0F;
        return std::copysign(FLT_MAX, std::signbit(sign) ? -1.0F : 1.0F);
    }
    return ftz(a / b);
}
std::int32_t cvt_w(float x, std::uint32_t address) {  // cvt.w.s: truncate, saturate
    if (std::isnan(x)) fail(address, "cvt.w.s of NaN");
    if (x >= 2147483647.0F) return INT32_MAX;
    if (x <= -2147483648.0F) return INT32_MIN;
    return static_cast<std::int32_t>(x);
}
// cvt.s.w; only exact conversions are hosted (the EE truncates where the
// host rounds).
float i2f(std::int64_t v, std::uint32_t address) {
    const auto w = static_cast<std::int32_t>(v);
    const float f = static_cast<float>(w);
    if (static_cast<std::int64_t>(f) != w) fail(address, "inexact cvt.s.w of " + std::to_string(w));
    return f;
}

constexpr float k_z = std::bit_cast<float>(0x3a83126fU);  // 0.001: the text draws' z
constexpr float k_f54 = std::bit_cast<float>(0x3f19999aU);  // 003d0940's +54
constexpr std::uint8_t k_dots[] = {'.', '.', '.', 0};    // 004bd310

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t o) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}
void wr32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
void wrf(std::vector<std::uint8_t>& b, std::size_t o, float v) { wr32(b, o, std::bit_cast<std::uint32_t>(v)); }

// A string pointer of 003cfab0 / 003d04c0: null, into the text object's
// string (+14's characters and terminator) or into 003cfab0's 0x100-byte
// stack line buffer (sp + c0). Reads outside either throw: past the buffer
// is the caller's stack (the line colour at sp + 1c0), not modelled.
enum class Region : std::uint8_t { null, text, buf };
struct Ptr {
    Region r = Region::null;
    std::int32_t o = 0;
    explicit operator bool() const { return r != Region::null; }
    bool operator==(const Ptr&) const = default;
    Ptr operator+(std::int32_t d) const { return {r, o + d}; }
};

class TextMem {
public:
    explicit TextMem(const std::string& text) : text_(text.begin(), text.end()) { text_.push_back(0); }
    std::array<std::uint8_t, 0x100> buf{};

    std::span<const std::uint8_t> from(Ptr p, std::uint32_t address) const {
        const auto all = region(p, address);
        if (p.o < 0 || static_cast<std::size_t>(p.o) > all.size()) fail(address, "string pointer outside its buffer");
        return all.subspan(static_cast<std::size_t>(p.o));
    }
    std::uint8_t at(Ptr p, std::uint32_t address, std::int32_t i = 0) const {
        const auto all = region(p, address);
        const std::int64_t k = static_cast<std::int64_t>(p.o) + i;
        if (k < 0 || k >= static_cast<std::int64_t>(all.size()))
            fail(address, p.r == Region::buf ? "read past the 0x100-byte line buffer" : "read past the string");
        return all[static_cast<std::size_t>(k)];
    }
    std::int32_t strlen(Ptr p, std::uint32_t address) const {  // 001294f8
        std::int32_t n = 0;
        while (at(p, address, n) != 0) ++n;
        return n;
    }
    void set(std::int64_t i, std::uint8_t v, std::uint32_t address) {
        if (i < 0 || i >= static_cast<std::int64_t>(buf.size())) fail(address, "write past the 0x100-byte line buffer");
        buf[static_cast<std::size_t>(i)] = v;
    }
    // 00129a70 strncpy(buf, p, 0x100): a forward copy (the source may lie
    // inside buf, past its start: the copy then reads each byte before it is
    // overwritten), zero padded. A 0x100-byte source leaves buf unterminated
    // and the strchr after it reads the stack: not hosted.
    void strncpy_buf(Ptr p, std::uint32_t address) {
        std::vector<std::uint8_t> s;
        for (std::int32_t i = 0; i < 0x100; ++i) {
            const std::uint8_t c = at(p, address, i);
            if (c == 0) break;
            s.push_back(c);
        }
        if (s.size() >= 0x100) fail(address, "line of 0x100 bytes or more (unterminated line buffer)");
        std::memcpy(buf.data(), s.data(), s.size());
        std::memset(buf.data() + s.size(), 0, buf.size() - s.size());
    }
    // 00129360 strcpy(buf + at, src) for a source outside buf.
    void strcpy_buf(std::int32_t at_, std::span<const std::uint8_t> src, std::uint32_t address) {
        for (std::size_t i = 0;; ++i) {
            if (i >= src.size()) fail(address, "strcpy source without terminator");
            set(static_cast<std::int64_t>(at_) + static_cast<std::int64_t>(i), src[i], address);
            if (src[i] == 0) return;
        }
    }
    // 00129708 strncat(buf, p, n) for a source outside buf.
    void strncat_buf(Ptr p, std::int32_t n, std::uint32_t address) {
        std::int32_t k = strlen({Region::buf, 0}, address);
        for (std::int32_t i = 0; i < n; ++i) {
            const std::uint8_t c = at(p, address, i);
            if (c == 0) break;
            set(k++, c, address);
        }
        set(k, 0, address);
    }

private:
    std::span<const std::uint8_t> region(Ptr p, std::uint32_t address) const {
        if (p.r == Region::text) return text_;
        if (p.r == Region::buf) return buf;
        fail(address, "null string pointer");
    }
    std::vector<std::uint8_t> text_;
};

std::int32_t width_at(const GuiFont& font, const TextMem& m, Ptr p, std::int32_t n) {
    return gui_font_width(font, m.from(p, 0x36aa50), n);
}

std::string c_string(std::span<const std::uint8_t> s) {
    std::string out;
    for (std::size_t i = 0; i < s.size() && s[i] != 0; ++i) out.push_back(static_cast<char>(s[i]));
    return out;
}

void font_call(GuiDrawList& out, std::vector<GuiFontCall>* calls, const GuiFont& font, std::span<const std::uint8_t> s,
               const std::array<float, 4>& rgba, float x, float y, float z, float l, float r, float f) {
    if (calls) calls->push_back({c_string(s), rgba, x, y, z, l, r, f});
    gui_font_draw(out, font, s, rgba, x, y, z, l, r, f);
    if (calls) calls->back().batch_end = out.batches.size();
}

// The space-or-newline word end of 003cfab0 (003d00a0) / 003d04c0 (003d05c0):
// s1 = the byte after the first ' ' / '\n' found from s + 1 (the end of the
// string when none), null for an empty s; s3 = s1 - s, or strlen(s) + 1.
void next_word(const TextMem& m, Ptr s, std::uint32_t address, Ptr& s1, std::int32_t& s3) {
    if (m.at(s, address) == 0) {
        s1 = {};
    } else {
        const std::int32_t len = m.strlen(s + 1, address);
        std::int32_t j = 0;
        if (len > 0) {
            for (;;) {
                const std::uint8_t c = m.at(s, address, 1 + j);
                if (c == ' ' || c == '\n') break;
                ++j;
                if (!(j < len)) break;
            }
        }
        s1 = s + (j + 1);
    }
    s3 = s1 ? s1.o - s.o : m.strlen(s, address) + 1;
}

}  // namespace

// ---------------------------------------------------------------- font

GuiFont GuiFont::from_fnt(const FontFile& fnt, std::uint32_t slot, std::uint16_t tex_width) {
    GuiFont f;
    f.tex_width = tex_width;
    auto& b = f.image;
    wr32(b, 0x0, slot);
    for (int i = 0; i < 8; ++i) {  // 0036afc0
        wrf(b, 0x10 + 16 * i, 0.5F);
        wrf(b, 0x14 + 16 * i, 0.5F);
        wrf(b, 0x18 + 16 * i, 0.5F);
        wrf(b, 0x1c + 16 * i, 1.0F);
    }
    b[0x90] = 1;
    wr32(b, 0x8a74, 0x004df630);
    // 0036ab80
    wrf(b, 0x94, static_cast<float>(fnt.space));
    b[0x98] = fnt.cell_w;
    b[0x99] = fnt.height;
    wr32(b, 0x9c, fnt.count);
    if (0xa0 + 16 * fnt.glyphs.size() > 0xbe0) fail(0x36ab80, "font glyph table larger than the object");
    for (std::size_t i = 0; i < fnt.glyphs.size(); ++i) {
        const auto& g = fnt.glyphs[i];
        wrf(b, 0xa0 + 16 * i, g.u0);
        wrf(b, 0xa4 + 16 * i, g.u1);
        wrf(b, 0xa8 + 16 * i, g.v0);
        wrf(b, 0xac + 16 * i, g.v1);
    }
    b[0xbe0] = fnt.kerning ? 1 : 0;
    if (0xbe1 + fnt.kern.size() > 0x8a74) fail(0x36ab80, "font kerning table larger than the object");
    for (std::size_t i = 0; i < fnt.kern.size(); ++i) b[0xbe1 + i] = static_cast<std::uint8_t>(fnt.kern[i]);
    return f;
}

std::uint32_t GuiFont::slot() const { return rd32(image, 0); }
std::uint32_t GuiFont::count() const { return rd32(image, 0x9c); }
float GuiFont::space() const { return f(0x94); }
float GuiFont::f(std::size_t offset) const {
    if (offset + 4 > image.size()) fail(0x36a330, "font read outside the object");
    return std::bit_cast<float>(rd32(image, offset));
}
std::array<float, 4> GuiFont::colour(std::uint32_t index) const {
    return {f(0x10 + 16 * index), f(0x14 + 16 * index), f(0x18 + 16 * index), f(0x1c + 16 * index)};
}
std::int8_t GuiFont::kern(std::int64_t index) const {
    const std::int64_t k = 0xbe1 + index;
    if (k < 0 || k >= static_cast<std::int64_t>(image.size())) fail(0x36aa50, "kerning read outside the font object");
    return static_cast<std::int8_t>(image[static_cast<std::size_t>(k)]);
}

std::uint64_t gui_font_image_hash(const GuiFont& font) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 4; i < 0x8a74; ++i) h = (h ^ font.image[i]) * 0x100000001b3ULL;
    return h;
}

namespace {
// The kerned advance of glyph g before the next byte (0036a958 / 0036aac8):
// 2.0, less the int8 at +be1 + (int8)(next - 0x21) + g * count when the
// font kerns and the next byte is neither '!' nor ' '.
float glyph_advance(const GuiFont& font, std::int32_t g, std::uint8_t next) {
    float adv = 2.0F;
    if (!font.kerning()) return adv;
    const auto k = static_cast<std::int8_t>(static_cast<std::uint8_t>(next - 0x21));
    if (k == 0 || next == 0x20) return adv;
    const std::int64_t index = static_cast<std::int64_t>(k) +
                               static_cast<std::int32_t>(static_cast<std::uint32_t>(g) * font.count());
    return fsub(adv, i2f(font.kern(index), 0x36a9a8));
}

bool control(const GuiFont& font, std::uint8_t c) {
    return c < 0x20 || static_cast<std::int32_t>(font.count() + 0x20) < static_cast<std::int32_t>(c);
}
}  // namespace

std::int32_t gui_font_width(const GuiFont& font, std::span<const std::uint8_t> s, std::int32_t n) {
    auto rd = [&](std::size_t k) {
        if (k >= s.size()) fail(0x36aa50, "string read past its buffer");
        return s[k];
    };
    const float texw = i2f(font.tex_width, 0x36aa88);
    float f = 0.0F;
    std::size_t i = 0;
    for (;;) {
        const std::uint8_t c = rd(i);
        if (c == 0 || n <= 0) break;
        if (control(font, c)) {
            ++i;
            continue;
        }
        float adv;
        if (c == 0x20) {
            adv = font.space();
        } else {
            const std::int32_t g = c - 0x21;
            f = fadd(f, fmul(texw, fsub(font.f(0xa4 + 16 * g), font.f(0xa0 + 16 * g))));
            adv = font.kerning() ? glyph_advance(font, g, rd(i + 1)) : 2.0F;
        }
        ++i;
        f = fadd(f, adv);
        --n;
    }
    return cvt_w(f, 0x36ab68);
}

void GuiDrawList::recycle() {
    spare.reserve(spare.size() + batches.size());
    for (GuiDrawBatch& b : batches) spare.push_back(std::move(b));
    batches.clear();
}

GuiDrawBatch GuiDrawList::take() {
    if (spare.empty()) return {};
    GuiDrawBatch old = std::move(spare.back());
    spare.pop_back();
    GuiDrawBatch b;
    b.strips = std::move(old.strips);
    b.vertices = std::move(old.vertices);
    b.breaks = std::move(old.breaks);
    b.strips.clear();
    b.vertices.clear();
    b.breaks.clear();
    return b;
}

void gui_font_draw(GuiDrawList& out, const GuiFont& font, std::span<const std::uint8_t> s,
                   const std::array<float, 4>& rgba, float x, float y, float z, float clip_l, float clip_r,
                   float fade) {
    auto rd = [&](std::size_t k) {
        if (k >= s.size()) fail(0x36a330, "string read past its buffer");
        return s[k];
    };
    if (rd(0) == 0) return;
    GuiDrawBatch b = out.take();
    b.render_mode = 5;
    b.slot = font.slot();
    b.prim = 3;
    b.strip_max = 0x40;
    b.v8 = 6ULL << 32;
    b.strips.push_back(0);  // 0025cd10
    // 0025ca40 + the three array stores: a full strip (V+1040 == V+104c)
    // opens the next one with a copy of the last two vertices (positions,
    // and colours / texture coordinates as V+8 bits 33 / 34 say: both set).
    auto emit = [&](const GuiVertex& v) {
        if (b.strips.back() == static_cast<std::int32_t>(b.strip_max)) {
            const std::size_t n = b.vertices.size();
            const GuiVertex a = b.vertices[n - 2], c = b.vertices[n - 1];
            b.vertices.push_back(a);
            b.vertices.push_back(c);
            b.strips.push_back(2);
        }
        b.vertices.push_back(v);
        ++b.strips.back();
    };
    auto colour = [](const std::array<float, 4>& c) {  // 0025c9d0
        std::array<std::int32_t, 4> o{};
        for (int k = 0; k < 4; ++k) o[k] = cvt_w(fmul(128.0F, c[k]), 0x25c9e4);
        return o;
    };
    const float texw = i2f(font.tex_width, 0x36a3f4);
    const float base = 2048.0F;  // 0025f220
    std::array<float, 4> col = rgba;
    const float left_fade = fadd(clip_l, fade), right_fade = fsub(clip_r, fade);
    const bool fading = !(fade <= 0.0F);
    auto alpha_at = [&](float px) {
        if (px < left_fade) return fdiv(fmul(rgba[3], fsub(px, clip_l)), fade);
        if (!(px <= right_fade)) return fdiv(fmul(rgba[3], fsub(clip_r, px)), fade);
        return rgba[3];
    };
    const float height = i2f(font.height(), 0x36a858);
    std::size_t i = 0;
    int emitted = 0;
    for (std::uint8_t c = rd(0); c != 0; c = rd(i)) {
        if (control(font, c)) {
            if (c >= 0x10 && c < 0x18) col = font.colour(c - 0x10u);
            ++i;
            continue;
        }
        float adv;
        if (c == 0x20) {
            adv = font.space();
        } else {
            const std::int32_t g = c - 0x21;
            float u0 = font.f(0xa0 + 16 * g);
            float u1 = font.f(0xa4 + 16 * g);
            const float v0 = font.f(0xa8 + 16 * g);
            const float v1 = font.f(0xac + 16 * g);
            float dw = fsub(u1, u0);
            float wid = fmul(texw, dw);
            bool skip = false;
            if (fading) {
                col[3] = alpha_at(x);
                if (fadd(x, wid) < clip_l) {
                    skip = true;
                } else if (!(x <= clip_r)) {
                    break;
                } else if (x < clip_l) {
                    col[3] = 0.0F;
                    u0 = fadd(u0, fmul(fdiv(fsub(clip_l, x), wid), dw));
                    dw = fsub(u1, u0);
                    wid = fmul(texw, dw);
                    x = clip_l;
                }
            }
            if (skip) {
                x = fadd(x, wid);
            } else {
                emit({{x, y, z, 1.0F}, colour(col), {fadd(u0, base), fadd(v0, base), 1.0F, 0.0F}});
                x = fadd(x, wid);
                if (fading) {
                    col[3] = alpha_at(x);
                    if (!(x <= clip_r)) {
                        col[3] = 0.0F;
                        u1 = fsub(u1, fmul(fdiv(fsub(x, clip_r), wid), dw));
                        x = clip_r;
                    }
                }
                emit({{x, fsub(y, height), z, 1.0F}, colour(col), {fadd(u1, base), fadd(v1, base), 1.0F, 0.0F}});
                emitted += 2;
            }
            adv = font.kerning() ? glyph_advance(font, g, rd(i + 1)) : 2.0F;
        }
        ++i;
        x = fadd(x, adv);
    }
    b.port_text = true;  // port-only (132ws5): the wide menu draw does not mirror a string
    if (emitted) out.batches.push_back(std::move(b));  // 00370550
}

// ---------------------------------------------------------------- text object

std::int32_t gui_text_line_count(const GuiTextDraw& t) {
    if (t.text.empty() || t.b58) return 1;  // 002071f0
    if (t.b5b) {
        std::int32_t n = 1;
        for (std::size_t p = 0; (p = t.text.find('\n', p)) != std::string::npos; ++p) ++n;  // 00129088
        // c_str stops at an embedded NUL
        if (t.text.find('\0') != std::string::npos) fail(0x3d04c0, "text with an embedded NUL");
        return n;
    }
    if (!t.font) fail(0x3d064c, "line count without a font");
    const GuiFont& font = *t.font;
    const TextMem m(t.text);
    constexpr std::uint32_t A = 0x3d04c0;
    const float x0 = i2f(t.x, A);
    float x = x0;
    const std::int32_t w = t.w <= 0 ? static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(t.w)) : t.w;
    const float r = fadd(x, i2f(w, A));
    Ptr s{Region::text, 0};
    std::int32_t lines = 0;
    for (;;) {
        const Ptr start = s;
        const float thr = fadd(x, fmul(fsub(1.0F, t.f54), fsub(r, x)));
        for (;;) {
            Ptr s1;
            std::int32_t s3 = 0;
            next_word(m, s, A, s1, s3);
            const std::int32_t wd = width_at(font, m, s, s3);
            if (fadd(x, i2f(wd, A)) < r || (x == x0 && t.f54 == 1.0F)) {
                s = s1;
                x = fadd(x, i2f(wd, A));
            } else if (t.f54 != 1.0F && x <= thr) {
                std::int32_t j = 0;
                while (j < s3 && fadd(x, i2f(width_at(font, m, s, j + 1), A)) < r) ++j;
                s = s + j;
                break;
            }
            if (s1 && m.at(s1, A) == '\n') break;
            if (!s) break;
            if (fadd(x, i2f(wd, A)) < r) continue;
            break;
        }
        ++lines;
        x = x0;
        if (s && m.at(s, A) == ' ')
            while (m.at(s, A) == ' ') s = s + 1;
        if (!s || m.at(s, A) == 0) return lines;
        if (s == start) fail(A, "a line that takes no character (the line count loops forever)");
    }
}

void gui_text_draw(GuiDrawList& out, const GuiTextDraw& t, std::vector<GuiFontCall>* calls) {
    if (!t.font) return;
    const GuiFont& font = *t.font;
    constexpr std::uint32_t A = 0x3cfab0;
    const std::int32_t line_h = font.height();  // 0036a2f0
    const float x0 = i2f(t.x, A);
    float x = x0;
    float y = i2f(t.y, A);
    if (t.b11) y = fsub(y, i2f(static_cast<std::int32_t>(static_cast<std::uint32_t>(t.h) - line_h), A));
    const std::int32_t w = t.w <= 0 ? static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(t.w)) : t.w;
    const float r = fadd(x, i2f(w, A));
    std::int32_t fp = -1;
    if (t.b12) {
        fp = gui_text_line_count(t);
        const auto lines_h = static_cast<std::int32_t>(static_cast<std::uint32_t>(fp) * static_cast<std::uint32_t>(line_h));
        y = fsub(y, i2f(t.h / 2 - lines_h / 2, A));
    }
    TextMem m(t.text);
    if (t.b10 == 3) {  // 003cfd90
        font_call(out, calls, font, m.from({Region::text, 0}, A), t.colour, fsub(x, t.f40), y, k_z, x0, r,
                  i2f(t.w / 4, A));
        return;
    }
    std::array<float, 4> col = t.colour;
    Ptr s{Region::text, 0};
    std::int32_t iterations = 0;
    do {
        const Ptr start = s;
        m.buf[0] = 0;
        if (t.b5b) {  // 003cfe24
            if (fp == -1) fp = gui_text_line_count(t);
            m.strncpy_buf(s, 0x3cfe50);
            Ptr nl;
            for (std::int32_t k = 0;; ++k) {  // 00129088
                const std::uint8_t c = m.at({Region::buf, 0}, 0x3cfe5c, k);
                if (c == '\n') {
                    nl = {Region::buf, k};
                    break;
                }
                if (c == 0) break;
            }
            if (nl) m.buf[static_cast<std::size_t>(nl.o)] = 0;
            const Ptr b0{Region::buf, 0};
            const float wd = i2f(width_at(font, m, b0, m.strlen(b0, A)), A);
            if (fadd(x, wd) < r) {
                x = fadd(x, wd);
                s = nl ? nl + 1 : Ptr{};
            } else {
                const float e3 = i2f(gui_font_width(font, k_dots, 3), A);
                const float avail = fsub(r, fadd(x, e3));
                if (avail <= 0.0F) {
                    x = fadd(x, e3);
                    m.strcpy_buf(0, k_dots, 0x3cff0c);
                    s = {};
                } else {
                    std::int32_t k = 0, ws = 0;
                    for (;;) {
                        const std::uint8_t c = m.at(s, A);
                        if (c == 0 || !(i2f(ws, A) <= avail) || c == '\n') break;
                        m.set(k++, c, 0x3cff40);
                        const std::int32_t prev = ws;
                        s = s + 1;
                        ws = width_at(font, m, b0, k);
                        if (!(i2f(ws, A) <= avail)) {
                            ws = prev;
                            --k;
                        }
                    }
                    m.strcpy_buf(k, k_dots, 0x3cffb4);
                    x = fadd(x, fadd(e3, i2f(ws, A)));
                    if (fp >= 2) {
                        while (m.at(s, A) != '\n' && m.at(s, A) != 0) s = s + 1;
                        if (m.at(s, A) == '\n') s = s + 1;
                        if (m.at(s, A) == 0) s = {};
                    }
                }
            }
            if (fp == 1) s = {};
        } else if (t.b58) {  // 003d0038
            const std::int32_t wd = width_at(font, m, s, m.strlen(s, A));
            m.strcpy_buf(0, m.from(s, A), 0x3d006c);
            x = fadd(x, i2f(wd, A));
            s = {};
        } else {  // 003d0080
            const float thr = fadd(x, fmul(fsub(1.0F, t.f54), fsub(r, x)));
            for (;;) {
                Ptr s1;
                std::int32_t s3 = 0;
                next_word(m, s, A, s1, s3);
                const std::int32_t wd = width_at(font, m, s, s3);
                if (fadd(x, i2f(wd, A)) < r || (x == x0 && t.f54 == 1.0F)) {
                    m.strncat_buf(s, s3, 0x3d0188);
                    s = s1;
                    x = fadd(x, i2f(wd, A));
                } else if (t.f54 != 1.0F && x <= thr) {  // 003d01d0: the character fill
                    std::int32_t k = m.strlen({Region::buf, 0}, A);
                    std::int32_t last = 0, j = 0;
                    while (j < s3) {
                        const std::int32_t wj = width_at(font, m, s, j + 1);
                        if (!(fadd(x, i2f(wj, A)) < r)) break;
                        last = wj;
                        m.set(k++, m.at(s, A, j), 0x3d023c);
                        ++j;
                    }
                    m.set(k, 0, 0x3d0258);
                    s = s + j;
                    x = fadd(x, i2f(last, A));
                    break;
                }
                if (s1 && m.at(s1, A) == '\n') break;
                if (!s) break;
                if (fadd(x, i2f(wd, A)) < r) continue;
                break;
            }
        }
        // 003d02b0
        const std::int32_t off = cvt_w(fsub(x, x0), 0x3d02c8);
        if (t.b10 == 0) x = x0;
        else if (t.b10 == 1) x = fsub(r, i2f(off, A));
        else if (t.b10 == 2) x = i2f(t.x + (t.w - off) / 2, A);
        font_call(out, calls, font, m.from({Region::buf, 0}, A), col, x, y, k_z, 0.0F, 0.0F, 0.0F);
        x = x0;
        y = fsub(y, i2f(static_cast<std::int32_t>(static_cast<std::uint32_t>(t.b11 == 0 ? 1 : -1) *
                                                  static_cast<std::uint32_t>(line_h)),
                        A));
        if (s && m.at(s, A) == ' ')
            while (m.at(s, A) == ' ') s = s + 1;
        if (s && m.at(s, A) == 0) s = {};
        if (s) {  // 003d03f0: the line's last colour code carries over
            for (std::int32_t k = m.strlen({Region::buf, 0}, A) - 1; k >= 0; --k) {
                const std::uint8_t c = m.buf[static_cast<std::size_t>(k)];
                if (c >= 0x10 && c < 0x18) {
                    col = font.colour(c - 0x10u);
                    break;
                }
            }
        }
        if (s && s == start && s.r == Region::text)
            fail(A, "a line that takes no character (the text draw loops forever)");
        if (++iterations > 0x10000) fail(A, "more than 0x10000 lines");
    } while (s);
}

bool gui_text_cursor(const GuiTextDraw& t, std::int32_t index, float& out_x, float& out_y) {
    if (!t.font) return false;
    const GuiFont& font = *t.font;
    constexpr std::uint32_t A = 0x3cf3a0;
    const std::int32_t line_h = font.height();  // 0036a2f0
    const float x0 = i2f(t.x, A);
    float x = x0;
    float y = i2f(t.y, A);
    if (t.b11) y = fsub(y, i2f(static_cast<std::int32_t>(static_cast<std::uint32_t>(t.h) - line_h), A));
    const std::int32_t w = t.w <= 0 ? static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(t.w)) : t.w;
    const float r = fadd(x, i2f(w, A));
    if (t.b12) {
        const std::int32_t n = gui_text_line_count(t);
        const auto lines_h = static_cast<std::int32_t>(static_cast<std::uint32_t>(n) * static_cast<std::uint32_t>(line_h));
        y = fsub(y, i2f(t.h / 2 - lines_h / 2, A));
    }
    TextMem m(t.text);
    Ptr s{Region::text, 0};
    const std::int64_t target = index;  // c_str(+14) + a3 (sp+c0)
    if (t.b10 == 3) return false;
    std::int32_t iterations = 0;
    for (;;) {
        m.buf[0] = 0;
        const Ptr start = s;  // fp
        if (t.b5b) fail(0x3cf4f8, "text cursor of an ellipsis-per-line object");
        if (t.b58) {  // 003cf698
            const std::int32_t wd = width_at(font, m, s, m.strlen(s, A));
            m.strcpy_buf(0, m.from(s, A), 0x3cf6cc);
            x = fadd(x, i2f(wd, A));
            s = {};
        } else {  // 003cf6e0
            const float thr = fadd(x, fmul(fsub(1.0F, t.f54), fsub(r, x)));
            for (;;) {
                Ptr s1;
                std::int32_t s3 = 0;
                next_word(m, s, A, s1, s3);
                const std::int32_t wd = width_at(font, m, s, s3);
                if (fadd(x, i2f(wd, A)) < r || (x == x0 && t.f54 == 1.0F)) {
                    m.strncat_buf(s, s3, 0x3cf7e8);
                    s = s1;
                    x = fadd(x, i2f(wd, A));
                } else if (t.f54 != 1.0F && x <= thr) {  // 003cf830: the character fill
                    std::int32_t k = m.strlen({Region::buf, 0}, A);
                    std::int32_t last = 0, j = 0;
                    while (j < s3) {
                        const std::int32_t wj = width_at(font, m, s, j + 1);
                        if (!(fadd(x, i2f(wj, A)) < r)) break;
                        last = wj;
                        m.set(k++, m.at(s, A, j), 0x3cf89c);
                        ++j;
                    }
                    m.set(k, 0, 0x3cf8b8);
                    s = s + j;
                    x = fadd(x, i2f(last, A));
                    break;
                }
                if (s1 && m.at(s1, A) == '\n') break;
                if (!s) break;
                if (fadd(x, i2f(wd, A)) < r) continue;
                break;
            }
        }
        // 003cf910: the line's alignment
        const std::int32_t off = cvt_w(fsub(x, x0), 0x3cf928);
        if (t.b10 == 0) x = x0;
        else if (t.b10 == 1) x = fsub(r, i2f(off, A));
        else if (t.b10 == 2) x = i2f(t.x + (t.w - off) / 2, A);
        const std::int32_t len = m.strlen({Region::buf, 0}, A);
        if (!(static_cast<std::int64_t>(start.o) + len < target)) {  // the line holds the index
            out_y = y;
            const auto n = static_cast<std::int32_t>(target - start.o);
            out_x = fadd(x, i2f(gui_font_width(font, m.from({Region::buf, 0}, A), n), A));
            return true;
        }
        x = x0;
        y = fsub(y, i2f(static_cast<std::int32_t>(static_cast<std::uint32_t>(t.b11 == 0 ? 1 : -1) *
                                                  static_cast<std::uint32_t>(line_h)),
                        A));
        if (s && m.at(s, A) == ' ')
            while (m.at(s, A) == ' ') s = s + 1;
        if (!s) return false;
        if (s == start) fail(A, "a line that takes no character (the cursor search loops forever)");
        if (++iterations > 0x10000) fail(A, "more than 0x10000 lines");
    }
}

void gui_text_widget_object(std::uint16_t flags, const std::array<float, 4>& rect, const std::array<float, 4>& rgba,
                            bool fading, std::array<std::int32_t, 4>& obj_rect, std::array<float, 4>& obj_colour) {
    std::array<float, 4> c = rgba;
    if (!(flags & 4)) {  // 0041c190: a disabled widget draws at half colour
        for (int k = 0; k < 3; ++k) c[k] = fmul(c[k], 0.5F);
    }
    if (!fading) obj_colour = c;  // 003d0810
    for (int k = 0; k < 4; ++k) obj_rect[k] = cvt_w(rect[k], 0x41c270);
}

GuiTextDraw gui_text_from_ie(const IeTextObject& obj, const GuiFont* font) {
    GuiTextDraw t;
    t.x = obj.x;
    t.y = obj.y;
    t.w = obj.w;
    t.h = obj.h;
    t.b10 = obj.byte_10;
    t.b11 = obj.byte_11;
    t.b58 = obj.byte_58;
    t.b5b = obj.byte_5b;
    t.f40 = obj.f40;
    t.f54 = obj.f54;
    t.colour = obj.colour;
    t.font = font;
    t.text = obj.text.value_or(std::string());
    return t;
}

// ---------------------------------------------------------------- widget tree

void GuiDrawer::draw_canvas(GuiDrawList& out, const GuiRuntime& gui, std::int32_t now_ms) {
    OMEGA_ZONE("draw.canvas");
    const Widget& canvas = gui.canvas();
    if (!(canvas.flags & flag_enabled)) return;  // 00415bc4
    // 00415bd0..00415e24: the ortho camera (005401f0 +f0 / +f8 / +104 /
    // +106: the host's 2D mapping, gui_renderer), the matrix push 003ed290's
    // copy with the new top set to identity times the canvas model rows
    // (1,0,0,0) (0,0,1,0) (0,1,0,0) (0,0,1,1) (sp+30..+6c), and *005392d0
    // (V+10e0) |= 4; 00415eb8..00415ed8 restore the mode word and pop.
    IeDrawState* st = st_;
    menu_depth_ = 0;  // port-only (132ws5)
    std::uint32_t saved_mode = 0;
    if (st) {
        saved_mode = st->mode;
        st->push();
        st->top() = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1};
        st->dirty = 1;
        st->mode = saved_mode | 4;
    }
    for (const Widget* c : canvas.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);  // 0037a1e0 then +44's +30
    if (st) {
        st->mode = saved_mode;
        st->dirty = 1;
        --st->depth;
    }
}

void GuiDrawer::draw_widget(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    // Port-only (checkpoint 132ws5): the batches of each shown screen's element (a canvas child's child, with its
    // subtree) get one GuiDrawBatch::port_menu tag for the wide menu draw.
    struct MenuTag {
        GuiDrawList& out;
        int& depth;
        std::uint32_t& tag;
        std::size_t from;
        bool element;
        ~MenuTag() {
            --depth;
            if (!element || from == out.batches.size()) return;
            ++tag;
            for (std::size_t k = from; k < out.batches.size(); ++k)
                if (out.batches[k].port_menu == 0) out.batches[k].port_menu = tag;
        }
    } menu_tag{out, menu_depth_, menu_tag_, out.batches.size(), menu_depth_ == 1};
    ++menu_depth_;
    // +44 is the decorator when the widget has one: 00417340 draws its IE
    // element (GuiDecor, Checkpoint 122b) before the widget's own +30.
    if (decor_ && st_ && w.deco) {
        OMEGA_ZONE("draw.decor");
        decor_->draw(out, *st_, w);
    }
    if (&w == game_screen) {
        // 0045d8d0: nothing of the screen (its timer, interact prompt, ...) draws while the HUD's +6a8 (the
        // start menu open, 002429f0 / 00242900) is set. Otherwise 0041e470 (the children, with the shown
        // "weaponselect" +6c and its panels), then, while the weapon select is open (+d4 == 1), 0045c0f0's
        // round counts. 0045d7f0 / 0045d710 around the children (the panels' fade, 0x00440e74) are the
        // weapon display draw's alpha in the port (weapon_display_draw reads each panel's +54).
        if (gui.g.controller_6a8) return;
        // Port-only (132fx1): the weapon select's batches follow the HUD weapon icon under the wide aspect.
        const auto follow = [&](std::size_t from) {
            if (!port_weapon_select_anchor) return;
            for (std::size_t k = from; k < out.batches.size(); ++k)
                if (!out.batches[k].port_anchor) out.batches[k].port_anchor = port_weapon_select_anchor;
        };
        for (std::size_t child = 0; child < w.children.size(); ++child) {
            const Widget* c = w.children[child];
            if (c->flags & flag_visible) {
                const std::size_t from = out.batches.size();
                draw_widget(out, gui, *c, now_ms);
                if (c == gui.gs.ws) {
                    follow(from);
                } else {
                    const auto anchor = port_anchor_widget | static_cast<std::uint32_t>(child + 1);
                    for (std::size_t k = from; k < out.batches.size(); ++k)
                        if (!out.batches[k].port_anchor) out.batches[k].port_anchor = anchor;
                }
            }
        }
        if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
        if (gui.gs.ws && (gui.gs.ws->flags & flag_visible) && gui.gs.d4 == 1 && gui.gs.text) {
            const std::size_t from = out.batches.size();
            draw_game_screen_counts(out, gui, w, now_ms);
            follow(from);
        }
        return;
    }
    switch (w.cls) {
    case WidgetClass::text:
    case WidgetClass::button:
        draw_text_widget(out, gui, w, now_ms);  // 0041c150
        return;
    case WidgetClass::edit:
        draw_text_widget(out, gui, w, now_ms);  // 0041a450: 0041c150, then the cursor
        draw_edit_cursor(out, gui, w);
        return;
    case WidgetClass::textlist:
        draw_text_list(out, gui, w, now_ms);  // 00420f90
        return;
    case WidgetClass::mltext:
    case WidgetClass::chatwin:  // GuiChatWindow (004ddce0): GuiMLTextWidget's +30 (132ln4)
        draw_ml_text(out, gui, w, now_ms);  // 0043d8b0
        return;
    case WidgetClass::missionlist:  // GuiMissionList (004ddb80): 00423670 with +64 002ab2a0 / +68 002aad40 (132ln4)
        draw_mission_list(out, gui, w, now_ms);
        return;
    case WidgetClass::netjoin:  // GuiNetJoinMission (004dda50): +30 is 0041e470 (132ln4)
    case WidgetClass::netcontacts:  // GuiNetContacts (004ddc20): +30 is 0041e470 (132on2h)
        break;
    case WidgetClass::netmsgarray:  // GuiNetMessageArray (004deb80): 00423670 with +64 00347b40 / +68 00348530 (132on2c)
        draw_net_message_array(out, gui, w, now_ms);  // the rows and cells (132on2h)
        return;
    case WidgetClass::briefing:  // 00455db0: 0043e1a0(+48, enabled ? 00587d20 : 00587d30), then 0041e470
        if (w.briefing_text) ml_colour_[w.briefing_text] = GuiRuntime::br_text_colour(w.flags);
        break;
    case WidgetClass::widget:
    case WidgetClass::title:
    case WidgetClass::canvas:
    case WidgetClass::msgbox:
    case WidgetClass::keyboard:
    case WidgetClass::agentload:
    case WidgetClass::onoffsel:
    case WidgetClass::cmdcentr:   // vtable 004dd4f0 +30 is 0041e470 too
    case WidgetClass::onlinecn:   // GuiOnlineConnect (004dd620): +30 is 0041e470 (132ln2)
    case WidgetClass::msnsetup:   // GuiMissionSetup (004dd730): +30 is 0041e470 (132ln3)
    case WidgetClass::acntlogn:   // GuiAccountLogin (004dd120): +30 is 0041e470 (132on2b)
    case WidgetClass::onlinepref:  // GuiOnlineOptions (004ddaf0): +30 is 0041e470 (132on2c)
    case WidgetClass::netmess:     // GuiNetMessages (004deb10): +30 is 0041e470 (132on2c)
    case WidgetClass::objectives:  // GuiObjectives / GuiStartMenu / GuiStartMenuBackdrop: 0041e470 (checkpoint 132f)
    case WidgetClass::startmenu:
    case WidgetClass::startback:
    case WidgetClass::slider:   // GuiSliderWidget (004e10c0) / GuiGameOptions (004dd580): +30 is 0041e470 (132f2)
    case WidgetClass::gameopt:
    case WidgetClass::scrollbar:  // GuiScrollBarWidget (004e0f40): +30 is 0041e470 (132f3)
    case WidgetClass::radiolog:   // GuiRadioLog (004e1290): +30 is 0041e470 (132f3)
    case WidgetClass::equippane:  // GuiEquipmentPane (004e12f0): +30 is 0041e470 (132f3b)
    case WidgetClass::port_cheats:  // port-only (132f5): GuiGameOptions' 0041e470 draw
    case WidgetClass::port_settings:  // port-only (132win11): likewise
    case WidgetClass::agentsave:    // GuiAgentSave (004dd3a0): +30 is 0041e470 (132cc5)
    case WidgetClass::endobjectives:  // GuiEndGameObjectives (004e17c0) / GuiEndGameAwards (004e1730) /
    case WidgetClass::endawards:      // GuiAutoSaver (004e16d0): +30 is 0041e470 (132f11)
    case WidgetClass::autosaver:
    case WidgetClass::missionstats:  // GuiMissionStatistics (004dee70): +30 is 0041e470 (132f12)
    case WidgetClass::dossiers:      // GuiDossiers / GuiRanks / GuiOmegaStrain / GuiAchievementScreen /
    case WidgetClass::ranks:         // GuiPersonnelMenu: +30 is 0041e470 (132f13)
    case WidgetClass::omega:
    case WidgetClass::achieve:
    case WidgetClass::pmenu:
        break;
    case WidgetClass::ratings:  // GuiRatings 0034d8c0: 0041e470, then the percentage cells
        draw_ratings(out, gui, w, now_ms);
        return;
    case WidgetClass::textarray:
    case WidgetClass::weaponstats:
    case WidgetClass::missionstatlist:  // GuiMissionStatList (004deee0): vt +68 0034fec0
        draw_text_array(out, gui, w, now_ms);  // 00423670 (vt +68 00425ea0 / 00455ae0)
        return;
    case WidgetClass::weapondisplay: {  // 00440580: 0041e470, then the model
        for (const Widget* c : w.children)
            if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
        if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
        if (w.wd->model == 0) return;
        if (!weapon_display_draw) fail(0x440580, "GuiWeaponDisplay model draw (no host)");
        weapon_display_draw(out, st_, w);
        return;
    }
    case WidgetClass::agentmod:   // 00342890: 00373670(D_00535d20) lights the model, then 0041e470 (132cc4b)
    case WidgetClass::agentcreate:  // 0028e600: the same 00373670(D_00535d20), then 0041e470 (132cc5)
    case WidgetClass::agentinfo:    // GuiAgentInfo (004df350): 0041e470 (132f14)
        break;
    case WidgetClass::agentdisplay: {  // 00361e70: 0041e470, then the actor through the +50 camera (132f14)
        for (const Widget* c : w.children)
            if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
        if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
        if (!agent_display_draw) fail(0x361e70, "GuiAgentDisplay model draw (no host)");
        agent_display_draw(out, st_, w);
        return;
    }
    case WidgetClass::subtitle:
        draw_subtitle(out, gui, w, now_ms);  // 00457ae0
        return;
    case WidgetClass::slideshow:  // 00327910: the (hidden) bitmap_frame's +44 +30, then 0041e470
        if (w.ss && w.ss->bitmap) draw_widget(out, gui, *w.ss->bitmap, now_ms);
        break;
    case WidgetClass::bitmap:
        draw_bitmap(out, gui, w, now_ms);  // 0043c0f0
        return;
    case WidgetClass::mapscreen:  // 00464a90: "backdark" (+48) +14 |= 2, its +44 +30, +14 &= ~2; 0026ca70(+4c); 0041e470
        // Checkpoint 132map1: the visible bit is set only around backdark's own draw and left clear, so 0041e470
        // (which draws a child only with +14 & 2, 0x0041e4cc) does not draw it a second time over the map.
        if (w.map_backdark) {
            w.map_backdark->flags = static_cast<decltype(w.map_backdark->flags)>(w.map_backdark->flags | flag_visible);
            if (decor_ && st_ && w.map_backdark->deco) decor_->draw(out, *st_, *w.map_backdark);
            w.map_backdark->flags = static_cast<decltype(w.map_backdark->flags)>(w.map_backdark->flags & ~flag_visible);
        }
        if (!map_screen_draw) fail(0x26ca70, "the HUD map draw (no host)");
        map_screen_draw(out, st_, w);
        break;
    case WidgetClass::scoresequip:
    case WidgetClass::endstats:  // GuiEndGameStatistics (004e1840): +30 is GuiScoresEquip's 0035bf70 (132f11)
        // 0035bf70: 00373670(00535d20), the light table only the figure
        // render 0035b390 reads (character_display_draw), then 0041e470.
        break;
    case WidgetClass::eqmodify:  // GuiEquipmentModify 002c0380: the same 00373670(00535d20), then 0041e470
        // 132ws6: in game state 0x12 after the level load, over the level's pools; the level's first world
        // update reads the last such table.
        if (light_table_rebuild) light_table_rebuild();
        break;
    case WidgetClass::chardisplay: {  // 00326810: 0041e470, then the +48 callback
        for (const Widget* c : w.children)
            if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
        if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
        if (!w.cd_screen) return;
        // GuiAgentModify's 00345020 (the model draw 00341f00), GuiAgentCreate's 00290bc0 (0028d870),
        // GuiScoresEquip's 0035b390: the host's figure draw (Checkpoint 132cc6 for the agent screens).
        if (!character_display_draw) {
            if (w.cd_screen->cls == WidgetClass::agentmod) fail(0x341f00, "GuiAgentModify model draw (no host)");
            if (w.cd_screen->cls == WidgetClass::agentcreate) fail(0x28d870, "GuiAgentCreate model draw (no host)");
            fail(0x35b390, "GuiScoresEquip figure draw (no host)");
        }
        character_display_draw(out, st_, *w.cd_screen, w);
        return;
    }
    default:
        fail(0x41e470, "widget class draw (class " + std::to_string(static_cast<int>(w.cls)) + ")");
    }
    // 0041e470
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
}

namespace {
std::int32_t state_width(const IeTextObject& obj, void* context) {
    const auto* font = static_cast<const GuiFont*>(context);
    const std::string s = obj.text.value_or(std::string());
    std::vector<std::uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    return gui_font_width(*font, b, 0x200);
}
}  // namespace

void GuiDrawer::draw_text_widget(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    // 0041c15c: the base draw first
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
    if (!w.obj) return;
    const TextObject& o = *w.obj;
    GuiTextDraw t;
    std::array<std::int32_t, 4> rect{};
    // The native runtime models no text fade (+59 stays clear).
    gui_text_widget_object(w.flags, w.rect, {w.rgb[0], w.rgb[1], w.rgb[2], w.alpha}, false, rect, t.colour);
    draw_object(out, &w, o, rect, t.colour, now_ms);
}

// 0041a450 after 0041c150: with a text object (+78) and either +88 set or the
// widget the focus (004147a0()->+20), the cursor 003cf3a0 places at +8c
// draws as a two-vertex line from (x, y) to (x, y - font height) (0036a2f0 of
// +54) in the pulse colour 0041a8f0 keeps (005720e0), under a pushed
// identity top with render mode 0, prim 2 and V+104c 0x62 (the 003701b0
// flush). The copy of 00539170 into the current camera record (+0..+c,
// +105 / +106 = 1) is not modelled: no hosted draw reads it.
void GuiDrawer::draw_edit_cursor(GuiDrawList& out, const GuiRuntime& gui, const Widget& w) {
    if (!w.obj) return;
    if (!w.e88 && gui.canvas().focus != &w) return;
    const TextObject& o = *w.obj;
    GuiTextDraw t;
    std::array<std::int32_t, 4> rect{};
    gui_text_widget_object(w.flags, w.rect, {w.rgb[0], w.rgb[1], w.rgb[2], w.alpha}, false, rect, t.colour);
    t.x = rect[0];
    t.y = rect[1];
    t.w = rect[2];
    t.h = rect[3];
    t.b10 = o.b10;
    t.b11 = o.b11;
    t.b12 = o.b12;
    t.b58 = o.b58;
    t.b5b = o.b5b;
    t.f54 = o.w54 ? std::bit_cast<float>(o.w54) : k_f54;
    t.text = o.text;
    if (o.font) {
        t.font = fonts_(o.font);
        if (!t.font) fail(0x418420, "font handle without a hosted font");
    }
    float x = 0.0F, y = 0.0F;
    if (!gui_text_cursor(t, w.cursor, x, y)) return;
    const GuiFont* f = fonts_(w.font);
    if (!f) fail(0x36a2f0, "font handle without a hosted font");
    const float h = static_cast<float>(f->height());
    GuiDrawBatch b = out.take();
    b.flush = 0x3701b0;
    b.render_mode = 0;
    b.prim = 2;
    b.strip_max = 0x62;
    b.v8 = std::uint64_t{2} << 32;
    b.strips = {2};
    std::array<std::int32_t, 4> c{};
    for (int k = 0; k < 4; ++k) c[k] = cvt_w(fmul(128.0F, gui.blink_rgba[k]), 0x25c9e4);  // 0025c9d0
    GuiVertex v0, v1;
    v0.pos = {x, y, 1.0F, 0.0F};
    v1.pos = {x, fsub(y, h), 1.0F, 0.0F};
    v0.colour = v1.colour = c;
    b.vertices = {v0, v1};
    out.batches.push_back(std::move(b));
}

// 003d08b0 (the text, resetting the draw state when it changes) then 003cfab0
// on a text object whose rect (+0..+c) and colour (+30..+3c) are set.
void GuiDrawer::draw_object(GuiDrawList& out, const Widget* key, const TextObject& o,
                            const std::array<std::int32_t, 4>& rect, const std::array<float, 4>& colour,
                            std::int32_t now_ms, const GuiFont* font) {
    GuiTextDraw t;
    t.colour = colour;
    t.x = rect[0];
    t.y = rect[1];
    t.w = rect[2];
    t.h = rect[3];
    t.b10 = o.b10;
    t.b11 = o.b11;
    t.b12 = o.b12;
    t.b58 = o.b58;
    t.b5b = o.b5b;
    // +54: the keyboard's write, else 003d0940's 0.6 (TextObject::w54 == 0
    // is "not written" in the runtime model).
    t.f54 = o.w54 ? std::bit_cast<float>(o.w54) : k_f54;
    t.text = o.text;
    if (font) {
        t.font = font;
    } else if (o.font) {
        t.font = fonts_(o.font);
        if (!t.font) fail(0x418420, "font handle without a hosted font");
    }
    // 003cfab0's state part (ie_text_draw_state): the per-object clock and
    // scroll offset, reset as 003d08b0 does whenever the text changes.
    IeTextObject st;
    auto it = text_.find(key);
    if (it == text_.end() || it->second.text != o.text) {
        st.w = t.w;
        ie_text_set(st, o.text);
    } else {
        st.text = it->second.text;
        st.f40 = it->second.f40;
        st.f44 = it->second.f44;
        st.byte_5a = it->second.b5a;
    }
    st.x = t.x;
    st.y = t.y;
    st.w = t.w;
    st.h = t.h;
    st.word_20 = o.font;
    st.byte_10 = t.b10;
    st.colour = t.colour;
    ie_text_draw_state(st, now_ms, t.font ? state_width : nullptr, const_cast<GuiFont*>(t.font));
    text_[key] = {o.text, st.f40, st.f44, st.byte_5a};
    t.f40 = st.f40;
    gui_text_draw(out, t);
}

// 0045c0f0: per shown panel (from +90, at most +dc) holding a weapon, its rounds - "%d" of the spare
// 00143870 for hold classes 1 / 7, else "%d/%d" of the clip 00143940 and spare when 00143710 says it has a
// clip - through the screen's one text object +48 (003d08b0), sized to the panel (+3c / +40), under a pushed
// identity top: first the shadow (0, 0, 0, 1 - +bc[i]) at (x + 1, y - 1), then the colour 00587d80
// (1, 1, 1, 1) with alpha * (1 - +bc[i]) at the panel's (x, y) (003d0810 then 003cfab0 each).
void GuiDrawer::draw_game_screen_counts(GuiDrawList& out, const GuiRuntime& gui, const Widget& w,
                                        std::int32_t now_ms) {
    for (const auto& c : gui.gs_counts_c0f0()) {
        TextObject o = *gui.gs.text;
        o.text = c.text;  // 003d08b0
        o.rect[2] = cvt_w(c.rect[2], 0x45c258);
        o.rect[3] = cvt_w(c.rect[3], 0x45c25c);
        IeDrawState* st = st_;
        if (st) {
            st->push();
            st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            st->dirty = 1;
        }
        const float a = fsub(1.0F, c.bc);
        const std::array<std::int32_t, 4> shadow = {cvt_w(fadd(1.0F, c.rect[0]), 0x45c3b0),
                                                    cvt_w(fsub(c.rect[1], 1.0F), 0x45c3b4), o.rect[2], o.rect[3]};
        draw_object(out, &w, o, shadow, {0.0F, 0.0F, 0.0F, a}, now_ms);
        const std::array<std::int32_t, 4> main = {cvt_w(c.rect[0], 0x45c3e8), cvt_w(c.rect[1], 0x45c3ec), o.rect[2],
                                                  o.rect[3]};
        draw_object(out, &w, o, main, {1.0F, 1.0F, 1.0F, fmul(1.0F, a)}, now_ms);
        if (st) {
            st->dirty = 1;
            --st->depth;
        }
    }
}

// 0043d8b0: the children (0041e470), then under a pushed identity matrix the
// lines from +68 down, at most +64 of them (at least one), each through the
// one text object at the widget's x and a y that steps down by the font
// height. The font's colour 0 (+10) is +50 while the lines draw: a line's
// first byte is its colour code (0x10 + code).
void GuiDrawer::draw_ml_text(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
    const auto& m = *w.ml;
    if (!m.obj || !m.font) return;
    IeDrawState* st = st_;
    if (st) {
        st->push();
        st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        st->dirty = 1;
    }
    const GuiFont* f = fonts_(m.font);
    if (!f) fail(0x36a2f0, "font handle without a hosted font");
    const auto fh = static_cast<std::int32_t>(f->height());
    std::int32_t y = cvt_w(w.rect[1], 0x43d9c8);
    std::array<float, 4> colour = m.colour;
    std::array<float, 4> obj_colour = m.obj->rgba;
    if (auto it = ml_colour_.find(&w); it != ml_colour_.end()) colour = obj_colour = it->second;
    if (m.obj->b59) fail(0x3d0810, "ML text object fading (+59)");
    GuiFont font = *f;
    std::memcpy(font.image.data() + 0x10, colour.data(), sizeof colour);
    std::int32_t drawn = 1;
    for (std::size_t i = 0; i < m.lines.size(); ++i) {
        if (static_cast<std::int32_t>(i) < m.top) continue;
        TextObject o = *m.obj;
        o.text = m.lines[i];  // 003d08b0
        const std::array<std::int32_t, 4> rect = {cvt_w(w.rect[0], 0x43da54), y, o.rect[2], o.rect[3]};
        draw_object(out, &w, o, rect, obj_colour, now_ms, &font);  // 003cfab0
        y -= fh;
        if (!(drawn < m.per_page)) break;
        ++drawn;
    }
    if (st) {
        st->dirty = 1;
        --st->depth;
    }
}

// 0034d8c0: the object's rect is the bitmap's (cvt.w.s of +34..+40), its text 003d08b0, then 003cfab0.
void GuiDrawer::draw_ratings(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
    if (!w.ps || !w.ps->pct_text) return;
    IeDrawState* st = st_;
    if (st) {
        st->push();
        st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        st->dirty = 1;
    }
    for (const auto& cell : gui.rt_cells(&w)) {
        TextObject o = *w.ps->pct_text;
        const auto& r = cell.bitmap->rect;
        o.rect = {cvt_w(r[0], 0x34da28), cvt_w(r[1], 0x34da30), cvt_w(r[2], 0x34da54), cvt_w(r[3], 0x34da58)};
        o.text = cell.text;  // 003d08b0
        draw_object(out, &w, o, o.rect, o.rgba, now_ms);  // 003cfab0
    }
    if (st) {
        st->dirty = 1;
        --st->depth;
    }
}

// 0043c0f0 (GuiBitmapWidget +30): the base draw, then the slot's picture as
// one textured quad over the widget's rect under an identity matrix push.
void GuiDrawer::draw_bitmap(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);  // 0041e470
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
    if (!w.bm || w.bm->texture == -1) return;
    const auto& b = *w.bm;
    GuiDrawBatch batch = out.take();
    batch.render_mode = 5;
    batch.slot = static_cast<std::uint32_t>(b.texture);  // 003820e0
    batch.prim = 3;
    batch.strip_max = 0x40;
    batch.v8 = 6ULL << 32;
    // A 1024 x 512 slot (the ZEUS pictures) shows its 640 x 480 corner.
    float u = 1.0F, v = 1.0F;
    if (b.width == 1024 && b.height == 512) {
        u = 0.625F;
        v = 0.9375F;
    }
    std::array<std::int32_t, 4> colour{};
    for (int k = 0; k < 4; ++k) colour[k] = cvt_w(fmul(128.0F, b.rgba[k]), 0x25c9e4);  // 0025c9d0
    const float base = 2048.0F;  // 0025f220
    const float x = w.rect[0], y = w.rect[1];
    batch.strips.push_back(2);  // 0025cd10, two 0025ca40 vertices
    batch.vertices.push_back({{x, y, b.z, 0.0F}, colour, {base, base, 1.0F, 0.0F}});
    batch.vertices.push_back(
        {{fadd(x, w.rect[2]), fsub(y, w.rect[3]), b.z, 0.0F}, colour, {fadd(u, base), fadd(v, base), 1.0F, 0.0F}});
    out.batches.push_back(std::move(batch));  // 00370550
}

// 00457ae0 (GuiSubTitleDisplay): with a text (+50) and lines (+5c), under a
// pushed identity matrix, the segments typed out at 20 characters a second
// of +54 after their pauses; with a line count (+78) only the last lines up
// to the clock, the oldest fading (f21) and the block sliding up as the next
// line starts. Then the children (0041e470).
void GuiDrawer::draw_subtitle(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    const auto& s = *w.st;
    if (s.str && !s.lines.empty() && s.obj) {
        IeDrawState* st = st_;
        if (st) {
            st->push();
            st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            st->dirty = 1;
        }
        const auto& times = s.times;
        const auto ntimes = static_cast<std::int32_t>(times.size());
        float f21 = 1.0F;
        std::int32_t s4 = 0;
        std::int32_t s3 = static_cast<std::int32_t>(s.lines.size()) - 1;
        if (s.vis) {
            s3 = 0;
            std::int32_t a1 = ntimes - 1;
            for (; a1 > 0; --a1) {
                if (times[static_cast<std::size_t>(a1 - 1)] <= s.time) {  // c.ole.s
                    s3 = a1;
                    s4 = a1 - (s.vis - 1);
                    if (s4 < 0) s4 = 0;
                    break;
                }
            }
            if (a1 == ntimes - 1) {
                f21 = 1.0F;
            } else if (a1 == 0) {
                const float t0 = times[0];
                f21 = fdiv(fsub(t0, s.time), t0);
            } else {
                const float t1 = times[static_cast<std::size_t>(a1)];
                const float tp = times[static_cast<std::size_t>(a1 - 1)];
                f21 = fdiv(fsub(t1, s.time), fsub(t1, tp));
            }
        }
        float f20 = s4 ? times[static_cast<std::size_t>(s4 - 1)] : 0.0F;
        const bool full = s.vis && !((s3 - s4) < s.vis - 1);
        const GuiFont* f = fonts_(s.font);
        if (!f && full) fail(0x36a2f0, "font handle without a hosted font");
        bool done = false;
        for (std::int32_t s2 = s4; s2 <= s3 && !done; ++s2) {
            for (const auto& seg : s.lines[static_cast<std::size_t>(s2)]) {
                f20 = fadd(f20, seg.delay);
                if (!(f20 <= s.time)) {
                    done = true;
                    break;
                }
                std::int32_t k = cvt_w(fmul(20.0F, fsub(s.time, f20)), 0x457d70);
                if (k <= 0) {
                    done = true;
                    break;
                }
                if (!(k < seg.count)) k = seg.count;
                // 00129a70(sp+a0, +50 + off, k): the buffer runs to sp+1a4.
                if (k >= 0x104) fail(0x457dbc, "a subtitle segment past the 260-byte draw buffer");
                const std::string& str = *s.str;
                const auto off = static_cast<std::size_t>(seg.off);
                std::string text = off < str.size() ? str.substr(off, static_cast<std::size_t>(k)) : std::string();
                text = text.substr(0, text.find('\0'));
                std::int32_t y = cvt_w(fsub(w.rect[1], static_cast<float>(seg.y)), 0x457de4);
                if (full) {
                    const float fh = static_cast<float>(f->height());  // 0036a2f0
                    // mula.s / madd.s: s4 * fh + (1 - f21) * fh
                    const float shift = fadd(fmul(static_cast<float>(s4), fh), fmul(fsub(1.0F, f21), fh));
                    y = cvt_w(fadd(static_cast<float>(y), shift), 0x457e40);
                }
                const std::int32_t x = cvt_w(fadd(w.rect[0], static_cast<float>(seg.x)), 0x457e60);
                std::array<float, 4> colour = seg.rgba;
                if (s2 == s4 && full) colour[3] = f21;
                TextObject o = *s.obj;
                o.text = text;  // 003d0810 the colour, 003d08b0 the text, 003cfab0
                draw_object(out, &w, o, {x, y, o.rect[2], o.rect[3]}, colour, now_ms);
                f20 = fadd(f20, fdiv(static_cast<float>(k), 20.0F));
            }
        }
        if (st) {
            st->dirty = 1;
            --st->depth;
        }
    }
    // 0041e470
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
}

// 00420f90: the matrix push with an identity top, 00423670, the pop.
void GuiDrawer::draw_text_list(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    IeDrawState* st = st_;
    if (st) {
        st->push();
        st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        st->dirty = 1;
    }
    const auto& l = *w.list;
    auto colw = [&](std::uint32_t c, std::uint32_t address) {
        if (c >= l.colw.size()) fail(address, "column width read past the +60 vector");
        return i2f(l.colw[c], address);
    };
    const float row_h = static_cast<float>(l.row_h);  // the unsigned cvt.s.w pair
    // 00423670 loop 1: the column draws (vt +60 00423660: nothing).
    float total = 0.0F;  // loop 2's width: the visible columns' sum
    for (std::uint32_t c = 0; c < l.vis_cols; ++c) total = fadd(total, colw(l.left + c, 0x4237dc));
    // loop 2: the row draws (vt +64 00423610).
    float y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        std::uint32_t row = l.top + r;
        if (l.no_scroll) fail(0x423be0, "GuiArrayWidget row map (+82)");
        if (row >= l.rows) break;
        if ((w.flags & 0x100) && row == l.row) fail(0x424f60, "GuiTextListWidget selected row highlight");
        y = fsub(y, row_h);
    }
    // loop 3: the cells (vt +68 00420e50).
    y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        float x = w.rect[0];
        for (std::uint32_t c = 0; c < l.vis_cols; ++c) {
            const std::uint32_t col = l.left + c;
            const float cw = l.cols == 1 ? w.rect[2] : colw(col, 0x423a7c);
            const std::uint32_t row = l.top + r;
            if (l.obj && row < l.items.size()) {  // 00420e50
                const auto& item = l.items[row];
                std::array<float, 4> colour;
                if (row == l.row && l.f0) {
                    colour = l.rgba_e0;
                } else {
                    switch (item.style) {
                    case 0: colour = l.rgba; break;
                    case 1: colour = l.rgba_c0; break;
                    case 2: colour = l.rgba_d0; break;
                    case 3: colour = l.rgba_e0; break;
                    default: fail(0x420ecc, "list item style past the four colours");
                    }
                }
                if (l.obj->b59) fail(0x3d0810, "list text object fading (+59)");
                TextObject o = *l.obj;
                o.text = item.text;
                const std::array<std::int32_t, 4> rect = {cvt_w(x, 0x420ef8), cvt_w(y, 0x420efc), cvt_w(cw, 0x420f1c),
                                                          cvt_w(row_h, 0x420f20)};
                draw_object(out, &w, o, rect, colour, now_ms);
            }
            x = fadd(x, colw(col, 0x423b48));
        }
        y = fsub(y, row_h);
    }
    // 00423ba0: 0041e470, the children (the "cursor"), still under the pushed matrix
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
    if (st) {
        st->dirty = 1;
        --st->depth;
    }
}

// 002ab2e4 (GuiMissionList's row draw) and its byte-identical copy 00347b88 (GuiNetMessageArray's; its colour
// 00587c30 holds the same cyan as 00587c60): the selection bar, under a pushed identity top with render mode 0
// (*005392d0 kept around it): the rect inset by one unit, four vertices in cyan, alpha 0.5, flushed by 00371200.
// The reachable form (a0 = 1 at 002ab2f4 / 00347b9c) is the four-vertex strip; the outline (five vertices through
// 003701b0) is not taken.
void GuiDrawer::draw_selection_bar(GuiDrawList& out, const Widget& w, float y, float row_w, float row_h,
                                   std::uint32_t source) {
    const float x0 = fadd(w.rect[0], 1.0F), y0 = fsub(y, 1.0F);
    const float bw = fsub(row_w, 2.0F), bh = fsub(row_h, 2.0F);
    const std::array<float, 4> cyan = {0.0F, 1.0F, 1.0F, std::bit_cast<float>(0x3f008081U)};
    std::array<std::int32_t, 4> c{};
    for (int k = 0; k < 4; ++k) c[k] = cvt_w(fmul(128.0F, cyan[k]), 0x25c9e4);  // 0025c9d0
    GuiDrawBatch b = out.take();
    b.flush = 0x371200;
    b.render_mode = 0;
    b.prim = 2;          // V+1048: render mode & 1 ? 3 : 2
    b.strip_max = 0x62;  // V+104c: (render mode & 1 ? 0x41 : 0x62) & ~1
    b.v8 = std::uint64_t{2} << 32;
    b.strips = {4};
    b.source = source;
    b.breaks.push_back({});
    b.breaks.back().fill(b.prim - 1);
    const std::array<std::array<float, 2>, 4> p = {{{x0, y0},
                                                    {fadd(x0, bw), y0},
                                                    {x0, fsub(y0, bh)},
                                                    {fadd(x0, bw), fsub(y0, bh)}}};
    for (const auto& q : p) {
        GuiVertex v;
        v.pos = {q[0], q[1], 1.0F, 0.0F};
        v.colour = c;
        b.vertices.push_back(v);
    }
    out.batches.push_back(std::move(b));
}

// Checkpoint 132on2h: 00423670 with GuiNetMessageArray's +64 00347b40 and +68 00348530 (NETMESS' message rows).
// The row draw: 00423610, then the selection bar when the row is the selected one (+54) and inside the rows (+4c),
// without GuiMissionList's focus test. The cell draw (GuiRuntime::nma_cell): column 0 moves the type's icon widget to
// the cell's centre (0041eba0: its rect and the decorator event 0x12) and draws its decorator (+44 vt +30); columns
// 1 and 3 draw the text object +a0 at the cell's rect under a pushed identity top (003d0810 / 003cfab0).
void GuiDrawer::draw_net_message_array(GuiDrawList& out, const GuiRuntime& gui, const Widget& w,
                                       std::int32_t now_ms) {
    const auto& l = *w.list;
    const auto& m = *w.nma;
    auto colw = [&](std::uint32_t c, std::uint32_t address) {
        if (c >= l.colw.size()) fail(address, "column width read past the +60 vector");
        return i2f(l.colw[c], address);
    };
    const float row_h = static_cast<float>(l.row_h);
    float row_w = 0.0F;
    for (std::uint32_t c = 0; c < l.vis_cols; ++c) row_w = fadd(row_w, colw(l.left + c, 0x4238dc));
    float y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        const std::uint32_t row = l.top + r;
        if (l.no_scroll) fail(0x423be0, "GuiArrayWidget row map (+82)");
        if (row >= l.rows) break;
        if ((w.flags & 0x100) && row == l.row) fail(0x424f60, "GuiArrayWidget selected row highlight (00423610)");
        if (row == l.row && row < l.rows) draw_selection_bar(out, w, y, row_w, row_h, 0x347b40);
        y = fsub(y, row_h);
    }
    y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        float x = w.rect[0];
        for (std::uint32_t cc = 0; cc < l.vis_cols; ++cc) {
            const std::uint32_t col = l.left + cc;
            const float cw = l.cols == 1 ? w.rect[2] : colw(col, 0x423a7c);
            const std::uint32_t row = l.top + r;
            const auto cell = gui.nma_cell(w, col, row);
            if (cell && col == 0) {
                if (Widget* icon = cell->icon) {  // 00348600
                    const float ix = fadd(x, fsub(fdiv(cw, 2.0F), fdiv(icon->rect[2], 2.0F)));
                    const float iy = fsub(y, fsub(fdiv(row_h, 2.0F), fdiv(icon->rect[3], 2.0F)));
                    icon->rect[0] = ix;  // 0041eba0
                    icon->rect[1] = iy;
                    if (!icon->deco) fail(0x34864c, "GuiNetMessageArray's icon without a decorator (+44)");
                    // 004176c0 (the decorator's event 0x12 with 0041eba0's cvt.w.s position)
                    icon->deco->matrix[12] = fadd(static_cast<float>(cvt_w(ix, 0x41ebbc)), fdiv(icon->rect[2], 2.0F));
                    icon->deco->matrix[14] = fsub(static_cast<float>(cvt_w(iy, 0x41ebc8)), fdiv(icon->rect[3], 2.0F));
                    if (decor_ && st_) decor_->draw(out, *st_, *icon);  // +44 vt +30
                }
            } else if (cell) {
                IeDrawState* st = st_;
                if (st) {  // 00348668: the pushed identity top
                    st->push();
                    st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    st->dirty = 1;
                }
                TextObject o = *m.obj;
                const std::array<std::int32_t, 4> rect = {cvt_w(x, 0x348724), cvt_w(y, 0x348728),
                                                          cvt_w(cw, 0x348748), cvt_w(row_h, 0x34874c)};
                o.rect = rect;
                o.b58 = 1;
                o.text = cell->text;  // 003d08b0 (a NULL text for a type past 11: empty)
                o.b10 = cell->halign;
                o.b5b = cell->b5b;
                if (o.b59) fail(0x3d0810, "GuiNetMessageArray's text object fading (+59)");
                draw_object(out, &w, o, rect, cell->rgba, now_ms);  // 003cfab0
                if (st) {
                    st->dirty = 1;
                    --st->depth;
                }
            }
            x = fadd(x, colw(col, 0x423b48));
        }
        y = fsub(y, row_h);
    }
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);  // 0041e470
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
}

// Checkpoint 132ln4: 00423670 (GuiArrayWidget's +30: loop 1 the columns vt +60 (00423660: nothing), loop 2 the rows
// vt +64, loop 3 the cells vt +68, then 0041e470) with GuiMissionList's +64 002ab2a0 and +68 002aad40.
void GuiDrawer::draw_mission_list(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    const auto& l = *w.list;
    const auto& m = *w.mls;
    auto colw = [&](std::uint32_t c, std::uint32_t address) {
        if (c >= l.colw.size()) fail(address, "column width read past the +60 vector");
        return i2f(l.colw[c], address);
    };
    const float row_h = static_cast<float>(l.row_h);
    // the rows' width: the visible columns' widths (004237a0..004238f4)
    float row_w = 0.0F;
    for (std::uint32_t c = 0; c < l.vis_cols; ++c) row_w = fadd(row_w, colw(l.left + c, 0x4238dc));
    // loop 2: the row draws (vt +64 002ab2a0) at (x, y, row width, row height)
    float y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        const std::uint32_t row = l.top + r;
        if (l.no_scroll) fail(0x423be0, "GuiArrayWidget row map (+82)");
        if (row >= l.rows) break;
        if ((w.flags & 0x100) && row == l.row) fail(0x424f60, "GuiArrayWidget selected row highlight (00423610)");
        if (gui.canvas().focus == &w && row == l.row)
            draw_selection_bar(out, w, y, row_w, row_h, 0x2ab2a0);
        y = fsub(y, row_h);
    }
    // loop 3: the cells (vt +68 002aad40) at (x, y, column width, row height)
    y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        float x = w.rect[0];
        for (std::uint32_t cc = 0; cc < l.vis_cols; ++cc) {
            const std::uint32_t col = l.left + cc;
            const float cw = l.cols == 1 ? w.rect[2] : colw(col, 0x423a7c);
            const std::uint32_t row = l.top + r;
            const auto cell = gui.mlist_cell(w, col, row);
            if (cell) {
                IeDrawState* st = st_;
                if (st) {  // 002aae50: the pushed identity top
                    st->push();
                    st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    st->dirty = 1;
                }
                TextObject o = *m.obj;
                o.b58 = 1;
                o.text = cell->text;  // 003d08b0
                o.b5b = cell->b5b;
                const std::array<std::int32_t, 4> rect = {cvt_w(x, 0x2aaf1c), cvt_w(y, 0x2aaf20), cvt_w(cw, 0x2aaf40),
                                                          cvt_w(row_h, 0x2aaf44)};
                o.rect = rect;
                if (o.b59) fail(0x3d0810, "GuiMissionList's text object fading (+59)");
                draw_object(out, &w, o, rect, cell->rgba, now_ms);  // 003cfab0
                if (st) {
                    st->dirty = 1;
                    --st->depth;
                }
            }
            x = fadd(x, colw(col, 0x423b48));
        }
        y = fsub(y, row_h);
    }
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);  // 0041e470
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
}

// 00423670 as GuiTextArrayWidget / GuiWeaponStats' +30 (no matrix push of
// its own: each cell's 00425ea0 pushes an identity top around its text).
void GuiDrawer::draw_text_array(GuiDrawList& out, const GuiRuntime& gui, const Widget& w, std::int32_t now_ms) {
    const auto& l = *w.list;
    const auto& t = *w.ta;
    auto colw = [&](std::uint32_t c, std::uint32_t address) {
        if (c >= l.colw.size()) fail(address, "column width read past the +60 vector");
        return i2f(l.colw[c], address);
    };
    const float row_h = static_cast<float>(l.row_h);
    // loop 2: the row draws (vt +64 00423610).
    float y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        std::uint32_t row = l.top + r;
        if (l.no_scroll) fail(0x423be0, "GuiArrayWidget row map (+82)");
        if (row >= l.rows) break;
        if ((w.flags & 0x100) && row == l.row) fail(0x424f60, "GuiTextArrayWidget selected row highlight");
        y = fsub(y, row_h);
    }
    // loop 3: the cells (vt +68).
    std::optional<TextObject> obj = t.obj;  // +90: the cell overrides persist on it
    y = w.rect[1];
    for (std::uint32_t r = 0; r < l.vis_rows; ++r) {
        float x = w.rect[0];
        for (std::uint32_t c = 0; c < l.vis_cols; ++c) {
            const std::uint32_t col = l.left + c;
            const float cw = l.cols == 1 ? w.rect[2] : colw(col, 0x423a7c);
            const std::uint32_t row = l.top + r;
            // 00455ae0 (GuiWeaponStats): 00425db0(w, col ? 00587d60 : 00587d50) = +a0 and 003d0810(+90).
            std::array<float, 4> colour = t.rgba;
            if (w.cls == WidgetClass::weaponstats) colour = GuiRuntime::ws_cell_colour(col);
            bool skip = false;
            if (w.cls == WidgetClass::missionstatlist) {  // 0034fec0: 00425db0(w, colour), or no draw at all
                const auto mc = GuiRuntime::msl_cell_colour(w, col, row);
                if (mc) colour = *mc; else skip = true;
            }
            if (obj && !skip) {
                if ((w.cls == WidgetClass::weaponstats || w.cls == WidgetClass::missionstatlist) && !obj->b59)
                    obj->rgba = colour;
                const gui::TextArrayCell* cell = gui.ta_cell(&w, col, row);  // 00426590
                if (cell) {
                    // 00425ef0: the cell's halign (1), font (2) and colour (4, 003d0810: kept while
                    // the object fades) override the object's for this cell; restored after the draw.
                    if (cell->flags & 1) obj->b10 = cell->halign;
                    if (cell->flags & 2) obj->font = cell->font;
                    if ((cell->flags & 4) && !obj->b59) obj->rgba = cell->rgba;
                    if (obj->b59) fail(0x3d0810, "text array object fading (+59)");
                    TextObject o = *obj;
                    o.text = cell->text;  // 001692d0 / 003d08b0, then 003cfab0
                    const std::array<std::int32_t, 4> rect = {cvt_w(x, 0x426008), cvt_w(y, 0x42600c),
                                                              cvt_w(cw, 0x42602c), cvt_w(row_h, 0x426030)};
                    IeDrawState* st = st_;
                    if (st) {
                        st->push();
                        st->top() = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                        st->dirty = 1;
                    }
                    draw_object(out, &w, o, rect, obj->rgba, now_ms);
                    if (st) {
                        st->dirty = 1;
                        --st->depth;
                    }
                    if (cell->flags & 1) obj->b10 = t.halign;
                    if (cell->flags & 2) obj->font = t.font;
                    if ((cell->flags & 4) && !obj->b59) obj->rgba = t.rgba;
                }
            }
            x = fadd(x, colw(col, 0x423b48));
        }
        y = fsub(y, row_h);
    }
    // 00423ba0: 0041e470, the children
    for (const Widget* c : w.children)
        if (c->flags & flag_visible) draw_widget(out, gui, *c, now_ms);
    if ((w.flags & 0x100) && gui.canvas().focus == &w) fail(0x41e560, "focus highlight draw");
}

namespace {
using Mat16 = std::array<float, 16>;
float vmadd(float acc, float a, float b) { return ftz(acc + a * b); }
// The VU product of 00440580's blocks (vf4..7 = first, vf8..11 = second):
// row i = second[i].x * first0 + .y * first1 + .z * first2 + .w * first3.
Mat16 vu_product(const Mat16& first, const Mat16& second) {
    Mat16 r{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t l = 0; l < 4; ++l) {
            float acc = fmul(first[l], second[4 * i]);
            acc = vmadd(acc, first[4 + l], second[4 * i + 1]);
            acc = vmadd(acc, first[8 + l], second[4 * i + 2]);
            r[4 * i + l] = vmadd(acc, first[12 + l], second[4 * i + 3]);
        }
    return r;
}
Mat16 translation(const std::array<float, 4>& row3) {
    Mat16 m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (std::size_t i = 0; i < 4; ++i) m[12 + i] = row3[i];
    return m;
}
}  // namespace

WeaponDisplayMatrices weapon_display_matrices(const std::array<float, 16>& top, const std::array<float, 4>& rect,
                                              const std::array<float, 4>& lo, const std::array<float, 4>& hi,
                                              const std::array<float, 16>& rot, float fade) {
    const float half = 0.5F;
    std::array<float, 4> c{};  // sp+2c0: (min + max) * 0.5, w = 1
    for (std::size_t i = 0; i < 4; ++i) c[i] = fmul(fadd(lo[i], hi[i]), half);
    c[3] = 1.0F;
    // 004405f4..: the rect corners (x, y) and (x + w, y - h) at z = top.row3.z + 200, w 0.
    const float x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    const float z = fadd(200.0F, top[14]);
    const std::array<float, 4> v0{x, y, z, 0.0F}, v1{fadd(x, w), fsub(y, h), z, 0.0F};
    std::array<float, 4> rc{};
    for (std::size_t i = 0; i < 4; ++i) rc[i] = fmul(fadd(v0[i], v1[i]), half);
    std::array<float, 4> ext{};  // min.x <= max.x (checked by the caller's gate): max - min
    for (std::size_t i = 0; i < 4; ++i) ext[i] = fsub(hi[i], lo[i]);
    const float sx = fdiv(w, ext[0]), sy = fdiv(h, ext[1]);
    const float s = sx < sy ? sx : sy;  // min.s
    // The pushed top: rows 0..2 (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, -1, 0); row 3 the old top's
    // xyz += rc, then += c * -s.
    Mat16 m = top;
    const Mat16 axes{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0};
    for (std::size_t i = 0; i < 12; ++i) m[i] = axes[i];
    for (std::size_t i = 0; i < 3; ++i) m[12 + i] = fadd(m[12 + i], rc[i]);
    const float ns = -s;
    for (std::size_t i = 0; i < 3; ++i) m[12 + i] = fadd(m[12 + i], fmul(c[i], ns));
    m = vu_product(m, Mat16{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1});
    const std::array<float, 4> nc{-c[0], -c[1], -c[2], -c[3]};  // vmsubw: every lane negated (w -1)
    m = vu_product(m, translation(c));
    m = vu_product(m, translation(nc));
    WeaponDisplayMatrices out;
    out.top = m;
    out.a2 = vu_product(vu_product(translation(c), rot), translation(nc));
    out.model = vu_product(out.top, out.a2);
    out.alpha_416 = fade == 0.0F ? std::uint8_t{0x80}
                                 : static_cast<std::uint8_t>(cvt_w(fmul(64.0F, fsub(1.0F, fade)), 0x440e74));
    return out;
}

std::array<float, 16> equipment_box_matrix(const std::array<float, 16>& top, const std::array<float, 4>& rect,
                                           const std::array<float, 4>& lo, const std::array<float, 4>& hi) {
    const float half = 0.5F;
    std::array<float, 4> c{};  // sp+210: (min + max) * 0.5, w = 1
    for (std::size_t i = 0; i < 4; ++i) c[i] = fmul(fadd(lo[i], hi[i]), half);
    c[3] = 1.0F;
    // 002bf63c..: the rect corners (x, y) and (x + w, y - h) at z = the top's row3.z (sp+1a8), w 0.
    const float x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    const float z = top[14];
    const std::array<float, 4> v0{x, y, z, 0.0F}, v1{fadd(x, w), fsub(y, h), z, 0.0F};
    std::array<float, 4> rc{};
    for (std::size_t i = 0; i < 4; ++i) rc[i] = fmul(fadd(v0[i], v1[i]), half);
    std::array<float, 4> ext{};
    for (std::size_t i = 0; i < 4; ++i) ext[i] = fsub(hi[i], lo[i]);
    const float sx = fdiv(w, ext[0]), sy = fdiv(h, ext[1]);
    const float s = sx < sy ? sx : sy;  // 002bf71c min.s
    Mat16 m = top;
    const Mat16 axes{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0};  // sp+130
    for (std::size_t i = 0; i < 12; ++i) m[i] = axes[i];
    for (std::size_t i = 0; i < 3; ++i) m[12 + i] = fadd(m[12 + i], rc[i]);
    const float ns = -s;
    for (std::size_t i = 0; i < 3; ++i) m[12 + i] = fadd(m[12 + i], fmul(c[i], ns));
    return vu_product(m, Mat16{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1});  // sp+e0, 002bf9b4..
}

std::array<std::array<float, 4>, 4> eqmodify_figure_clip_2bfa90() {
    const double near_f4 = 1.0, far_fc = 40.0, half_fov_100 = 0.5236;
    const double px = 1.0 / std::tan(half_fov_100), py = px * 4.0 / 3.0;
    const double pz = (far_fc + near_f4) / (far_fc - near_f4), pw = -2.0 * far_fc * near_f4 / (far_fc - near_f4);
    // view: (-x, -(y - 5), z - 1.5); A: camera (x, y, z) -> (x, z, y); clip = (px ax, py ay, pz az + pw, az).
    const double cam_y = 5.0, cam_z = 1.5;
    std::array<std::array<float, 4>, 4> col{};
    col[0] = {static_cast<float>(-px), 0.0F, 0.0F, 0.0F};
    col[1] = {0.0F, 0.0F, static_cast<float>(-pz), -1.0F};
    col[2] = {0.0F, static_cast<float>(py), 0.0F, 0.0F};
    col[3] = {0.0F, static_cast<float>(-py * cam_z), static_cast<float>(pz * cam_y + pw), static_cast<float>(cam_y)};
    return col;
}

}  // namespace omega
