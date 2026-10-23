#pragma once
// Checkpoint 126c: the level loading screen (hud_loading.cc, the 0x3c-byte
// object 0x4a8ee0 names) and the loading thread's frame (machine.cc
// 00166430 / 00166840).
//
// The level load 001350e0 mounts "GAMEDATA\<level>\LOADING.HOG" (0x49aa70;
// "GAMEDATA\COMMON\LOADING.HOG" 0x49aa90 when 0036def0 finds none) and calls
// 00166430(app, 0, path): with the file, 003f96e0 mounts it (app +270), the
// object is allocated (machine.cc line 0x7a9), built by 002721b0(obj, hog,
// "loading_screen"), set up by 00271d80 and 00271b10(obj, 0.0) and stored at
// app +274; then the thread globals (0x4ee500 = app +274, 0x4ee508 = app
// +280 the default font, 0x4ee518 = 003fdb50("LoadingText"), 0x4ee510 =
// 003fdb50("Continue")), the thread 00166840 (00166690, priority 5) and the
// vblank handler 001667c0 (00166650, INTC 2).
//
// Object layout (00272170 zeroes +4..+38):
//   +0  the IE screen (00241b70: "%s.IE" through 0036e1b0, 003e7b00)
//   +4  the progress element (00271b10's), never written past 00272170
//   +8  background_text   +c  filmstrip   +10 bar   +14 dotsbottom
//   +18 whiteflash   +1c backgroundmap1   +20 backgroundmap2   +24 pressx
//   +28 host   +2c briefing   +30 levelname1   +34 levelname2
//   +38 loading_data_text

#include "../gui/gui_draw.hpp"
#include "hud_screen_draw.hpp"
#include "ie_draw.hpp"
#include "ie_screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace omega {

struct LoadingScreen {
    IeScreen screen;              // +0
    int progress_04 = -1;         // +4
    int background_text_08 = -1;  // +8
    int filmstrip_0c = -1;        // +c
    int bar_10 = -1;              // +10
    int dotsbottom_14 = -1;       // +14
    int whiteflash_18 = -1;       // +18
    int backgroundmap1_1c = -1;   // +1c
    int backgroundmap2_20 = -1;   // +20
    int pressx_24 = -1;           // +24
    int host_28 = -1;             // +28
    int briefing_2c = -1;         // +2c
    int levelname1_30 = -1;       // +30
    int levelname2_34 = -1;       // +34
    int loading_data_text_38 = -1;  // +38
    bool destroyed = false;
};

// 003fdb50 (GetString) over the mission's string pool (003fdca0 loads it
// before 00166430): nullopt for an unknown key.
using LoadingStrings = std::function<std::optional<std::string>(const std::string& key)>;

// The colour block 0x4a8da0 (the static initialiser 004d1df0 writes 1, 1,
// 1, 1; the file data reads 0).
inline constexpr std::array<float, 4> loading_text_colour{1.0F, 1.0F, 1.0F, 1.0F};

// 002721b0(obj, hog, name): 00241b70 (the screen over the decoded IE file;
// text_word is *0055a278, the callout font 00165950 stores from app +284
// unless a level script's SetInterfaceFont changed it), 003eebe0(root, 0, 1),
// then 00272170 (+4..+38 null).
LoadingScreen loading_screen_create(const IeFile& file, std::uint32_t text_word, IeTextureService* textures);
// 00272200(root, name, text): 00260d50 (find_child + create_element); with
// the element, the text and the element's text object: 003e7ec0 (003d08b0
// set_text), element +19 = 0, 003d0810(text, 0x4a8da0) and text +20 =
// *(app + 0x280) (font_word). Answers the element (-1 when none) even
// without a text.
int loading_text_element(IeScreen& screen, std::string_view name, const std::optional<std::string>& text,
                         std::uint32_t font_word);
// 00271d80(obj): 003eebe0(root, 1, 1); in the root child
// "background_panels_group" (003eeb30): filmstrip (+c, 003f4510 0..300),
// dotsbottom (+14, 0..60), bar (+10, 0..60), background_text (+8, 0..60),
// all loop 0; then from the root pressx (+24, hidden), with a "Continue"
// string loading_data_text (+38, 00272200 then hidden), host (+28, hidden),
// whiteflash (+18, 0..17), backgroundmap1 / 2 (+1c / +20, 0..300),
// levelname1 (+30) with level_techname1, else level_name, else the level
// name 0x4ffc90 itself, levelname2 (+34) with level_techname2 and briefing
// (+2c) with level_briefing (each only when the string exists).
void loading_screen_setup(LoadingScreen& s, const LoadingStrings& strings, const std::string& level,
                          std::uint32_t font_word);
