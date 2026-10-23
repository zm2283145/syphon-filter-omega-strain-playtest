#pragma once
// The in-play HUD update 00244fa0(hud *004ffc2c, dt) (hud.cc), called by the
// render pass (00133420 with the world dt of 00133340), with its leaves:
//   00244d10 the mission timer update (the two 0x18-byte timers at +6ac and
//            +6c4, drawn through the GAMEGUI "value_timer" / "value_timer_
//            description" text widgets of 00245f00),
//   00244ee0 the timer start, 00244f40 the timer text ("%.2d:%.2d.%d",
//            0x4a7298, through sprintf 00128bd0),
//   00245810 / 00245890 the mission clock (+6dc, or in multiplayer the
//            network clock 0042b1a0(0) / 1000 less its first reading +6e0),
//   00330c30 the level's par row (*00532af8 rows through 003fdae0, matched
//            by 00128e80, then *00532b98 + 8 i, +4 in multiplayer).
// Everything else the update calls goes through HudUpdateHost, in the
// original order; see hud_update below.
#include "agent_refill.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// A HUD timer (0x18 bytes): +0 end time, +4 running, +8 blink deadline,
// +c shown, +10 the description widget, +14 the value widget (0 = none).
struct HudTimer {
    float end = 0.0F;
    std::uint8_t running = 0;
    float blink = 0.0F;
    std::uint8_t shown = 0;
    std::uint32_t description = 0;
    std::uint32_t value = 0;
};

// The GuiTextWidget operations the timers use (widgets are the handles the
// HUD stored at +6bc / +6c0 / +6d4 / +6d8).
class HudTimerWidgets {
public:
    virtual ~HudTimerWidgets() = default;
    virtual std::uint16_t flags(std::uint32_t widget) = 0;                     // the halfword +14
    virtual void set_flags(std::uint32_t widget, std::uint16_t flags) = 0;
    virtual void set_colour(std::uint32_t widget, const std::array<float, 4>& rgba) = 0;  // 0041c070
    virtual void set_text(std::uint32_t widget, const std::string& text) = 0;   // 0041beb0(widget, text, 0)
    virtual std::uint32_t text_length(std::uint32_t widget) = 0;                // 002071f0(widget + 48)
};

// What the mission clock and the mission-time cues (0x00244fd0..0x00245198)
// read and call.
class HudMissionCueHost {
public:
    virtual ~HudMissionCueHost() = default;
    virtual bool multiplayer() = 0;        // byte 005721c8
    virtual std::int32_t network_clock() = 0;  // 0042b1a0(0), in multiplayer
    virtual std::int32_t par_time(bool multiplayer) = 0;  // 00330c30(004ffc90, mp): see hud_par_time
    virtual std::int32_t objective_total() = 0;           // 00227120(00225cd0())
    virtual std::int32_t objectives_completed() = 0;      // 002270c0(00225cd0(), sp + c0, 0x14, 1)
    virtual void mission_cue(std::int32_t a1) = 0;        // 00245400(0x15, a1)
};

// What 00244fa0's timer part reads and calls (see hud_update_timers).
class HudTimerHost : public HudTimerWidgets, public HudMissionCueHost {
public:
    virtual bool byte_5721c0() = 0;        // read only in multiplayer
    // The message 003c9e30(msg, 004f7a08), vtable 004dba00, byte +20 = kind,
    // word +24 = value, sent by 003c9420(msg, *(*004ffc0c + 30), 0) and
    // destroyed (vtable 004dcf10, 003c9d60(msg, 0)).
    virtual void timer_message(std::uint8_t kind, std::int32_t value) = 0;
};

class HudUpdateHost : public HudTimerHost {
public:
    virtual bool byte_4ffbb7() = 0;        // game + 67: nonzero skips the mission-time cues
    virtual void ticker_update(float dt) = 0;             // 003f9e60(hud + 100, dt)
    virtual void list_6e8(float dt) = 0;                  // 00242330(hud, dt), when the word +6e8 is set
    virtual std::uint32_t player() = 0;                   // 0040c540(004f7db8)
    virtual void radar(std::uint32_t player) = 0;         // (*(hud + d4))[+8](hud + d0, player): 00261690
    virtual void targets(std::uint32_t player) = 0;       // 00273b40(hud + 90, player)
    virtual void ammo(std::uint32_t player) = 0;          // 00242190(hud, player)
    virtual void screen_update(float dt) = 0;             // 003e79c0(*(hud + 54), dt)
    virtual void map_update(float dt) = 0;                // 0026d620(*(hud + 6a0), dt), when +6a0 is set
    virtual bool byte_4ffc29() = 0;                       // game + d9
    virtual std::uint8_t byte_4ffba0() = 0;               // game + 50
    virtual void gui_update(float dt) = 0;                // 00419260(00414790(), dt)
    virtual void gui_415870() = 0;                        // 00415870(004147a0()), unless +50 == 0x13
    virtual void gui_vt60(float dt) = 0;                  // (*004147a0())[+60](004147a0(), dt)
};

