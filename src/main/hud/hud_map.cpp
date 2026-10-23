// Checkpoint 132hud: the HUD's 3D map (hud_map.hpp).
#include "hud_map.hpp"
#include "game_libm.hpp"
#include "gui_runtime.hpp"
#include "ie_screen.hpp"
#include "ni_event_object.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// 004acce0: one float per command centre level (the bits as stored).
constexpr std::array<std::uint32_t, 18> kExtent004acce0{
    0x44c44000, 0x43e78000, 0x44028000, 0x44238000, 0x449d96b8, 0x44e8ffae, 0x44628000, 0x44621c39, 0x43cd0000,
    0x44404000, 0x44357b85, 0x445e4000, 0x440e8000, 0x44b25a3d, 0x43fdbcee, 0x43eb8419, 0x4429c852, 0x43c33e14};
constexpr float kQuarterPi = std::bit_cast<float>(0x3f490fdbU);   // 0x3f490fdb (lui 0x3f49 / ori 0xfdb)
constexpr float kHalfPi = std::bit_cast<float>(0x3fc90fdbU);      // 0x0026bf10
constexpr float kPitchMin = std::bit_cast<float>(0x3e860a92U);    // 0x0026bf68: pi / 12
constexpr float kPitchRate = std::bit_cast<float>(0x39d1b717U);   // 0x0026bf00: 0.0004
constexpr float kZoomRate = std::bit_cast<float>(0x38d1b717U);    // 0x0026bfb4: 0.0001
constexpr float kPi = std::bit_cast<float>(0x40490fdbU);          // 0x0026d2c4

// div.s as tools/ee/ee.py runs it (hud_callouts.cpp's form): a zero divisor gives the largest float
// signed as S * T (positive for a zero S); the map's empty-union case divides by |004ff084| = 0.
float ee_div(float s, float t) {
    if (t != 0.0F) return ie_ftz(s / t);
    const double sign = s != 0.0F ? static_cast<double>(s) * static_cast<double>(t) : 1.0;
    return std::copysign(std::numeric_limits<float>::max(), static_cast<float>(sign));
}

float vu_length(const RootVector& v) {
    // vmul.xyz / vmulax.w / vmadday.w / vmaddz.w then vsqrt (0x0026e678.., 0x0026c6f0..).
    const float sum = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return std::sqrt(sum);
}
}  // namespace

std::int32_t hud_map_level_index(std::string_view level) {
    for (std::size_t i = 0; i < gui::cc_level_names.size(); ++i)
        if (level == gui::cc_level_names[i]) return static_cast<std::int32_t>(i);
    return -1;
}

float hud_map_level_extent(std::int32_t index) {
    if (index < 0 || index >= static_cast<std::int32_t>(kExtent004acce0.size()))
        throw std::runtime_error("0026e6e0: 004acce0 has no entry " + std::to_string(index));
    return std::bit_cast<float>(kExtent004acce0[static_cast<std::size_t>(index)]);
}

