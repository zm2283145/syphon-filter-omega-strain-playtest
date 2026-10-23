#include "corona_draw.hpp"

#include "game_libm.hpp"
#include "vu_float.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using Vec = std::array<float, 4>;
using Mat = std::array<Vec, 4>;  // column-major: Mat[c][r]

constexpr u32 video = 0x5381f0;     // 00374df0's getters take lui 0x54 / addiu -0x7e10
constexpr u32 dma = 0x537f80;       // 003781b0: lui 0x53 / addiu 0x7f80
constexpr u32 vision = 0x5392d0;    // lw -0x6d30(0x540000): fog mode bits 0x180, TEX1 bits 0xc
constexpr u32 aspect = 0x539184;    // 003754a0: lwc1 -0x6e7c(0x540000)
constexpr u32 fade_slot = 0x4bc1b0; // ld -0x3e50(0x4c0000)
constexpr u32 vu0_data = 0x11004000;

std::string hex(u32 value) {
    char text[12];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

u32 r32(const LevelLoadMemory& m, u32 a) { return m.read(a, 4); }
u32 r8(const LevelLoadMemory& m, u32 a) { return m.read(a, 1); }
u32 r16(const LevelLoadMemory& m, u32 a) { return m.read(a, 2); }
void w32(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 4, v); }
void w8(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 1, v); }
u64 r64(const LevelLoadMemory& m, u32 a) { return m.read(a, 4) | static_cast<u64>(m.read(a + 4, 4)) << 32; }
void w64(LevelLoadMemory& m, u32 a, u64 v) {
    m.write(a, 4, static_cast<u32>(v));
    m.write(a + 4, 4, static_cast<u32>(v >> 32));
}
u64 sx(u32 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }
u64 zx(u32 v) { return v; }

