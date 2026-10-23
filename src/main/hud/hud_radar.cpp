#include "hud_radar.hpp"
#include "game_libm.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("HUD radar ") + address + ": " + what);
}

float mul(float a, float b) { return ie_ftz(a * b); }
float add(float a, float b) { return ie_ftz(a + b); }
float sub(float a, float b) { return ie_ftz(a - b); }
// vmadd / vmadda lane: the product rounded to binary32, the sum flushed.
float madd(float acc, float a, float b) {
    const float p = a * b;
    return ie_ftz(acc + p);
}

} // namespace

HudRadar hud_radar_construct(IeScreen& screen, int group) {
    if (group < 0) fail("002619e8", "no \"radargroup\" node (00260d50 on a null node)");
    HudRadar r;
    r.danger_8 = screen.find_element(group, "dangermeter");
    r.environmental_c = screen.find_element(group, "environmentalmeter");
    r.marker_10 = screen.find_element(group, "playermarker");
    r.north_14 = screen.find_element(group, "northindicator");
    screen.set_visible(group, 1, false);                       // 003eebe0(group, 1, 0)
    const int radar = screen.find_child(group, "radar");
    if (radar < 0) fail("00261a58", "no \"radar\" child (003eebe0 on a null node)");
    screen.set_visible(radar, 1, true);
    r.waypoint_18 = screen.find_element(group, "selected_waypoint");
    if (r.waypoint_18 < 0) fail("00261a80", "no \"selected_waypoint\" child (003f45a0 on a null element)");
    screen.evaluate_element(r.waypoint_18, static_cast<float>(hud_radar_waypoint_last));  // cvt.s.w *0048b0d8
    r.triangle_4ffd58 = screen.find_child(group, "radartriangle");
    r.triangle_4ffd50 = screen.find_child(group, "radartriangle");
    r.square_4ffd60 = screen.find_child(group, "radarsquare");
    return r;
}

void hud_radar_update(IeScreen& screen, const HudRadar& r, const HudRadarFrame& f) {
    if (r.north_14 < 0) fail("002616f8", "003e8070 on a null north indicator element");
    // 002616e0: (0, -heading, 0) at sp + f0, kept for the waypoint.
    const float neg_heading = ie_ftz(-f.camera_heading_64);
    ie_element_rotate(screen.element_at(r.north_14), {0.0F, neg_heading, 0.0F});
    if (r.waypoint_18 >= 0) {
        if (!f.waypoint_position) {
            screen.show_element(r.waypoint_18, 0);              // 00261874
        } else {
            const auto& p = f.player_position;
            const auto& w = *f.waypoint_position;
            const float dx = sub(p[0], w[0]), dy = sub(p[1], w[1]), dz = sub(p[2], w[2]);  // vsub 00261760
            // vmul.xyz, then vmulax.w / vmadday.w / vmaddz.w with vf0.w = 1.
            const float sq = madd(madd(mul(1.0F, mul(dx, dx)), 1.0F, mul(dy, dy)), 1.0F, mul(dz, dz));
            const float dist = add(0.0F, ie_ftz(std::sqrt(std::fabs(sq))));  // vsqrt Q, vaddq.x vf0.x + Q
            const float max = f.max_dist;
            if (dist <= max) {                                   // c.ole.s 002617b4
                screen.show_element(r.waypoint_18, 0);
            } else {
                const float a = game_atan2f(ie_ftz(-dx), ie_ftz(-dy));  // 002617cc
                ie_element_rotate(screen.element_at(r.waypoint_18), {0.0F, add(neg_heading, a), 0.0F});
                const float over = sub(dist, max);
                const float clamped = over < max ? over : max;  // min.s 00261808 (4614a829)
                const float span = static_cast<float>(hud_radar_waypoint_last - hud_radar_waypoint_first);
                if (max == 0.0F) fail("0026182c", "div.s by a zero RadarMaxDist (not modelled)");
                const float part = ie_ftz(mul(clamped, span) / max);
                const float value = sub(static_cast<float>(hud_radar_waypoint_last), part);
                screen.show_element(r.waypoint_18, 1);           // 0026183c
                screen.fade_element(r.waypoint_18, value);       // 00261848
            }
        }
    }
    if (r.marker_10 < 0) fail("0026191c", "003e8070 on a null player marker element");
    // 002618e8: the player's row 1 in the camera frame (00133190's transpose).
    const auto& v = f.player_row1;
    std::array<float, 3> c{};
    for (std::size_t k = 0; k < 3; ++k) {
        float acc = mul(f.camera_rows[k][0], v[0]);
        acc = madd(acc, f.camera_rows[k][1], v[1]);
        c[k] = madd(acc, f.camera_rows[k][2], v[2]);
    }
    const float a = game_atan2f(ie_ftz(-c[0]), c[1]);           // 00261900
    ie_element_rotate(screen.element_at(r.marker_10), {0.0F, ie_ftz(-a), 0.0F});
}

std::optional<float> hud_radar_meter(IeScreen& screen, const HudRadar& r, bool environmental, float value,
                                     const HudMeterVars& vars) {
    const int meter = environmental ? r.environmental_c : r.danger_8;
    const float scaled = mul(value, vars.danger_mag);          // 0026157c
    const float f = scaled < 1.0F ? scaled : 1.0F;             // min.s 0026158c (460008a9)
    std::optional<float> targets_30;
    if (!environmental) targets_30 = 1.0F;                     // 00261594
    if (meter < 0) return targets_30;
    if (f <= std::bit_cast<float>(0x3f7ae148U)) {               // c.ole.s 002615b0
        const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(vars.last) -
                                                    static_cast<std::uint32_t>(vars.first));
        const float x = add(static_cast<float>(vars.first), mul(static_cast<float>(span), f));
        screen.fade_element(meter, static_cast<float>(ie_cvt_w(x)));  // 00261664
        return targets_30;
    }
    auto& anim = screen.element_at(meter).anim;
    ie_anim_range(anim, static_cast<float>(vars.last), static_cast<float>(vars.dim), 1);  // 002615d4
    if (environmental) return targets_30;
    const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(vars.dim) -
                                                static_cast<std::uint32_t>(vars.last));
    if (span == 0) fail("00261614", "div.s by zero (dangerDimFrame == dangerLastFrame, not modelled)");
    return ie_ftz(sub(static_cast<float>(vars.dim), anim.t) / static_cast<float>(span));
}

} // namespace omega
