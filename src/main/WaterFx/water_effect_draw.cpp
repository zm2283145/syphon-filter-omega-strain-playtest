#include "water_effect_draw.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// EE FPU / VU results flush denormals to zero (sign kept).
float fl(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
// cvt.w.s: truncation, saturating at the int32 range.
std::int32_t cvt_w(float v) {
    const double d = v;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return static_cast<std::int32_t>(d);
}
constexpr float min_alpha = 0.0078125F;                            // 0x3c000000
constexpr float splash_rate = std::bit_cast<float>(0x3e0f5c29U);    // 0.14
constexpr float splash_time = 5.0F;                                // 0x40a00000
constexpr float ripple_rate = std::bit_cast<float>(0x3e2aaaaaU);    // 0.16666666
constexpr float variant3_scale = std::bit_cast<float>(0x3f333333U); // 0.7
constexpr float ripple_life = std::bit_cast<float>(0x3f99999aU);    // 1.2
// 0025f220 returns 2048 for both st offsets.
constexpr float st_bias = 2048.0F;
// The strip's uv per vertex (002b4050 / 002b3130 alike).
constexpr std::array<std::array<float, 2>, 4> strip_uv{{{0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}}};

RootVector vadd(const RootVector& a, const RootVector& b) {
    return {fl(a[0] + b[0]), fl(a[1] + b[1]), fl(a[2] + b[2]), fl(a[3] + b[3])};
}
WaterDrawVertex vertex(const RootVector& position, float alpha, std::size_t k) {
    WaterDrawVertex v;
    v.position = position;
    const std::int32_t white = cvt_w(fl(128.0F * 1.0F));
    v.rgba = {white, white, white, cvt_w(fl(128.0F * alpha))};
    v.st = {fl(strip_uv[k][0] + st_bias), fl(strip_uv[k][1] + st_bias), 1.0F, 0.0F};
    return v;
}
}  // namespace

void draw_water_splash(const WaterEffectRecord& r, WaterTextureFrames& frames, std::vector<WaterDrawFlush>& out) {
    if (r.splash_timer == 0.0F) return;
    const float alpha = fl(splash_rate * r.splash_timer);
    if (alpha <= min_alpha) return;
    WaterDrawFlush f;
    f.texture = r.splash_resource;
    f.alpha = water_alpha_words[0];  // 005392d8 = 004bc1b0[0]
    const float half = fl(r.splash_inner + fl(fl(fl(splash_time - r.splash_timer) / splash_time) * r.splash_outer));
    const float n = -half;
    const std::array<RootVector, 4> corners{{{n, n, 0.0F, 1.0F}, {n, half, 0.0F, 1.0F}, {half, n, 0.0F, 1.0F},
                                             {half, half, 0.0F, 1.0F}}};
    for (std::size_t k = 0; k < 4; ++k) f.strip[k] = vertex(vadd(corners[k], r.splash_position), alpha, k);
    f.frame = frames.at(f.texture);
    out.push_back(f);
}

void draw_water_ripples(const WaterEffectRecord& r, WaterTextureFrames& frames, std::vector<WaterDrawFlush>& out) {
    for (const WaterRippleNode& node : r.ripples) {
        float outer_alpha = fl(ripple_rate * node.lifetime);
        if (node.variant == 3) outer_alpha = fl(outer_alpha * variant3_scale);
        if (outer_alpha <= min_alpha) continue;
        const float inner_alpha = fl(node.sinking * outer_alpha);
        // The texture entry's frame moves to the node's when it differs and is
        // below the frame count (lhu header +22, sltu); +60 / +61 (residency)
        // are cleared with it.
        std::int32_t& current = frames.at(r.ripple_resource);
        if (node.frame != current &&
            static_cast<std::uint32_t>(node.frame) < static_cast<std::uint32_t>(r.ripple_frame_count))
            current = node.frame;
        if (node.variant >= water_alpha_words.size())
            throw std::runtime_error("002b3264: ripple variant " + std::to_string(node.variant) +
                                     " indexes past the 004bc1b0 alpha table");
        WaterDrawFlush f;
        f.texture = r.ripple_resource;
        f.alpha = water_alpha_words[node.variant];
        const float scale =
            fl(r.ripple_inner + fl(r.ripple_outer * fl(1.0F - fl(node.lifetime / ripple_life))));
        for (std::size_t k = 0; k < 4; ++k) {
            const RootVector& c = node.corners[k];
            const RootVector scaled{fl(c[0] * scale), fl(c[1] * scale), fl(c[2] * scale), fl(c[3] * scale)};
            f.strip[k] = vertex(vadd(scaled, node.position), (k % 2 == 0) ? inner_alpha : outer_alpha, k);
        }
        f.frame = current;
        out.push_back(f);
    }
    // 002b38f0: 005392d8 is restored to 004bc1b0[0] after the list.
}

std::vector<WaterDrawFlush> draw_water_effects(
    const std::array<const WaterEffectRecord*, WaterEffectManager::slot_count>& slots, WaterTextureFrames& frames) {
    std::vector<WaterDrawFlush> out;
    for (const WaterEffectRecord* r : slots) {
        if (!r) continue;
        draw_water_splash(*r, frames, out);
        draw_water_ripples(*r, frames, out);
    }
    return out;
}

std::vector<WaterDrawFlush> draw_water_effects(const WaterEffectManager& m, WaterTextureFrames& frames) {
    std::array<const WaterEffectRecord*, WaterEffectManager::slot_count> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i) slots[i] = m.slot(i);
    return draw_water_effects(slots, frames);
}
}  // namespace omega