float ftz(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float rf(const LevelLoadMemory& m, u32 a) { return std::bit_cast<float>(r32(m, a)); }
void wf(LevelLoadMemory& m, u32 a, float v) { w32(m, a, std::bit_cast<u32>(v)); }
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float neg(float a) { return -a; }
// div.s as the interpreter evaluates it: a zero divisor gives +-FLT_MAX (sign
// of S*T, + when S is 0); results flush to zero.
float dvd(float a, float b) {
    if (b == 0)
        return std::copysign(std::numeric_limits<float>::max(),
                             a != 0 && std::signbit(a) != std::signbit(b) ? -1.0F : 1.0F);
    return ftz(a / b);
}
float madd(float acc, float a, float b) { return ftz(acc + a * b); }
float cvt_s_w(u32 v) { return static_cast<float>(static_cast<s32>(v)); }
// cvt.w.s (and 00190e50): truncation, saturating.
u32 cvt_w_s(float f, const char* where) {
    const double d = f;
    if (std::isnan(d)) throw std::runtime_error(std::string("corona draw ") + where + ": cvt.w.s of NaN");
    if (d >= 2147483647.0) return 0x7fffffffU;
    if (d <= -2147483648.0) return 0x80000000U;
    return static_cast<u32>(static_cast<s32>(d));
}
s32 si(u32 v) { return static_cast<s32>(v); }

Vec load_vec(const LevelLoadMemory& m, u32 a) { return {rf(m, a), rf(m, a + 4), rf(m, a + 8), rf(m, a + 12)}; }
void store_vec(LevelLoadMemory& m, u32 a, const Vec& v) {
    for (int k = 0; k < 4; ++k) wf(m, a + 4 * k, v[k]);
}
Mat load_mat(const LevelLoadMemory& m, u32 a) {
    return {load_vec(m, a), load_vec(m, a + 16), load_vec(m, a + 32), load_vec(m, a + 48)};
}
void store_mat(LevelLoadMemory& m, u32 a, const Mat& x) {
    for (int c = 0; c < 4; ++c) store_vec(m, a + 16 * c, x[c]);
}
// vmulax / vmadday / vmaddaz / vmaddw: ((c0*x + c1*y) + c2*z) + c3*w per lane.
Vec transform(const Mat& a, const Vec& v) {
    Vec r{};
    for (int k = 0; k < 4; ++k) r[k] = madd(madd(madd(mul(a[0][k], v[0]), a[1][k], v[1]), a[2][k], v[2]), a[3][k], v[3]);
    return r;
}
// 0023e0a0 (and the inline copies in 0037d9a0): column k of a*b is a * b.col(k).
Mat multiply(const Mat& a, const Mat& b) { return {transform(a, b[0]), transform(a, b[1]), transform(a, b[2]), transform(a, b[3])}; }

// 00379b00: the current camera record.
u32 current_record(const LevelLoadMemory& m, u32 renderer) { return renderer + 0x1f0 + 0x110 * r32(m, renderer + 0xa70); }
// 00379ac0 (after 00374df0): +10 for an orthographic record (+104), else the perspective +50.
u32 projection(const LevelLoadMemory& m, u32 record) { return record + (r8(m, record + 0x104) ? 0x10 : 0x50); }

// The 00375200..00375360 getters on the video object. +1064 == -1 selects the
// field; otherwise the additive getters answer 0 and the scales 1.
float viewport_getter(const LevelLoadMemory& m, u32 field, float otherwise) {
    return r32(m, video + 0x1064) == 0xffffffffU ? rf(m, video + field) : otherwise;
}
// 003751c0 / 003751e0: the per-mode scale tables indexed by +88.
float scale_1c0(const LevelLoadMemory& m, u32 x) { return rf(m, x + 0x78 + 4 * r32(m, x + 0x88)); }
float scale_1e0(const LevelLoadMemory& m, u32 x) { return rf(m, x + 0x68 + 4 * r32(m, x + 0x88)); }

// 00379e20: the clip-space point goes to VU0 data memory (VSQI through VI00),
// VCLIPw.xyz against |w| gives the low six bits, and 0x40 is added unless the
// point is nearer than +1b8.
u32 clip_code_00379e20(LevelLoadMemory& m, u32 renderer, const Mat& c, const Vec& pos) {
    const Vec v = transform(c, pos);
    store_vec(m, vu0_data, v);
    const float lim = std::fabs(v[3]);
    u32 code = 0;
    for (int k = 0; k < 3; ++k) {
        if (v[k] > lim) code |= 1U << (2 * k);
        if (v[k] < -lim) code |= 2U << (2 * k);
    }
    const float z = rf(m, vu0_data + 8);
    if (!(z < rf(m, renderer + 0x1b8))) code |= 0x40;
    return code;
}

// 00375c30: reserve a tagged packet of qwc qwords on channel ch.
u32 packet_reserve_00375c30(LevelLoadMemory& m, u32 ch, u32 id, u32 qwc, u32 addr, bool second_dword) {
    const u32 desc = r32(m, dma + 4 * (ch & 0xff));
    u32 cursor = r32(m, desc + 0x10);
    if (!(cursor + 16 * qwc < r32(m, desc + 8))) {
        w32(m, dma + 0xd8, ch);
        cursor = r32(m, r32(m, dma + 0xcc + 4 * ch) + 0xc);
        w32(m, desc + 0x10, cursor);
    }
    w64(m, cursor, (zx(addr) << 32) | sx(id << 28) | zx(qwc));
    w64(m, cursor + 8, 0);
    const u32 ret = cursor + 16;
    w32(m, desc + 0x10, ret + 16 * qwc);
    return second_dword ? ret - 8 : ret;
}
// 00375ba0: open a packet whose length 00375ac0 patches in later.
u32 packet_open_00375ba0(LevelLoadMemory& m, u32 ch, u32 id, u32 addr, bool second_dword) {
    const u32 desc = r32(m, dma + 4 * (ch & 0xff));
    u32 cursor = r32(m, desc + 0x10);
    if (!(cursor < r32(m, desc + 8))) {
        w32(m, dma + 0xd8, ch);
        cursor = r32(m, r32(m, dma + 0xcc + 4 * ch) + 0xc);
        w32(m, desc + 0x10, cursor);
    }
    w32(m, desc + 0x18, addr);
    w8(m, desc + 0x1c, id);
    w32(m, desc + 0x14, cursor);
    w64(m, cursor, 0);
    w64(m, cursor + 8, 0);
    cursor += 16;
    w32(m, desc + 0x10, cursor);
    return second_dword ? cursor - 8 : cursor;
}
// 00375ac0: close the packet at `end`.
void packet_close_00375ac0(LevelLoadMemory& m, u32 ch, u32 end) {
    const u32 desc = r32(m, dma + 4 * (ch & 0xff));
    const u32 cursor = r32(m, desc + 0x10);
    if (end == cursor) {
        w32(m, desc + 0x10, r32(m, desc + 0x14));
        return;
    }
    const u32 id = r8(m, desc + 0x1c);
    if (id > 0) {
        const s32 diff = si(end - cursor);
        const s32 q = (diff / 8) >> 1;
        w64(m, r32(m, desc + 0x14), (zx(r32(m, desc + 0x18)) << 32) | sx(id << 28) | sx(static_cast<u32>(q)));
    }
    w32(m, desc + 0x10, end);
}

// 003797e0: TEST from +d8..+f4.
u64 test_word_003797e0(const LevelLoadMemory& m, u32 r) {
    return zx(r32(m, r + 0xd8)) | zx(r32(m, r + 0xdc)) << 1 | zx(r32(m, r + 0xe0)) << 4 | zx(r32(m, r + 0xe4)) << 12 |
           zx(r32(m, r + 0xe8)) << 14 | zx(r32(m, r + 0xec)) << 15 | zx(r32(m, r + 0xf0)) << 16 |
           zx(r32(m, r + 0xf4)) << 17;
}
// 00379840: PRMODE from +100..+11c.
u64 prmode_word_00379840(const LevelLoadMemory& m, u32 r) {
    return zx(r32(m, r + 0x100)) << 3 | zx(r32(m, r + 0x104)) << 4 | zx(r32(m, r + 0x108)) << 5 |
           zx(r32(m, r + 0x10c)) << 6 | zx(r32(m, r + 0x110)) << 7 | zx(r32(m, r + 0x114)) << 8 |
           zx(r32(m, r + 0x118)) << 9 | zx(r32(m, r + 0x11c)) << 10;
}
// 00379740: the texture record for id.
u32 texture_record(const LevelLoadMemory& m, u32 manager, u32 id) { return r32(m, r32(m, manager + 8) + 4 * id); }
// 003bb620: patch the TEX1 word at +58 from the vision mode, return its address.
u32 tex1_003bb620(LevelLoadMemory& m, u32 tex) {
    const u32 mode = r32(m, vision);
    u64 t0 = 0, a3 = 0;
    if (mode & 0xc) {
        t0 = 1;
        if (mode & 4) a3 = 4;
        else if (mode & 8) a3 = 5;
    }
    const u64 old = r64(m, tex + 0x58);
    w64(m, tex + 0x58, (old & ~u64{0x1e0}) | t0 << 5 | a3 << 6);
    return tex + 0x58;
}

class PacketWriter {
public:
    PacketWriter(LevelLoadMemory& m, u32 at) : m_(m), at_(at) {}
    void dword(u64 v) { w64(m_, at_, v); at_ += 8; }
    void pair(u64 data, u64 reg) { dword(data); dword(reg); }
    u32 at() const { return at_; }

private:
    LevelLoadMemory& m_;
    u32 at_;
};

u64 xy(s32 a, s32 b) { return sx(static_cast<u32>(a) << 4) | sx(static_cast<u32>(b) << 4) << 16; }
}  // namespace