// The HUD fields the update reads and writes.
struct HudUpdateFields {
    float clock_6dc = 0.0F;     // the mission clock (constructor 0)
    float clock_6e0 = 0.0F;     // the first network clock reading (multiplayer)
    std::uint32_t word_6e8 = 0;
    std::uint32_t map_6a0 = 0;
    HudTimer timer_6ac;
    HudTimer timer_6c4;
};

// 00244f40: sprintf(buf, "%.2d:%.2d.%d", trunc(t / 60), trunc(t) % 60,
// trunc(10 t) % 10) (cvt.w.s truncation, EE div remainders). The buffer is
// the 16 bytes at 00244d10's sp + 60: a longer text overruns the frame and
// throws.
std::string hud_timer_text(float seconds);
// 00244d10(timer, now): returns 1 when a running timer has expired.
bool hud_timer_update(HudTimer& timer, float now, HudTimerWidgets& widgets);
// 00244ee0(timer, end, text): a null text is "" (0x4a7218).
void hud_timer_start(HudTimer& timer, float end, const char* text, HudTimerWidgets& widgets);
// 00245810(hud): in multiplayer the network clock in seconds (its first
// reading fills +6e0 while +6e0 < 0); else +6dc.
float hud_network_time(HudUpdateFields& hud, HudMissionCueHost& host);
// 00245890(hud): the mission time (multiplayer: 00245810 - +6e0).
float hud_mission_time(HudUpdateFields& hud, HudMissionCueHost& host);
// 00330c30(level, mp): the first row whose name equals the level (00128e80,
// case folded through the ctype table; a byte >= 0x80 throws), its pair +4
// in multiplayer else +0; 0 with no row.
std::int32_t hud_par_time(const std::vector<AwardLevelRow>& levels, std::string_view level, bool multiplayer);

// The static float 004f7dd8 (BSS, 0 at boot; only 00244fa0 reads and writes it).
struct HudUpdateStatics {
    float last_mission_time = 0.0F;
};

// 0x00244fd0..0x00245198, 00244fa0's head while byte 004ffbb7 is clear: +6dc
// += dt; now = 00245890; par = 00330c30; the objective total (always read);
// then for the first of the par's quarter / half / three quarters that the
// mission time crossed since the static 004f7dd8 (last < mark <= now), with
// the completed count read then: 1/4 cue 2 in multiplayer with none done;
// 1/2 cue 3 while fewer than total / 2 (EE div) are done, else cue 2 in
// multiplayer; 3/4 cue 4 while (float) done < 0.75 (float) total. The static
// then takes now. The cue is 00245400(0x15, n): the voice line
// "<level cue>_21_<n>" (game_sound_voice.hpp mission_cue_line_name).
void hud_mission_cues(HudUpdateFields& hud, HudUpdateStatics& statics, float dt, HudMissionCueHost& host);

// 00244fa0's timers, after the map update: 00244d10 on +6ac against +6dc
// (expiry: timer_message(2, 1)); then, unless +6ac is running or blinking
// (its blink deadline past its end), 00244d10 on +6c4 against 00245810
// (expiry: timer_message(1, 0), in multiplayer only with byte 005721c0).
void hud_update_timers(HudUpdateFields& hud, HudTimerHost& host);

// 00245560(hud, label, seconds)'s timer (StartGlobalTimer 0012f860 ->
// 0012f890 -> 00245560(*004ffc2c, 0, seconds)): 00244ee0(+6c4, seconds +
// 00245810(hud), label) (add.s 0x00245588: seconds + time). Returns the end.
// The E_TIMER sound (0020faa0(msg, 004f0f90), +20 = 1, posted to 004ffb50)
// and the multiplayer message (byte 005721c8: 004f7dc0 to the player) are
// the caller's.
float hud_global_timer_start(HudUpdateFields& hud, float seconds, const char* label, HudTimerWidgets& widgets,
                             HudMissionCueHost& host);

// 00244fa0(hud, dt).
void hud_update(HudUpdateFields& hud, HudUpdateStatics& statics, float dt, HudUpdateHost& host);

} // namespace omega