HudMapGlobals hud_map_construct(std::int32_t level_index, const std::vector<std::optional<HudMapBounds>>& models) {
    // 0x0026e274..: the union starts inverted, so the first model's bounds become it (vmini / vmaxi).
    RootVector lo{10.0F, 10.0F, 10.0F, 1.0F}, hi{-10.0F, -10.0F, -10.0F, 1.0F};
    for (const auto& model : models) {
        if (!model) continue;
        if (!(model->minimum[0] <= model->maximum[0])) continue;  // 0x0026e488
        for (std::size_t i = 0; i < 4; ++i) {
            lo[i] = std::min(lo[i], model->minimum[i]);
            hi[i] = std::max(hi[i], model->maximum[i]);
        }
    }
    HudMapGlobals g;
    // 0x0026e584: the diagonal when min.x <= max.x, else zero; the centre (sp+100) is unused.
    RootVector diagonal{};
    if (lo[0] <= hi[0])
        for (std::size_t i = 0; i < 4; ++i) diagonal[i] = hi[i] - lo[i];
    g.extent_48b288 = vu_length(diagonal);
    // 0x0026e6a4: app +24c (*004ffc04 +188 +c4).
    if (level_index == -1) {
        g.scaled_4ff058 = false;
        g.scale_48b278 = 0.25F;
    } else {
        const float entry = hud_map_level_extent(level_index);
        g.scale_48b278 = entry <= 0.0F ? 0.25F : 128.0F / entry;
        g.scaled_4ff058 = true;
    }
    g.inverse_scale_48b280 = 1.0F / g.scale_48b278;
    // 0x0026e764: 003e9540(1.25) stores 00493980 (the IAMAP tracker's scale; the trackers are not hosted).
    const float sine = game_sinf(kQuarterPi);
    g.distance_4ff084 = -(g.extent_48b288 / sine);
    // 0x0026e7c0: min.s (EE 0x29) with pi / 4.
    g.zoom_min_48b2a0 = std::min(game_atanf(ee_div(150.0F * g.scale_48b278, std::fabs(g.distance_4ff084))), kQuarterPi);
    const float zoom = game_atanf(ee_div(g.extent_48b288 / 2.0F, std::fabs(g.distance_4ff084)));
    // 0x0026e84c / 0x0026e850: max.s with the minimum, then min.s with pi / 4.
    g.zoom_default_48b2a8 = std::min(std::max(zoom, g.zoom_min_48b2a0), kQuarterPi);
    g.far_48b290 = 1.0F + (g.extent_48b288 + std::fabs(g.distance_4ff084));
    g.near_48b298 = std::fabs(g.distance_4ff084) - (1.0F + g.extent_48b288);
    if (g.near_48b298 < 2.0F) g.near_48b298 = 2.0F;  // 0x0026e8cc
    return g;
}

void hud_map_reset(HudMap& map) {
    map.g.pitch_4ff070 = 0.35F;
    map.g.words_4ff074 = {};
    map.g.zoom_48b2b0 = map.g.zoom_default_48b2a8;
    map.target_60 = 0;
    map.shown_64 = 0;
    map.blend_90 = 0.0F;
    map.hold_94 = 0.0F;
}

std::uint32_t hud_map_sticks(HudMap& map, const std::array<float, 2>& left, const std::array<float, 2>& right) {
    auto& g = map.g;
    // 0x0026bee8: the first stick's y turns the pitch.
    if (left[1] > 0.0F) {
        const float p = g.pitch_4ff070 + kPitchRate * left[1];
        g.pitch_4ff070 = p <= kHalfPi ? p : kHalfPi;
    }
    if (left[1] < 0.0F) {
        const float p = g.pitch_4ff070 + kPitchRate * left[1];
        g.pitch_4ff070 = p < kPitchMin ? kPitchMin : p;
    }
    // 0x0026bfa0: the second stick's y the zoom.
    if (right[1] < 0.0F) {
        const float z = g.zoom_48b2b0 + kZoomRate * right[1];
        g.zoom_48b2b0 = z < g.zoom_min_48b2a0 ? g.zoom_min_48b2a0 : z;
    }
    if (right[1] > 0.0F) {
        const float z = g.zoom_48b2b0 + kZoomRate * right[1];
        g.zoom_48b2b0 = z <= kQuarterPi ? z : kQuarterPi;
    }
    // 0x0026c050: GS_3D_MAP_TURN while any axis is off centre.
    if (left[1] == 0.0F && right[1] == 0.0F && left[0] == 0.0F && right[0] == 0.0F) {
        if (!g.sound_latch_4ff060) return 0;
        g.sound_latch_4ff060 = false;
        return 0x11;
    }
    if (g.sound_latch_4ff060) return 0;
    g.sound_latch_4ff060 = true;
    return 0x10;
}