void camera_perspective_003754a0(LevelLoadMemory& m, u32 rec) {
    const float f20 = dvd(1.0F, rf(m, aspect));
    const float t = ftz(game_tanf(rf(m, rec + 0x100)));
    const float f2 = mul(t, f20);
    wf(m, rec + 0x50, dvd(1.0F, t));
    for (u32 off : {0x60U, 0x70U, 0x80U, 0x54U}) w32(m, rec + off, 0);
    wf(m, rec + 0x64, dvd(1.0F, f2));
    for (u32 off : {0x74U, 0x84U, 0x58U, 0x68U}) w32(m, rec + off, 0);
    {
        const float near = rf(m, rec + 0xf4), far = rf(m, rec + 0xfc);
        wf(m, rec + 0x78, dvd(add(far, near), sub(far, near)));
    }
    {
        const float far = rf(m, rec + 0xfc), near = rf(m, rec + 0xf4);
        const float num = neg(mul(mul(2.0F, far), near));
        wf(m, rec + 0x88, dvd(num, sub(far, near)));
    }
    w32(m, rec + 0x5c, 0);
    w32(m, rec + 0x6c, 0);
    w32(m, rec + 0x7c, 0x3f800000);
    for (u32 off : {0x8cU, 0xd0U, 0xd4U}) w32(m, rec + off, 0);
    wf(m, rec + 0xd8, neg(rf(m, rec + 0xf4)));
    wf(m, rec + 0xdc, dvd(1.0F, rf(m, rec + 0xf4)));
    w32(m, rec + 0xe0, 0);
    w32(m, rec + 0xe4, 0);
    wf(m, rec + 0xe8, rf(m, rec + 0xfc));
    wf(m, rec + 0xec, dvd(1.0F, rf(m, rec + 0xfc)));
}