// 00271b10(obj, p): p clamped to [0, 1]; with +4, 003f44e0(+4, 10 + 7p) and
// 003f43d0(+4, 7.0). +4 is never set, so nothing changes.
void loading_screen_progress(LoadingScreen& s, float p);
// 00271ba0(obj, dt) = 003e79c0(screen, dt).
void loading_screen_update(LoadingScreen& s, float dt);
// 00271bb0(obj): the 2D camera record (+f0 = 0, +f8 = 100.0, +106 = +104 =
// 1; host 2D mapping, not modelled) and 003e7670(screen).
void loading_screen_draw(GuiDrawList& out, IeDrawState& st, LoadingScreen& s);
// 00271bf0(obj, v): +24 and +38 shown = v, +28 hidden.
void loading_screen_show_continue(LoadingScreen& s, std::uint8_t v);
// 00271c60(obj, v): +28 shown = v, +24 and +38 hidden.
void loading_screen_show_host(LoadingScreen& s, std::uint8_t v);
// 00271cd0 (the destructor): 00272280 (003e8eb0 when set) on +4..+38, then
// 003eebe0(root, 0, 1).
void loading_screen_destroy(LoadingScreen& s);

// The thread's globals (00166430 and the vblank handler).
struct LoadingThreadGlobals {
    LoadingScreen* screen = nullptr;        // 0x4ee500
    std::uint32_t font = 0;                 // 0x4ee508 (app +280)
    std::optional<std::string> loading_text;  // 0x4ee518, "LoadingText"
    std::optional<std::string> continue_text; // 0x4ee510, "Continue"
    std::int32_t frames = 1200;             // 0x4ee4d0 (atoi of <LEVEL>_LOADINGFRAMES, else 1200)
    std::uint8_t frames_set = 0;            // 0x4ee4d8
    std::int32_t vblanks = 0;               // 0x4ee4c8
    std::int32_t countdown = 0;             // 0x4ee4f8
    std::uint8_t continue_4f0 = 0;          // 0x4ee4f0 (00165e60 with a2 = 0 only: stays 0)
    std::uint8_t host_4e0 = 0;              // 0x4ee4e0
    std::uint8_t started_528 = 0;           // 0x4ee528 (the thread's first-run flag)
    std::uint64_t last_ticks = 0;           // 0x4ee520
};
// *0x48a050, the frame countdown base (read only: 1).
inline constexpr std::int32_t loading_frame_skip = 1;

// 001667c0, the vblank handler: vblanks + 1; with the thread (0x48a030 !=
// -1), countdown 0 wakes it (iWakeupThread 0010e010, true), else countdown
// - 1.
bool loading_vblank_1667c0(LoadingThreadGlobals& g, bool thread_exists = true);

// The EE clock the thread reads: (u32)(T0_COUNT + *00533870) as 0x166c08's
// unsigned float / 5.76, truncated, added to the 64-bit *00533878 (10 us
// ticks).
std::uint64_t loading_clock_ticks(std::uint32_t t0_count, std::uint32_t t0_base, std::uint64_t ticks_533878);
// 0x166c64..0x166d08: the ticks' low word (sign extended) remainder by
// 100000 as a float / 100000.0 (f), then alpha 2f when f <= 0.5, else
// 2(1 - f).
float loading_text_alpha(std::uint64_t ticks);

struct LoadingFrameServices {
    // A clock read (each read is one T0_COUNT sample in the thread).
    std::function<std::uint64_t()> ticks;
    // The font behind a handle (0036a300's +0x4ee508; the text objects' +20).
    std::function<const GuiFont*(std::uint32_t)> font;
};
// One frame of the thread 00166840 after its wakeup (0x166850..0x166e50):
// the first run sets 0x4ee520 to the clock; countdown = *0x48a050 + 1;
// 0037a9f0 (the frame begin, the host's clear) and *005392d0 = 0; with
// 0x4ee4f0 and the screen, 00271c60 (0x4ee4e0) or 00271bf0 instead of the
// text; else the "LoadingText" string through 0036a300(font, s, (1, 1, 1,
// alpha), -270, -190, 0.001); then, with the screen, 00271ba0(dt = ticks
// since 0x4ee520 / 100000), 0x4ee520 = now, 00271b10(vblanks / frames),
// 00271bb0 and 003d0c10(003d1020()) (the listed text objects' state then
// their 003cfab0 draw; now_ms from the same clock, ie_text_clock_ms).
void loading_frame_166850(GuiDrawList& out, IeDrawState& st, LoadingThreadGlobals& g,
                          const LoadingFrameServices& services);

}  // namespace omega