void hud_map_camera(HudMap& map, float dt, const HudMapCamera& camera) {
    const auto& g = map.g;
    const NiMatrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    // 0x0026d088: T row 3 = (-ff080, -ff084, -ff088, 1) (ff080 / ff088 stay 0).
    NiMatrix t = identity;
    t[3] = {-0.0F, -g.distance_4ff084, -0.0F, 1.0F};
    // 0x0026d124..0x0026d1c4: R = I x Rz(camera +64) x Rx(pitch) (00132130, 0023b450).
    NiMatrix r = identity;
    ni_rotate_rows(r, ni_axis_angle(0.0F, 0.0F, 1.0F, camera.heading_64));
    ni_rotate_rows(r, ni_axis_angle(1.0F, 0.0F, 0.0F, g.pitch_4ff070));
    // 0x0026d218: Object_LookupById(+60); none: the player (*004ffc0c +30 +c).
    auto position = camera.position ? camera.position(map.target_60) : std::nullopt;
    if (!position) {
        map.target_60 = camera.player_id;
        position = camera.position ? camera.position(map.target_60) : std::nullopt;
        if (!position) throw std::runtime_error("0026d238: the map's player object has no position");
    }
    map.position_70 = *position;
    if (map.shown_64 == 0) {  // 0x0026d260
        map.shown_64 = map.target_60;
        map.from_80 = map.position_70;
    }
    if (map.target_60 != map.shown_64) {  // 0x0026d288
        map.blend_90 += dt;
        if (map.blend_90 < 0.5F) {
            const float c = game_cosf(kPi * (map.blend_90 / 0.5F));
            const float f = (2.0F - (1.0F + c)) / 2.0F;
            // vaddax / vmaddax / vmsubx: (+80 + +70 f) - +80 f.
            for (std::size_t i = 0; i < 4; ++i)
                map.position_70[i] = (map.from_80[i] + map.position_70[i] * f) - map.from_80[i] * f;
        } else {
            map.shown_64 = map.target_60;
            map.from_80 = map.position_70;
            map.blend_90 = 0.0F;
        }
    }
    // 0x0026d378: B row 3 = (-s x, -s y, -s z, w).
    NiMatrix b = identity;
    const float s = g.scale_48b278;
    b[3] = {-(map.position_70[0] * s), -(map.position_70[1] * s), -(map.position_70[2] * s), map.position_70[3]};
    // 0x0026d3d8 / 0x0026d47c: the VU products (0023e0a0's form).
    map.view_20 = ni_matrix_product(ni_matrix_product(t, r), b);
}

HudMapMatrix hud_map_ie_matrix(const HudMap& map) {
    HudMapMatrix m = map.view_20;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t k = 0; k < 3; ++k) m[i][k] = m[i][k] * map.g.scale_48b278;
    return m;
}

std::array<std::array<float, 4>, 4> hud_map_clip(const HudMap& map) {
    return hud_map_clip_record(map.g.zoom_48b2b0, map.g.near_48b298, map.g.far_48b290, map.view_20);
}

std::array<std::array<float, 4>, 4> hud_map_iamap_clip(const HudMap& map) {
    static constexpr HudMapMatrix identity{{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}};
    return hud_map_clip_record(map.g.zoom_48b2b0, 1.25F, map.g.far_48b290, identity);  // 0x0026cdc0: +f4 = 1.25
}

std::array<float, 40> hud_map_projection_003754a0(float half_fov, float near_plane, float far_plane) {
    // The EE FPU steps of 003754a0 (corona_draw.cpp's camera_perspective_003754a0 on a record): f20 =
    // 1 / D_00539184 (the in-game 4 / 3, 0x3faaaaab), t = Math_Tan(+100), +50 = 1 / t, +64 = 1 / (t f20),
    // +78 = (far + near) / (far - near), +88 = -((2 far) near) / (far - near), +7c = 1, +d8 = -near,
    // +dc = 1 / near, +e8 = far, +ec = 1 / far; the rest of +50..+ef zero. No checks (none in the original).
    const auto mul = [](float a, float b) { return ie_ftz(a * b); };
    const auto add = [](float a, float b) { return ie_ftz(a + b); };
    const auto sub = [](float a, float b) { return ie_ftz(a - b); };
    const float f20 = ee_div(1.0F, std::bit_cast<float>(0x3faaaaabU));
    const float t = ie_ftz(game_tanf(half_fov));
    const float f2 = mul(t, f20);
    std::array<float, 40> w{};
    w[0x00] = ee_div(1.0F, t);                                                      // +50
    w[0x05] = ee_div(1.0F, f2);                                                     // +64
    w[0x0a] = ee_div(add(far_plane, near_plane), sub(far_plane, near_plane));       // +78
    w[0x0b] = 1.0F;                                                                 // +7c
    w[0x0e] = ee_div(-mul(mul(2.0F, far_plane), near_plane), sub(far_plane, near_plane));  // +88
    w[0x22] = -near_plane;                                                          // +d8
    w[0x23] = ee_div(1.0F, near_plane);                                             // +dc
    w[0x26] = far_plane;                                                            // +e8
    w[0x27] = ee_div(1.0F, far_plane);                                              // +ec
    return w;
}