// 00375390: the orthographic record's projection (+10..+4c) and its clip
// constants (+b0..+cc). The scales are the 00375330/00375360 getters (+5c,
// +60 when +1064 is -1, else 1).
void camera_orthographic_00375390(LevelLoadMemory& m, u32 rec) {
    const float f3 = mul(2.0F, viewport_getter(m, 0x5c, 1.0F));
    const float f1 = mul(2.0F, viewport_getter(m, 0x60, 1.0F));
    wf(m, rec + 0x10, dvd(f3, rf(m, rec + 8)));
    for (u32 o : {0x20U, 0x30U, 0x40U, 0x14U}) w32(m, rec + o, 0);
    wf(m, rec + 0x24, dvd(f1, rf(m, rec + 0xc)));
    for (u32 o : {0x34U, 0x44U, 0x18U, 0x28U}) w32(m, rec + o, 0);
    wf(m, rec + 0x38, dvd(2.0F, sub(rf(m, rec + 0xf8), rf(m, rec + 0xf0))));
    const float n = rf(m, rec + 0xf0), f = rf(m, rec + 0xf8);
    wf(m, rec + 0x48, dvd(neg(add(f, n)), sub(f, n)));
    for (u32 o : {0x1cU, 0x2cU, 0x3cU}) w32(m, rec + o, 0);
    w32(m, rec + 0x4c, 0x3f800000);
    const u32 clip[8] = {0, 0, 0xbf800000U, 0x3f800000U, 0, 0, 0x3f800000U, 0x3f800000U};
    for (u32 k = 0; k < 8; ++k) w32(m, rec + 0xb0 + 4 * k, clip[k]);
}

void camera_record_refresh_00374df0(LevelLoadMemory& m, LevelLoadHost& host, u32 rec) {
    (void)host;
    if (!r8(m, rec + 0x106)) return;
    if (r8(m, rec + 0x104))
        camera_orthographic_00375390(m, rec);
    else
        camera_perspective_003754a0(m, rec);
    const bool vp = r8(m, rec + 0x105) != 0;
    const float g360 = viewport_getter(m, 0x4c, 1.0F), g330 = viewport_getter(m, 0x44, 1.0F);
    const float g300 = viewport_getter(m, 0x50, 1.0F), g2d0 = viewport_getter(m, 0x48, 1.0F);
    const float g2a0 = viewport_getter(m, 0x54, 0.0F), g270 = viewport_getter(m, 0x3c, 0.0F);
    const float g240 = viewport_getter(m, 0x58, 0.0F), g210 = viewport_getter(m, 0x40, 0.0F);
    const float f25 = vp ? mul(g360, g330) : 1.0F;
    const float f24 = vp ? mul(g300, g2d0) : 1.0F;
    const float f23 = vp ? add(g2a0, g270) : 0.0F;
    const float f22 = vp ? add(g240, g210) : 0.0F;
    const float f21 = vp ? rf(m, video + 0xf88) : rf(m, rec + 8);
    const float f20 = vp ? rf(m, video + 0xf8c) : rf(m, rec + 0xc);
    const float f28 = scale_1e0(m, video), f27 = scale_1c0(m, video);
    const float g1b0 = rf(m, video + 0x38), g1a0 = rf(m, video + 0x34);
    const float f26 = mul(0.5F, sub(g1b0, g1a0));
    const float f1 = mul(0.5F, rf(m, rec + 8));
    const float f5 = mul(0.5F, rf(m, rec + 0xc));
    wf(m, rec + 0x90, mul(16.0F, mul(f28, mul(f1, f25))));
    const float f3 = sub(mul(0.5F, f21), f1);
    const float f1b = sub(mul(0.5F, f20), f5);
    wf(m, rec + 0x94, mul(16.0F, mul(f27, mul(neg(f5), f24))));
    wf(m, rec + 0x98, mul(16.0F, neg(f26)));
    w32(m, rec + 0x9c, 0x3f800000);
    wf(m, rec + 0xa0, mul(16.0F, add(2048.0F, sub(sub(f3, rf(m, rec)), f23))));
    wf(m, rec + 0xa4, mul(16.0F, add(2048.0F, sub(sub(f1b, rf(m, rec + 4)), f22))));
    wf(m, rec + 0xa8, mul(16.0F, add(f26, rf(m, video + 0x34))));
    w32(m, rec + 0xac, 0);
    // Scissor: re-read through the getters.
    const float s = viewport_getter(m, 0x4c, 1.0F), t = viewport_getter(m, 0x50, 1.0F);
    const float a = viewport_getter(m, 0x54, 0.0F), b = viewport_getter(m, 0x58, 0.0F);
    const float x0 = add(a, mul(rf(m, rec), s));
    const float y0 = add(b, mul(rf(m, rec + 4), t));
    const float x1 = add(a, add(x0, mul(rf(m, rec + 8), s)));
    const float y1 = add(b, add(y0, mul(rf(m, rec + 0xc), t)));
    const u32 ix0 = cvt_w_s(x0, "00374df0"), ix1 = cvt_w_s(x1, "00374df0") - 1;
    const u32 iy0 = cvt_w_s(y0, "00374df0"), iy1 = cvt_w_s(y1, "00374df0") - 1;
    w64(m, rec + 0x108, sx(ix0) | sx(ix1) << 16 | zx(iy0) << 32 | sx(iy1) << 48);
    w8(m, rec + 0x106, 0);
}

