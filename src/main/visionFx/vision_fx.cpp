#include "vision_fx.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Vision effects: " + what); }
// EE FPU: binary32, round to nearest, denormal results flushed; div.s by zero gives the largest float (the sign of
// a * b, + for 0 / 0, as tools/ee/ee.py).
float ftz(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float div(float a, float b) {
    if(b == 0.0F) {
        const bool negative = a != 0.0F && std::signbit(a) != std::signbit(b);
        return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return ftz(a / b);
}
std::int32_t cvt_w_s(float v) {                    // cvt.w.s: truncation, saturating
    if(v >= 2147483647.0F) return std::numeric_limits<std::int32_t>::max();
    if(v <= -2147483648.0F) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(v);
}

// 002604c0's timer blocks (the fade at 002604dc..0026057c, the flair at 00260580..00260624).
void step_timer(VisionFxTimer& t, float dt) {
    if(add(t.out, add(t.in, t.hold)) == 0.0F || t.done) return;
    t.elapsed = add(t.elapsed, dt);
    if(!t.started && !(t.elapsed <= t.delay)) {
        t.elapsed = sub(t.elapsed, t.delay);
        t.started = true;
    }
    if(t.started && !(t.elapsed < add(t.out, add(t.in, t.hold)))) t.done = true;
}
// The level of a timer (002604c0's +2c at 002606e0..002607a8; 0025fbb0's +28 at 0025fc90..0025fd68):
// before the start 1 when out > 0, after the end 1 when out <= 0, else min(1, e / in) - max(0, (e - in - hold) / out).
float timer_level(const VisionFxTimer& t) {
    if(!t.started) return t.out <= 0.0F ? 0.0F : 1.0F;
    if(t.done) return t.out <= 0.0F ? 1.0F : 0.0F;
    float level = div(t.elapsed, t.in);
    if(1.0F < level) level = 1.0F;
    float off = div(sub(sub(t.elapsed, t.in), t.hold), t.out);
    if(!(0.0F <= off)) off = 0.0F;
    return sub(level, off);
}
void reset_fade(VisionFxTimer& t) { t = {}; }                       // 00260970
void set_fade(VisionFxTimer& t, float delay, float in, float hold, float out) {   // 00260950
    t.delay = delay;
    t.in = in;
    t.hold = hold;
    t.out = out;
}

std::string hex(std::uint32_t v) {
    static const char* digits = "0123456789abcdef";
    std::string out(8, '0');
    for(int i = 7; i >= 0; --i, v >>= 4) out[static_cast<std::size_t>(i)] = digits[v & 0xf];
    return out;
}
constexpr std::uint32_t fbmsk_normal = 0xff000000U;
constexpr std::uint32_t fbmsk_night = 0xffff00ffU;     // 0025fe40: green only
constexpr std::uint32_t fbmsk_thermal = 0xff00ffffU;   // 00260070: blue only
// 1 / ((r*r + g*g) + b*b): vmul.xyz, vmulax / vmadday / vmaddz by (1, 1, 1), then div.s 1, s.
float inverse_sum_squares(const ShadowVector& c) {
    return div(1.0F, add(add(mul(c[0], c[0]), mul(c[1], c[1])), mul(c[2], c[2])));
}
ShadowVector vec(const std::array<float, 4>& v) { return {v[0], v[1], v[2], v[3]}; }
// Light vcall +28 = 00144b10: the position (and 00535d62 = 1) is written only when the VU difference has a non-zero
// lane (MAC zero flags not all set; flushed differences count as zero).
bool set_light_position(LevelLight& l, const ShadowVector& p) {
    bool changed = false;
    for(std::size_t i = 0; i < 4; ++i)
        if(sub(p[i], l.position[i]) != 0.0F) changed = true;
    if(changed) l.position = p;
    return changed;
}
LevelLight& pool_entry(std::vector<LevelLight>& pool, std::size_t index, const char* what) {
    if(index >= pool.size()) fail(std::string("the ") + what + " light the prepare took is no longer in its pool");
    return pool[index];
}
}

std::array<VisionModeBlock, 5> vision_fx_default_blocks() {
    std::array<VisionModeBlock, 5> b{};
    for(VisionModeBlock& m : b) {                              // 00260c10..00260c4c
        m.ambient = {0.2F, 0.2F, 0.2F, 0.0F};
        m.point = {0.8F, 0.8F, 0.8F, 0.8F};
        m.scale_30 = std::bit_cast<float>(0x3f7ae148U);
    }
    for(std::size_t i = 1; i <= 2; ++i) b[i].ambient = {2.0F, 2.0F, 2.0F, 0.0F};   // 00260cd4..00260d44
    b[1].outer_24 = 20.0F;
    b[1].tint_28 = std::bit_cast<float>(0x3e99999aU);   // 0.3
    b[2].outer_24 = 25.0F;
    b[2].tint_28 = 0.5F;
    return b;
}

void vision_fx_set_noise(VisionFx& fx, const std::array<std::int32_t, 9>& flare_ids, std::int32_t noise_id,
                         const VisionNoiseTexture& noise) {
    fx.flares_1d4 = flare_ids;                                   // 00260a80..00260aa0
    fx.noise_1f8 = noise_id;                                     // 00260ab4
    fx.noise_19c = fx.noise_1f8;
    fx.frame_1ac = 0;
    fx.noise_w_1a0 = static_cast<float>(noise.width_04);         // 00260ae0: lhu, cvt.s.w (exact)
    fx.noise_h_1a4 = static_cast<float>(noise.height_06);
}

void vision_fx_set_flair(VisionFx& fx, std::uint8_t flair, float delay, float in, float hold, float out) {
    fx.flair_08 = flair;
    fx.flair = {};
    fx.flair.delay = delay;
    fx.flair.in = in;
    fx.flair.hold = hold;
    fx.flair.out = out;
    if(fx.flair_08 == 0) fx.level_28 = 0.0F;
}

void vision_fx_set_mode(VisionFx& fx, std::int32_t mode, bool force) {
    if(fx.mode_00 != mode) {
        fx.pending_04 = mode;
        if(force) fx.mode_00 = mode;
    }
    if(fx.pending_04 == -1 && mode != 0) {
        fx.pending_04 = mode;
        fx.fade = {};
        fx.fade.in = 1.0F;
    }
}

void vision_fx_update(VisionFx& fx, float dt, VisionNoiseTexture* noise, float viewport_h_1fc) {
    step_timer(fx.fade, dt);
    step_timer(fx.flair, dt);
    if(fx.pending_04 != fx.mode_00) {
        const std::int32_t pending = fx.pending_04;
        if(pending != 0 && pending != -1) {
            reset_fade(fx.fade);
            set_fade(fx.fade, 0.0F, 1.5F, 0.0F, 0.0F);
            fx.mode_00 = fx.pending_04;
        } else if(pending != -1) {
            reset_fade(fx.fade);
            set_fade(fx.fade, 0.0F, 0.0F, 0.0F, std::bit_cast<float>(0x3f4ccccdU));   // 0.8
            fx.pending_04 = -1;
        }
        if(fx.fade.done && fx.pending_04 == -1) {                // 00260940
            fx.mode_00 = 0;
            fx.pending_04 = 0;
        }
    }
    fx.fade_2c = timer_level(fx.fade);
    fx.light_1a8 = mul(std::bit_cast<float>(0x3f266666U), fx.fade_2c);   // 0.65
    if(fx.flair.done) {
        fx.flair = {};
        fx.flair_08 = 0;
        fx.level_28 = 0.0F;
    }
    if(fx.mode_00 != 1 && fx.mode_00 != 2) return;              // 002607f8
    if(!noise)
        fail("vision mode " + std::to_string(fx.mode_00) + " without the noise texture object (00260818 reads "
             "*(*(00539248)+8)[+19c] unconditionally)");
    if(noise->frames_22 >= 2) {                                  // 0026083c
        fx.frame_time_1b0 = add(fx.frame_time_1b0, dt);
        if(!(fx.frame_time_1b0 <= fx.frame_period_1b4)) {
            fx.frame_time_1b0 = sub(fx.frame_time_1b0, fx.frame_period_1b4);
            ++fx.frame_1ac;
            if(fx.frame_1ac == noise->frames_22) fx.frame_1ac = 0;
            if(fx.frame_1ac != noise->frame_4c && fx.frame_1ac < noise->frames_22) {   // 002608ac..002608d0
                noise->byte_60 = 0;
                noise->byte_61 = 0;
                noise->frame_4c = fx.frame_1ac;
            }
        }
    }
    const float h = mul(0.5F, viewport_h_1fc);                  // 002608d8..00260924
    const float v = sub(fx.band_198, mul(dt, h));
    fx.band_198 = v;
    if(v < -h) fx.band_198 = h;
}

void vision_fx_prepare(VisionFx& fx, VisionRenderState& render, LevelLights* lights,
                       const VisionPlayer* player) {
    fx.saved_1d0 = render.flags_5392d0;                          // 0025fbd0
    const std::uint32_t flags = fx.saved_1d0 & 0xffffff9fU;
    const std::int32_t mode = fx.mode_00;
    if(mode == 0) {
        render.byte_5382c0 = 1;                                  // 0025fc18
        render.fbmsk_5382e8 = fbmsk_normal;
        fx.level_24 = 0.0F;                                      // 0025fc78
        if(fx.flair_08 == 1) fx.level_28 = timer_level(fx.flair);   // 0025fc7c..0025fd68
        render.night_53b350 = 0;                                 // 0025fd70
        render.thermal_53b358 = 0;
    } else if(mode == 1 || mode == 2) {
        if(!lights)
            fail("0025fbb0 in vision mode " + std::to_string(mode) + " without the light manager 00535d20");
        render.gather_535d63 = 0;                                // 0025fd98
        fx.ambient_190.reset();                                  // 00373be0: entry 0 when the count (+10) is not 0
        if(!lights->ambient.empty()) fx.ambient_190 = 0;
        fx.point_194.reset();                                    // 00373e70(mgr, 4): the first point entry with id
        for(std::size_t i = 0; i < lights->point.size(); ++i)   // -1, its id = index + 1 (00374008)
            if(lights->point[i].id == -1) {
                fx.point_194 = i;
                lights->point[i].id = static_cast<std::int32_t>(i + 1);
                break;
            }
        if(fx.point_194) {                                       // 0025fdb8..0025fe30
            LevelLight& l = lights->point[*fx.point_194];
            l.vtable = level_light_vtable_point;                 // 004d9280, 004d9240, then 004d9200
            l.intensity = 0.0F;
            l.enabled = false;
            l.aux = 3;
            l.type = 4;
            l.colour = {0.0F, 0.0F, 0.0F, 0.0F};
            l.position = {0.0F, 0.0F, 0.0F, 1.0F};
            render.lights_535d62 = 1;
            l.weight = 0.0F;
        }
        render.flags_5392d0 = flags;                             // 0025fe44
        render.fbmsk_5382e8 = mode == 1 ? fbmsk_night : fbmsk_thermal;
        render.byte_5382c0 = 1;
        if(fx.flair_08 == 1) fx.level_28 = timer_level(fx.flair);   // 0025fea8..0025ffa0
        if(mode == 1) render.night_53b350 = 1;                   // 0025ffa8
        else render.thermal_53b358 = 1;                          // 002601d8
    }
    if(mode < 0 || mode > 4)
        fail("0025fbb0 in vision mode " + std::to_string(mode) + " (the tail 002601e8 indexes the block +50 + "
             "0x40 * mode outside the five blocks)");
    const VisionModeBlock& blk = fx.blocks[static_cast<std::size_t>(mode)];   // 002601e8
    if(fx.ambient_190) {
        if(!lights) fail("0025fbb0: an ambient light is held without the light manager");
        LevelLight& l = pool_entry(lights->ambient, *fx.ambient_190, "ambient");
        fx.saved_ambient_1c0 = {l.colour[0], l.colour[1], l.colour[2], l.colour[3]};
        l.colour = vec(blk.ambient);
        l.weight = inverse_sum_squares(l.colour);
    }
    if(fx.point_194) {                                           // 002602a0
        if(!lights) fail("0025fbb0: a point light is held without the light manager");
        if(!player) fail("0025fbb0: the point light needs the player's vcalls +f4 / +e0 (*(*004ffc0c+30))");
        LevelLight& l = pool_entry(lights->point, *fx.point_194, "point");
        VisionPlayerMatrix m = player->matrix_f4;
        m[3] = player->position_e0;                               // 002602e0..00260308
        const std::array<float, 4> up{0.0F, 5.0F, 0.0F, 1.0F};   // 002602ec..00260320
        ShadowVector p{};
        for(std::size_t i = 0; i < 4; ++i)                       // vmulax / vmadday / vmaddaz / vmaddw
            p[i] = add(add(add(mul(m[0][i], up[0]), mul(m[1][i], up[1])), mul(m[2][i], up[2])), mul(m[3][i], up[3]));
        if(set_light_position(l, p)) render.lights_535d62 = 1;   // vcall +28 (00144b10)
        l.colour = vec(blk.point);                               // 002603a0
        l.weight = inverse_sum_squares(l.colour);
        l.inner = blk.inner_20;
        l.outer = mul(blk.outer_24, fx.fade_2c);
        if(blk.aux8_2c) l.aux = static_cast<std::uint8_t>(l.aux | 8);
        if(blk.aux10_2d) l.aux = static_cast<std::uint8_t>(l.aux | 0x10);
        if(!l.enabled) {                                         // vcall +2c (00144b80)
            l.enabled = true;
            render.lights_535d62 = 1;
        }
    }
    fx.level_24 = mul(blk.tint_28, fx.fade_2c);                  // 00260498..002604a4
}

void vision_fx_prepare(VisionFx& fx) {
    VisionRenderState render;
    vision_fx_prepare(fx, render, nullptr, nullptr);
}

std::optional<VisionFxQuad> vision_fx_quad(const VisionFx& fx) {
    if(fx.mode_00 < 0 || fx.mode_00 > 4)
        fail("0025f930 in vision mode " + std::to_string(fx.mode_00) + " (the quad reads +80 + 0x40 * mode)");
    const float a = fx.level_24, w = fx.level_28;
    if(add(a, w) <= 0.0F) return std::nullopt;                   // 0025fa08
    const float level = add(a, mul(w, sub(1.0F, a)));
    VisionFxQuad q;
    q.a = cvt_w_s(mul(128.0F, level));
    q.r = q.g = q.b = cvt_w_s(mul(255.0F, fx.blocks[static_cast<std::size_t>(fx.mode_00)].scale_30));
    return q;
}

VisionFxDraw vision_fx_draw(VisionFx& fx, VisionRenderState& render, LevelLights* lights) {
    if(fx.ambient_190) {                                         // 0025f940
        if(!lights) fail("0025f930: an ambient light is held without the light manager");
        LevelLight& l = pool_entry(lights->ambient, *fx.ambient_190, "ambient");
        l.colour = vec(fx.saved_ambient_1c0);
        l.weight = inverse_sum_squares(l.colour);
        fx.ambient_190.reset();
    }
    if(fx.point_194) {                                           // 0025f9c8: vcall +8(1) = 00144a60
        if(!lights) fail("0025f930: a point light is held without the light manager");
        LevelLight& l = pool_entry(lights->point, *fx.point_194, "point");
        l.vtable = level_light_vtable_base;                      // 004d9200, then 004d9240
        l.id = -1;
        if(l.enabled) {                                          // vcall +30 (00144980)
            l.enabled = false;
            render.lights_535d62 = 1;
        }
        render.lights_535d62 = 1;                                // 00144ab0
        l.vtable = level_light_vtable_released;                  // 00144ac8
        if(l.type == 4) l.id = -1;                               // 00373d70 (the table removal: no table kept)
        else if(l.type != 0x10 && l.type != 2 && l.type != 1)
            fail("00373d70 for a light of type " + hex(l.type));
        fx.point_194.reset();
    }
    VisionFxDraw out;
    out.fbmsk = render.fbmsk_5382e8;
    out.quad = vision_fx_quad(fx);                               // 0025f9f8..0025fa84
    render.flags_5392d0 = fx.saved_1d0;                          // 0025fa90..0025fb04
    render.fbmsk_5382e8 = fbmsk_normal;
    render.byte_5382c0 = 1;
    render.word_538314 = 1;
    if(fx.mode_00 == 1 || fx.mode_00 == 2) {                     // 0025fb40 / 0025fb70
        render.gather_535d63 = 1;
        out.layers = true;
        out.noise_texture = fx.noise_19c;
        out.noise_w = fx.noise_w_1a0;
        out.noise_h = fx.noise_h_1a4;
        out.noise_alpha = fx.light_1a8;
        out.band_y0 = fx.band_198;
        out.band_y1 = add(4.0F, fx.band_198);                    // 0025eaf4..0025eb08
        out.band_alpha = std::bit_cast<float>(0x3e0f5c29U);      // 0.14
    }
    return out;
}

}