std::array<std::array<float, 4>, 4> hud_map_clip_record(float half_fov, float near_plane, float far_plane,
                                                        const HudMapMatrix& v) {
    using Q = std::array<float, 4>;
    using M = std::array<Q, 4>;
    // vmulax / vmadday / vmaddaz / vmaddw per lane, and 0023e0a0's column products (corona_draw.cpp's form).
    const auto transform = [](const M& a, const Q& x) {
        Q r{};
        for (std::size_t k = 0; k < 4; ++k)
            r[k] = ie_ftz(ie_ftz(ie_ftz(ie_ftz(a[0][k] * x[0]) + a[1][k] * x[1]) + a[2][k] * x[2]) + a[3][k] * x[3]);
        return r;
    };
    const auto multiply = [&](const M& a, const M& b) {
        return M{transform(a, b[0]), transform(a, b[1]), transform(a, b[2]), transform(a, b[3])};
    };
    const auto w = hud_map_projection_003754a0(half_fov, near_plane, far_plane);
    M p{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t r = 0; r < 4; ++r) p[c][r] = w[4 * c + r];
    // 0037d9a0: renderer +f00 = (P x +e80) x view, +e80 the y / z swap (0037ec24..0037ec64).
    const M axis{Q{1.0F, 0.0F, 0.0F, 0.0F}, Q{0.0F, 0.0F, 1.0F, 0.0F}, Q{0.0F, 1.0F, 0.0F, 0.0F},
                 Q{0.0F, 0.0F, 0.0F, 1.0F}};
    M view{};
    for (std::size_t c = 0; c < 4; ++c) view[c] = v[c];
    const M pa = multiply(p, axis);
    return multiply(pa, view);
}
void hud_map_hold_step(HudMap& map, float dt, std::optional<std::int32_t> highlighted_object) {
    if (!(map.hold_94 > 0.0F)) return;
    map.hold_94 -= dt;
    if (map.hold_94 <= 0.0F) {
        if (highlighted_object) {
            map.target_60 = *highlighted_object;
            map.from_80 = map.position_70;
            map.blend_90 = 0.0F;
        }
        map.hold_94 = 0.0F;
    }
}

void hud_map_release(HudMap& map, std::int32_t player_id) {
    map.hold_94 = 0.0F;
    map.shown_64 = map.target_60;
    map.from_80 = map.position_70;
    map.target_60 = player_id;
}

bool hud_map_prev(HudMap& map, const std::vector<std::uint32_t>& records) {
    if (records.empty()) return false;  // 0x0026c1a4
    if (!map.highlight_3c) {            // 0x0026c1c0: the head
        map.highlight_3c = records.front();
        return true;
    }
    if (records.size() < 2) return false;  // 0x0026c1fc
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i] == *map.highlight_3c) {
            map.highlight_3c = i == 0 ? records.back() : records[i - 1];  // 0x0026c274: the head's prev is the tail
            return true;
        }
    return true;
}

bool hud_map_next(HudMap& map, const std::vector<std::uint32_t>& records) {
    if (records.empty()) return false;
    if (!map.highlight_3c) {
        map.highlight_3c = records.front();
        return true;
    }
    if (records.size() < 2) return false;
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i] == *map.highlight_3c) {
            map.highlight_3c = i + 1 == records.size() ? records.front() : records[i + 1];  // the end wraps to the head
            return true;
        }
    return true;
}

void hud_map_fit(HudMap& map, const RootVector& target, const RootVector& player, const std::array<float, 4>& viewport) {
    // 0x0026c6c4: player - target (vsub.xyzw), its xyz length.
    const RootVector d{player[0] - target[0], player[1] - target[1], player[2] - target[2], player[3] - target[3]};
    const float f3 = map.g.scale_48b278 * (20.0F + vu_length(d));
    const float aspect = viewport[2] / viewport[3];
    const float angle = game_atanf(ee_div(f3 * aspect, std::fabs(map.g.distance_4ff084)));
    // 0x0026c7b4 / 0x0026c7b8: max.s with the zoom, min.s with pi / 4.
    map.g.zoom_48b2b0 = std::min(std::max(angle, map.g.zoom_48b2b0), kQuarterPi);
}

}  // namespace omega