void renderer_view_projection_0037d9a0(LevelLoadMemory& m, LevelLoadHost& host, u32 r) {
    const u32 rec = current_record(m, r);
    if (r8(m, rec + 0x106)) w8(m, r + 0xa78, 1);
    if (!r8(m, r + 0xa78)) return;
    camera_record_refresh_00374df0(m, host, rec);
    Mat p = load_mat(m, projection(m, rec));
    const float f2 = dvd(1.0F, scale_1c0(m, r));
    const float f3 = dvd(1.0F, scale_1e0(m, r));
    const bool ortho = r8(m, current_record(m, r) + 0x104) != 0;
    const Mat view = load_mat(m, r + 0xa80 + 64 * r32(m, r + 0xa74));
    if (!ortho) {
        const Mat e80 = load_mat(m, r + 0xe80);
        store_mat(m, r + 0xf00, multiply(multiply(p, e80), view));
        p[0][0] = mul(p[0][0], f3);
        p[1][1] = mul(p[1][1], f2);
        const Mat view2 = load_mat(m, r + 0xa80 + 64 * r32(m, r + 0xa74));
        store_mat(m, r + 0xec0, multiply(multiply(p, e80), view2));
        store_vec(m, r + 0x1b0, transform(p, {0.0F, 0.0F, rf(m, r + 0x158), 1.0F}));
        store_vec(m, r + 0x1c0, transform(p, {0.0F, 0.0F, rf(m, r + 0x15c), 1.0F}));
    } else {
        store_mat(m, r + 0xf00, multiply(p, view));
        p[0][0] = mul(p[0][0], f3);
        p[1][1] = mul(p[1][1], f2);
        const Mat view2 = load_mat(m, r + 0xa80 + 64 * r32(m, r + 0xa74));
        store_mat(m, r + 0xec0, multiply(p, view2));
    }
    w8(m, r + 0xa78, 0);
}

void renderer_state_packet_003798a0(LevelLoadMemory& m, LevelLoadHost& host, u32 r) {
    const u32 v0 = packet_reserve_00375c30(m, 1, 1, 0x10, 0, true);
    PacketWriter p(m, v0);
    p.dword(0x11000000);
    p.dword(0x020001e90300002eULL);
    p.dword(0x5000000f01000404ULL);
    p.dword(0x100000000000800eULL);
    p.dword(0xe);
    const u32 rec = current_record(m, r);
    camera_record_refresh_00374df0(m, host, rec);
    w64(m, r + 0xb0, r64(m, rec + 0x108));
    p.pair(r64(m, r + 0x90), 0x4c);
    p.pair(r64(m, r + 0x98), 0x4d);
    p.pair(r64(m, r + 0xa0), 0x4e);
    p.pair(r64(m, r + 0xa0), 0x4f);
    p.pair(r64(m, r + 0xa8), 0x18);
    p.pair(r64(m, r + 0xa8), 0x19);
    p.pair(r64(m, r + 0xb0), 0x40);
    p.pair(r64(m, r + 0xb0), 0x41);
    p.pair(r64(m, r + 0xc0), 0x44);
    p.pair(r64(m, r + 0xb8), 0x45);
    p.pair(0, 0x49);
    p.pair(zx(r32(m, r + 0xec)), 0x4a);
    p.pair(0, 0x4b);
    p.pair(0x8000008080ULL, 0x3b);
}

