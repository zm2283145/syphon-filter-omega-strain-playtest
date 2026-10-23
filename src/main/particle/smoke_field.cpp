#include "smoke_field.hpp"

#include "game_libm.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace omega::particle {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Smoke field: " + what); }

// EE FPU / VU single precision as the interpreter models it: binary32 round
// to nearest, denormal results flushed to zero, x / 0 = +-FLT_MAX.
float ftz(float v) { return std::fabs(v) < std::numeric_limits<float>::min() ? std::copysign(0.0f, v) : v; }
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float dvd(float a, float b) {
    if (b == 0.0f) {
        const bool negative = std::signbit(a) != std::signbit(b);
        return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return ftz(a / b);
}
// cvt.w.s truncates toward zero.
std::int32_t cvt_w(float v) {
    if (!(std::fabs(v) < 2147483648.0f)) fail("cvt.w.s out of range");
    return static_cast<std::int32_t>(v);
}
float fb(std::uint32_t bits) { return std::bit_cast<float>(bits); }

SmokeVector vsub(const SmokeVector& a, const SmokeVector& b) {
    return {sub(a[0], b[0]), sub(a[1], b[1]), sub(a[2], b[2]), sub(a[3], b[3])};
}
SmokeVector vadd(const SmokeVector& a, const SmokeVector& b) {
    return {add(a[0], b[0]), add(a[1], b[1]), add(a[2], b[2]), add(a[3], b[3])};
}
// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w by vf0.w = 1, vsqrt Q (|x|).
float length(const SmokeVector& v) {
    const float sum = add(add(mul(v[0], v[0]), mul(v[1], v[1])), mul(v[2], v[2]));
    return std::sqrt(std::fabs(sum));
}

SmokeMatrix frame_at(const SmokeVector& position) {
    // The identity at sp+260 (vf0 rows) with row 3 replaced (sp+290).
    return {{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, position}};
}

// 00234ef0..00235094 / 00235400..00235454: full strength within +ac,
// fading to 0 at R = +b0.
float strength_at(const SmokeField& field, float dist) {
    if (!(dist < field.outer)) return 0.0f;
    if (dist <= field.fade) return field.strength;
    const float span = sub(field.outer, field.fade);
    const float t = dvd(sub(dist, field.fade), span);
    return mul(field.strength, sub(1.0f, t));
}
}  // namespace

SmokeField make_smoke_field(std::int8_t type, float f12, float f13, float f14, float f15) {
    SmokeField field;
    field.type = type;                                       // 00235eb4
    field.inner = f12;                                       // +a8
    field.fade = add(f13, f12);                              // 00235eb0 +ac
    field.outer = add(f14, field.inner);                     // 00235ed4 +b0
    field.strength = f15;                                    // +b4
    const float ratio = dvd(f12, mul(2.0f, f14));            // 00235ecc / 00235ed0
    field.step = mul(2.0f, game_asinf(ratio));               // 00121aa0, 00235efc +a4
    field.ring = cvt_w(add(0.5f, dvd(fb(0x3fc90fdbU), field.step)));  // 00235f18..00235f2c +a0
    return field;
}

