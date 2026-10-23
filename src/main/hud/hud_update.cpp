#include "hud_update.hpp"

#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("HUD update: " + what); }

// cvt.w.s: truncation toward zero, saturating at the int range.
std::int32_t cvt_w(float v) {
    if (std::isnan(v)) fail("cvt.w.s of a NaN (the EE has none)");
    if (v >= 2147483647.0F) return INT_MAX;
    if (v <= -2147483648.0F) return INT_MIN;
    return static_cast<std::int32_t>(v);
}

// 00128e80 == 0 (stops at the first differing byte).
bool same_name(std::string_view a, std::string_view b) {
    const auto fold = [](unsigned char c) {
        if (c >= 0x80) fail("00128e80 with a byte >= 0x80 indexes before the ctype table 0x499c71");
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 0x20) : c;
    };
    for (std::size_t i = 0;; ++i) {
        const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        if (fold(x) != fold(y)) return false;
        if (!x) return true;
    }
}

constexpr float blink_red = std::bit_cast<float>(0x3e99999aU);  // 0.3f, the g and b of a blinking timer

} // namespace

std::string hud_timer_text(float seconds) {
    const std::int32_t whole = cvt_w(seconds);
    const std::int32_t minutes = cvt_w(seconds / 60.0F);
    const std::int32_t tenths = cvt_w(10.0F * seconds);
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%.2d:%.2d.%d", minutes, whole % 60, tenths % 10);
    if (n < 0 || n > 15) fail("00244f40's text overruns the 16-byte buffer at 00244d10's sp + 60");
    return std::string(buf, static_cast<std::size_t>(n));
}

bool hud_timer_update(HudTimer& t, float now, HudTimerWidgets& w) {
    const std::uint8_t live = t.end <= now ? 0 : 1;
    bool expired = false;
    if (t.running && !live) {
        t.running = 0;
        expired = true;
        t.blink = 5.0F + now;
    }
    t.running = live;
    if (!t.value || !t.description) return expired;
    w.set_flags(t.value, static_cast<std::uint16_t>(w.flags(t.value) & 0xfffdU));
    w.set_flags(t.description, static_cast<std::uint16_t>(w.flags(t.description) & 0xfffdU));
    if (!t.running && t.blink <= now) {
        t.blink = 0.0F;
        return expired;
    }
    const std::string text = hud_timer_text(t.running ? t.end - now : 0.0F);
    std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
    bool shown = true;
    if (!(t.blink <= now)) {
        if (cvt_w((t.blink - now) / 0.25F) & 1) {
            rgba[1] = rgba[2] = blink_red;
        } else {
            shown = false;
        }
    }
    if (shown) {
        w.set_colour(t.value, rgba);
        w.set_text(t.value, text);
        w.set_flags(t.value, static_cast<std::uint16_t>(w.flags(t.value) | 2U));
        if (w.text_length(t.description))
            w.set_flags(t.description, static_cast<std::uint16_t>(w.flags(t.description) | 2U));
    }
    t.shown = shown ? 1 : 0;
    return expired;
}

void hud_timer_start(HudTimer& t, float end, const char* text, HudTimerWidgets& w) {
    t.end = end;
    t.blink = 0.0F;
    t.running = 0;
    if (t.description) w.set_text(t.description, text ? text : "");
}

float hud_network_time(HudUpdateFields& hud, HudMissionCueHost& host) {
    if (!host.multiplayer()) return hud.clock_6dc;
    const float now = static_cast<float>(host.network_clock()) / 1000.0F;
    if (hud.clock_6e0 < 0.0F) hud.clock_6e0 = now;
    return now;
}

float hud_mission_time(HudUpdateFields& hud, HudMissionCueHost& host) {
    if (!host.multiplayer()) return hud.clock_6dc;
    const float now = hud_network_time(hud, host);
    return now - hud.clock_6e0;
}

std::int32_t hud_par_time(const std::vector<AwardLevelRow>& levels, std::string_view level, bool multiplayer) {
    for (const auto& row : levels)
        if (same_name(row.name, level)) return multiplayer ? row.pair_second : row.pair_first;
    return 0;
}

void hud_mission_cues(HudUpdateFields& hud, HudUpdateStatics& statics, float dt, HudMissionCueHost& host) {
    {
        hud.clock_6dc += dt;
        const bool mp = host.multiplayer();
        const float now = hud_mission_time(hud, host);
        const float par = static_cast<float>(host.par_time(mp));
        const float quarter = par / 4.0F;
        const float half = par / 2.0F;
        const float three_quarters = 0.75F * par;
        const std::int32_t total = host.objective_total();
        const float last = statics.last_mission_time;
        if (last < quarter && !(now < quarter)) {
            const std::int32_t done = host.objectives_completed();
            if (mp && done == 0) host.mission_cue(2);
        } else if (last < half && !(now < half)) {
            const std::int32_t done = host.objectives_completed();
            if (done < total / 2) host.mission_cue(3);
            else if (mp) host.mission_cue(2);
        } else if (last < three_quarters && !(now < three_quarters)) {
            const std::int32_t done = host.objectives_completed();
            if (static_cast<float>(done) < 0.75F * static_cast<float>(total)) host.mission_cue(4);
        }
        statics.last_mission_time = now;
    }
}

void hud_update_timers(HudUpdateFields& hud, HudTimerHost& host) {
    if (hud_timer_update(hud.timer_6ac, hud.clock_6dc, host)) host.timer_message(2, 1);
    const bool skip = hud.timer_6ac.running != 0 || !(hud.timer_6ac.blink <= hud.timer_6ac.end);
    if (!skip) {
        const float now = hud_network_time(hud, host);
        if (hud_timer_update(hud.timer_6c4, now, host) && (!host.multiplayer() || host.byte_5721c0()))
            host.timer_message(1, 0);
    }
}

float hud_global_timer_start(HudUpdateFields& hud, float seconds, const char* label, HudTimerWidgets& widgets,
                             HudMissionCueHost& host) {
    const float end = seconds + hud_network_time(hud, host);
    hud_timer_start(hud.timer_6c4, end, label, widgets);
    return end;
}

void hud_update(HudUpdateFields& hud, HudUpdateStatics& statics, float dt, HudUpdateHost& host) {
    if (!host.byte_4ffbb7()) hud_mission_cues(hud, statics, dt, host);
    host.ticker_update(dt);
    if (hud.word_6e8) host.list_6e8(dt);
    const std::uint32_t player = host.player();
    host.radar(player);
    host.targets(player);
    host.ammo(player);
    host.screen_update(dt);
    if (hud.map_6a0) host.map_update(dt);
    hud_update_timers(hud, host);
    if (!host.byte_4ffc29() || host.multiplayer()) {
        host.gui_update(dt);
        if (host.byte_4ffba0() != 0x13) host.gui_415870();
        host.gui_vt60(dt);
    }
}

} // namespace omega