void corona_draw_003781b0(LevelLoadMemory& m, LevelLoadHost& host, const CoronaDrawCall& call) {
    const u32 r = call.renderer;
    const s32 count = si(call.count);
    if (count == 0) return;

    // 00379ac0 copy with the 1.5 aspect squeeze, then (M*E80)*V or M*V.
    const u32 rec0 = current_record(m, r);
    camera_record_refresh_00374df0(m, host, rec0);
    Mat mm = load_mat(m, projection(m, rec0));
    mm[0][0] = dvd(mm[0][0], 1.5F);
    mm[1][1] = dvd(mm[1][1], 1.5F);
    Mat c;
    if (!r8(m, current_record(m, r) + 0x104)) {
        const Mat t = multiply(mm, load_mat(m, r + 0xe80));
        c = multiply(t, load_mat(m, r + 0xa80 + 64 * r32(m, r + 0xa74)));
    } else {
        c = multiply(mm, load_mat(m, r + 0xa80 + 64 * r32(m, r + 0xa74)));
    }
    const Vec pos = load_vec(m, call.position);
    const u32 clip = clip_code_00379e20(m, r, c, pos);
    if (clip & ~0x40U) return;

    float f22 = 1.0F;
    renderer_view_projection_0037d9a0(m, host, r);
    const Vec pp = transform(load_mat(m, r + 0xec0), pos);
    float f21 = dvd(1.0F, pp[3]);
    if ((clip & 0x40) && (r32(m, vision) & 0x180)) {
        float f1 = mul(std::bit_cast<float>(0x3f008081U), add(rf(m, r + 0x180), mul(rf(m, r + 0x190), f21)));
        if (f1 <= 0.0F) return;
        if (!(f1 < 128.0F)) f1 = 128.0F;
        f22 = dvd(f1, 128.0F);
    }
    const u32 rec = current_record(m, r);
    camera_record_refresh_00374df0(m, host, rec);
    const u32 rec2 = current_record(m, r);
    camera_record_refresh_00374df0(m, host, rec2);
    const Vec scale = load_vec(m, rec + 0x90);
    const Vec s{mul(scale[0], f21), mul(scale[1], f21), mul(scale[2], f21), mul(scale[3], f21)};
    const float sxf = add(rf(m, rec2 + 0xa0), mul(pp[0], s[0]));
    const float syf = add(rf(m, rec2 + 0xa4), mul(pp[1], s[1]));
    const float szf = add(rf(m, rec2 + 0xa8), mul(pp[2], s[2]));
    const float f3 = dvd(1.0F, ftz(game_tanf(rf(m, current_record(m, r) + 0x100))));
    const float sp370 = mul(0.015625F, mul(f3, s[0]));
    const float sp374 = mul(0.015625F, mul(f3, s[1]));
    const float half = call.half_size;
    f21 = mul(half, sp370);
    const float f20 = mul(half, neg(sp374));
    const s32 s6 = si(r32(m, r + 0x1c)) >> 1, s7 = si(r32(m, r + 0x20)) >> 1;
    const s32 x = si(cvt_w_s(sxf, "003781b0")) >> 4;
    const s32 y = si(cvt_w_s(syf, "003781b0")) >> 4;
    const s32 z = si(cvt_w_s(szf, "003781b0")) >> 4;
    s32 box = si(cvt_w_s(mul(0.5F, sp370), "003781b0"));
    if (!(box < 0x29)) box = 0x28;
    s32 cx = x, cy = y;
    if (cx < 0x800 - s6 + box) cx = 0x800 - s6 + box;
    if (s6 + 0x800 - box < cx) cx = s6 + 0x800 - box;
    if (cy < 0x800 - s7 + box) cy = 0x800 - s7 + box;
    if (s7 + 0x800 - box < cy) cy = s7 + 0x800 - box;
    const u32 fbw_src = r32(m, r + 0x10), psm = r32(m, r + 0x2c);

    // Texture residency: queue the distinct ids at +1c of the +1058 manager.
    {
        const u32 man = r32(m, r + 0x1058);
        w8(m, man + 0x14, 1);
        w32(m, man + 0x18, 0);
        w8(m, man + 0x16, 0);
        for (s32 i = 0; i < count; ++i) {
            const u32 id = r32(m, call.ids + 4 * static_cast<u32>(i));
            const u32 mgr = r32(m, r + 0x1058);
            const u32 n = r32(m, mgr + 0x18);
            if (n >= 0x200) continue;
            if (n != 0 && static_cast<s32>(static_cast<std::int16_t>(r16(m, mgr + 0x1a + 2 * n))) == si(id)) continue;
            const u32 n2 = r32(m, mgr + 0x18);
            w32(m, mgr + 0x18, n2 + 1);
            m.write(mgr + 0x1c + 2 * n2, 2, id & 0xffff);
        }
        host.call(m, 0x382540, {r32(m, r + 0x1058)});
    }
    if (r8(m, r + 0xd0)) {
        renderer_state_packet_003798a0(m, host, r);
        w8(m, r + 0xd0, 0);
    }

    // Packet 1: the render state.
    {
        const u32 s4 = r32(m, r + 0x1060) != 0xffffffffU ? 11 : 9;
        PacketWriter p(m, packet_reserve_00375c30(m, 1, 1, s4 + 2, 0, true));
        p.dword(0x11000000);
        p.dword(0x020001e90300002eULL);
        p.dword(zx(0x50000000U | (s4 + 1)) << 32 | 0x01000404U);
        p.dword(sx(s4) | 0x1000000000008000ULL);
        p.dword(0xe);
        p.pair(zx(r32(m, r + 0x120)), 0x1a);
        p.pair(prmode_word_00379840(m, r), 0x1b);
        p.pair(test_word_003797e0(m, r), 0x47);
        p.pair(zx(r32(m, r + 0x124)), 0x46);
        u64 fog = 0;
        if (r64(m, r + 0x10e8) == r64(m, fade_slot))
            fog = sx(r32(m, r + 0x160)) | sx(r32(m, r + 0x164)) << 8 | sx(r32(m, r + 0x168)) << 16;
        p.pair(fog, 0x3d);
        {
            const u64 t = zx(r32(m, r + 0x128));
            p.pair(t | t << 2, 8);
        }
        {
            const u64 t = zx(r32(m, r + 0x128));
            p.pair(t | t << 2, 9);
        }
        p.pair(0, 0x3f);
        p.pair(r64(m, r + 0x10e8), 0x42);
        if (r32(m, r + 0x1060) != 0xffffffffU) {
            const u32 man = r32(m, r + 0x1058);
            const u32 tex = texture_record(m, man, r32(m, r + 0x1060));
            p.pair(r64(m, tex + 0x50), 6);
            const u32 man2 = r32(m, r + 0x1058);
            const u32 tex2 = texture_record(m, man2, r32(m, r + 0x1060));
            p.pair(r64(m, tex1_003bb620(m, tex2)), 0x14);
            w32(m, r + 0x1060, 0xffffffffU);
        }
    }

    // Packet 2: the depth-tested occlusion probe and the sprites.
    const u32 s4 = packet_open_00375ba0(m, 1, 1, 0, true);
    w64(m, s4, 0);
    PacketWriter p(m, s4 + 8);
    p.dword(0);
    p.dword(0xe);
    p.pair(1, 0x1a);
    const u64 fbw = zx(fbw_src >> 6);
    const u64 frame_lo = zx(r32(m, r + 0x12c + 4 * r32(m, r + 0x140))) | fbw << 16 | zx(psm) << 24;
    const u64 frame1 = frame_lo | zx(r32(m, r + 0xf8) & 0xffffffU) << 32;
    p.pair(frame1, 0x4c);
    p.pair(0x6a, 0x42);
    p.pair(0x51001, 0x47);
    p.pair(0x46, 0);
    p.pair(0, 1);
    const u64 ones = 0xffffffff00000000ULL;
    p.pair(xy(cx - box, cy - box) | ones, 5);
    p.pair(xy(cx + box, cy + box) | ones, 5);
    p.pair(0xffffffff, 1);
    p.pair(xy(x - box, y - box) | zx(static_cast<u32>(z)) << 32, 5);
    p.pair(xy(x + box, y + box) | zx(static_cast<u32>(z)) << 32, 5);
    p.pair(frame_lo | 0xffffffULL << 32, 0x4d);
    p.pair(zx(r32(m, r + 0x134)), 0x4f);
    p.pair(0x60, 0x15);
    // TEX0_2 over the frame buffer: TBP = FBP << 5, TBW = FBW, PSM, and 0xe_a8000000
    // (TW = TH = 10, TCC, TFX decal).
    const u64 tex0 = fbw << 14 | zx(r32(m, r + 0x12c + 4 * r32(m, r + 0x140)) << 5) | zx(psm) << 20 |
                     0xea8000000ULL;
    p.pair(tex0, 7);
    p.pair(5, 9);
    p.pair(0x31001, 0x48);
    p.pair(0, 0x19);
    p.pair(0x316, 0);
    p.pair(0, 0x3f);
    p.pair(xy((cx - box) - (0x7ff - s6), (cy - box) - (0x7ff - s7)), 3);
    p.pair(0x04000000, 5);
    p.pair(xy((cx + box) - (0x801 - s6), (cy + box) - (0x801 - s7)), 3);
    p.pair(0x06000200, 5);
    static constexpr u64 reduce[4][4] = {{0x04100010, 0x07000000, 0x06100210, 0x08000100},
                                         {0x07100010, 0x09000000, 0x08100110, 0x09800080},
                                         {0x09100010, 0x0a000000, 0x09900090, 0x0a400040},
                                         {0x0a100010, 0x0c000000, 0x0a500050, 0x0c200020}};
    for (const auto& step : reduce) {
        p.pair(0, 0x3f);
        p.pair(step[0], 3);
        p.pair(step[1], 5);
        p.pair(step[2], 3);
        p.pair(step[3], 5);
    }
    p.pair(1, 0x46);
    p.pair(0x8000000048ULL, 0x42);
    p.pair(0x4010, 8);

    // 00132000: the screen-centre direction per pixel of half extent.
    const s32 hw = si(r32(m, r + 0x1c)) >> 1, hh = si(r32(m, r + 0x20)) >> 1;
    const float fx = cvt_s_w(static_cast<u32>(x)), fy = cvt_s_w(static_cast<u32>(y));
    const Vec axis{dvd(sub(2048.0F, fx), cvt_s_w(static_cast<u32>(hw))),
                   dvd(sub(2048.0F, fy), cvt_s_w(static_cast<u32>(hh))), 0.0F, 0.0F};
    for (s32 i = 0; i < count; ++i) {
        const u32 iu = static_cast<u32>(i);
        const s32 ex = si(cvt_w_s(sub(fx, mul(axis[0], rf(m, call.offsets + 4 * iu))), "003781b0"));
        const s32 ey = si(cvt_w_s(sub(fy, mul(axis[1], rf(m, call.offsets + 4 * iu))), "003781b0"));
        const s32 ew = si(cvt_w_s(mul(f21, rf(m, call.scales + 4 * iu)), "003781b0"));
        const s32 eh = si(cvt_w_s(mul(f20, rf(m, call.scales + 4 * iu)), "003781b0"));
        const s32 lx = si(cvt_w_s(mul(cvt_s_w(static_cast<u32>(hw)), scale_1e0(m, r)), "003781b0"));
        const s32 ly = si(cvt_w_s(mul(cvt_s_w(static_cast<u32>(hh)), scale_1c0(m, r)), "003781b0"));
        if (ex < 0x800 - lx + ew || lx + 0x800 - ew < ex || ey < 0x800 - ly + eh || ly + 0x800 - eh < ey) continue;
        const Vec col = load_vec(m, call.colours + 16 * iu);
        const u32 cr = cvt_w_s(mul(128.0F, col[0]), "003781b0");
        const u32 cg = cvt_w_s(mul(128.0F, col[1]), "003781b0");
        const u32 cb = cvt_w_s(mul(128.0F, col[2]), "003781b0");
        const u32 ca = cvt_w_s(mul(128.0F, mul(f22, col[3])), "003781b0");
        if (si(ca) <= 0) continue;
        const u32 id = r32(m, call.ids + 4 * iu);
        const u32 tex = texture_record(m, r32(m, r + 0x1058), id);
        const u32 header = r32(m, tex + 0x44);
        const u32 tw = r16(m, header + 4), th = r16(m, header + 6), tpsm = r16(m, header + 0xa);
        const u64 tex0_el = r64(m, tex + 0x50);
        // 003796d0 re-reads the record: TBP0 of its TEX0.
        const u32 tbp = static_cast<u32>(r64(m, texture_record(m, r32(m, r + 0x1058), id) + 0x50) & 0x3fff);
        p.pair(0, 0x3f);
        p.pair(zx(tbp >> 5) | zx(tw >> 6) << 16 | zx(tpsm) << 24 | 0xffffffULL << 32, 0x4d);
        p.pair(0x316, 0);
        p.pair(0xffffffff, 1);
        p.pair(0x0c100010, 3);
        p.pair(0, 5);
        p.pair(0x0c100010, 3);
        const u64 size = zx(tw << 4) | zx(th << 4) << 16;
        p.pair(size, 5);
        p.pair(0, 0x3f);
        p.pair(tex0_el, 6);
        u32 glare_alpha = 0;
        if (call.glare && i == 0) {
            const float sq = add(add(mul(axis[0], axis[0]), mul(axis[1], axis[1])), mul(axis[2], axis[2]));
            const float len = ftz(add(0.0F, ftz(vu::sqrt(sq))));
            const float f2 = sub(1.0F, len);
            float f0 = mul(f2, f2);
            f0 = mul(f2, f0);
            f0 = mul(f2, f0);
            glare_alpha = cvt_w_s(mul(120.0F, f0), "003781b0");
        }
        const u64 rgb = sx(cr) | sx(cg) << 8 | sx(cb) << 16;
        p.pair(0x156, 0);
        p.pair(rgb | sx(ca) << 24, 1);
        p.pair(0, 3);
        p.pair(xy(ex - ew, ey - eh) | ones, 5);
        p.pair(size, 3);
        p.pair(xy(ex + ew, ey + eh) | ones, 5);
        if (call.glare_sprite && call.glare && i == 0) {
            const s32 gw = si(r32(m, r + 0x1c)) >> 1, gh = si(r32(m, r + 0x20)) >> 1;
            const u64 uv = xy(static_cast<s32>(tw >> 1), static_cast<s32>(th >> 1));
            p.pair(0x156, 0);
            p.pair(sx(glare_alpha) << 24 | rgb, 1);
            p.pair(uv, 3);
            p.pair(xy(0x800 - gw, 0x800 - gh) | ones, 5);
            p.pair(uv, 3);
            p.pair(xy(gw + 0x800, gh + 0x800) | ones, 5);
        }
        w8(m, r + 0xd0, 1);
    }
    p.dword(0x11000000);
    p.dword(0);
    const u32 end = p.at();
    const s32 n = (si(end - s4) / 8) >> 1;
    w64(m, s4, zx(0x50000000U | static_cast<u32>(n)) << 32 | 0x13000000U);
    w64(m, s4 + 8, sx(static_cast<u32>(n - 1)) | 0x1000000000008000ULL);
    packet_close_00375ac0(m, 1, end);
    w8(m, r + 0xd0, 1);
}

}  // namespace omega