void update_smoke_field(SmokeField& field, float dt, const SmokeFieldHost& host) {
    // 00234c98..00234d18: the effect entry, the camera object.
    const auto id = host.effect_id(field.type);
    if (!id || *id < 0) return;
    const auto camera = host.camera();
    if (!camera) return;

    // 00234d20..00234e44: the camera point and the heading of its motion.
    const SmokeVector pos = (*camera)[3];
    const SmokeVector delta = vsub(pos, field.previous);
    SmokeVector dir = delta;
    if (delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f) {
        // vaddx.xyz MAC == 0xe: no motion, the camera's row 1 instead.
        const auto again = host.camera();
        if (!again) fail("camera vanished (00234dbc)");
        dir = (*again)[1];
    } else {
        // 00234e00..00234e28: vdiv Q = 1 / |d|, vmulq.xyz.
        const float q = dvd(1.0f, length(delta));
        dir = {mul(delta[0], q), mul(delta[1], q), mul(delta[2], q), delta[3]};
    }
    const float heading = game_atan2f(-dir[0], dir[1]);

    // 00234e4c..002350b8: refresh the strengths; erase beyond R + 10.
    for (auto it = field.nodes.begin(); it != field.nodes.end();) {
        auto node = it++;
        const float dist = length(vsub(pos, node->position));
        if (!(dist <= add(10.0f, field.outer))) {
            // 00138ab0, 003a2ba0(instance, -1) + 0036b130, 0036b130(value).
            const std::uint64_t instance = node->instance;
            field.nodes.erase(node);
            if (instance) host.destroy(instance);
            continue;
        }
        host.set_alpha(node->instance, strength_at(field, dist));
    }

    // 002350c0..00235484: an empty list is seeded with a disc of
    // (5 R) / +a8 instances placed uniformly by area.
    if (field.nodes.empty()) {
        const std::int32_t count = cvt_w(dvd(mul(5.0f, field.outer), field.inner));
        for (std::int32_t n = 0; n < count; ++n) {
            const float u = mul(fb(0x30000000U), static_cast<float>(static_cast<std::int32_t>(host.random())));
            const float r = mul(fb(0x30000000U), static_cast<float>(static_cast<std::int32_t>(host.random())));
            const float theta = mul(fb(0x40c90fdbU), r);
            const float c = game_cosf(theta);
            const float s = game_sinf(theta);
            const float k = mul(field.outer, std::sqrt(std::fabs(u)));  // 00235210 sqrt.s
            const SmokeVector offset{mul(c, k), mul(s, k), mul(0.0f, k), mul(0.0f, k)};
            const auto here = host.camera();
            if (!here) fail("camera vanished (00235254)");
            const SmokeVector at = vadd((*here)[3], offset);
            SmokeNode node;
            node.position = at;
            node.instance = host.create(*id, frame_at(at));
            host.set_alpha(node.instance, strength_at(field, length(vsub(pos, at))));
            field.nodes.push_back(node);
        }
    }

    // 00235488..002358b8: +a0 ring points +a4 apart around the heading, R ahead.
    const float dz = sub(pos[2], field.previous[2]);
    for (std::int32_t i = 0; i < field.ring; ++i) {
        float angle = add(mul(field.step, static_cast<float>(i - (field.ring >> 1))), heading);
        if (!std::isfinite(angle)) fail("nonfinite ring angle (002354e8)");
        for (;;) {
            if (!(angle <= fb(0x40490fdbU))) {
                angle = add(angle, fb(0xc0c90fdbU));
                continue;
            }
            if (angle < fb(0xc0490fdbU)) {
                angle = add(angle, fb(0x40c90fdbU));
                continue;
            }
            break;
        }
        const float flat = sub(1.0f, std::fabs(dz));
        const float x = mul(-game_sinf(angle), flat);
        const float y = mul(game_cosf(angle), flat);
        const SmokeVector reach{mul(x, field.outer), mul(y, field.outer), mul(dz, field.outer), mul(0.0f, field.outer)};
        const SmokeVector at = vadd(pos, reach);
        bool covered = false;
        for (const auto& node : field.nodes) {
            if (length(vsub(node.position, at)) < field.inner) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        SmokeNode node;
        node.position = at;
        node.instance = host.create(*id, frame_at(at));
        host.set_alpha(node.instance, 0.0f);
        field.nodes.push_back(node);
    }

    // 002358c0..00235954: 003a2400 each instance at its node.
    for (const auto& node : field.nodes) host.update(node.instance, frame_at(node.position), dt);
    field.previous = pos;  // 00235958
}

void draw_smoke_field(const SmokeField& field,
                      const std::function<void(std::uint64_t instance, std::uint32_t flags, const SmokeMatrix& matrix)>& draw) {
    for (const auto& node : field.nodes)
        if (node.instance) draw(node.instance, field.flags_38, frame_at(node.position));
}

}  // namespace omega::particle
