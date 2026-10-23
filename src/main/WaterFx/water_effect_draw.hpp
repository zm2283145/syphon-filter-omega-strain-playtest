#pragma once
#include "water_effects.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace omega {
// 004bc1b0: the ALPHA_1 words; a ripple picks its variant's entry, the splash entry 0.
inline constexpr std::array<std::uint64_t, 4> water_alpha_words{0x0000008000000044ULL, 0x0000008000000068ULL,
                                                                0x0000008000000048ULL, 0x0000008000000042ULL};
struct WaterDrawVertex {
    RootVector position{};               // 001325e0: the world corner (w as computed; VU1 00371200 ignores it)
    std::array<std::int32_t, 4> rgba{};  // 0025c9d0: cvt.w.s(128 * colour)
    std::array<float, 4> st{};           // 00132000(u + 2048, v + 2048, 1, 0)
};
// One 00371200 flush: one 4-vertex strip (triangles 0 1 2 and 1 2 3).
struct WaterDrawFlush {
    std::uint32_t texture{};  // the handle bound by 003820e0(*00539248, handle)
    std::int32_t frame{};     // the texture entry's +4c at the flush
    std::uint64_t alpha{};    // 005392d8
    std::array<WaterDrawVertex, 4> strip{};
};
// The texture manager's per-entry frame word +4c (keyed by handle). It
// persists across draws: a ripple only moves it to its own frame when that
// frame is below the texture's frame count (unsigned compare).
struct WaterTextureFrames {
    std::map<std::uint32_t, std::int32_t> frame;
    std::int32_t& at(std::uint32_t handle) { return frame[handle]; }  // absent: 0
};
// 002b4050(record + 40): the splash quad, nothing when the timer is 0 or the alpha 0.14 * timer <= 2^-7.
void draw_water_splash(const WaterEffectRecord&, WaterTextureFrames&, std::vector<WaterDrawFlush>&);
// 002b3130(record + 70): one quad per ripple, newest first.
void draw_water_ripples(const WaterEffectRecord&, WaterTextureFrames&, std::vector<WaterDrawFlush>&);
// 002b1cb0(mgr): for each of the 20 slots the splash, then the ripples.
std::vector<WaterDrawFlush> draw_water_effects(const std::array<const WaterEffectRecord*, WaterEffectManager::slot_count>&,
                                               WaterTextureFrames&);
std::vector<WaterDrawFlush> draw_water_effects(const WaterEffectManager&, WaterTextureFrames&);
}  // namespace omega
