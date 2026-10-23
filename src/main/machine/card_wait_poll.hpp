#pragma once
// 002cc260(a0, a1), the app's nested GUI service, in its card-wait form: *0055a838 = 00441c10 is
// `002cc260(0, 0)` (a tail call), run once per pass of every waiting card call (0040e270 & co.)
// while a save flow polls (research/FRONTEND.md "The nested GUI redraw during card waits" and
// "Checkpoint 132f8").
//   1. byte 0051eeb0 set: return at once (re-entry guard); else set it;
//   2. a0 == 0: with app +310 (the character data, 002c82f0) set and a1 != 0x3039, skip;
//      else 002cbb30 (the GUI frame) then 002cbad0 (its draw and the present 0037a9f0); both
//      return at once unless app +28 (the GUI is running) or +2d is set;
//   3. 0051eeb0 = 0.
// a0 = 1, 2 and 3 (the pad / 0x2001 / 0x2002 forms) are not reached through 00441c10.
#include <cstdint>
#include <functional>

namespace omega {

struct CardWaitApp {
    bool gui_28{};            // app +28 (002cb3d0 sets it; the level hand-off's 002c9860 / 002cb160 clear it)
    bool flag_2d{};           // app +2d (002cc5a0 clears it; no store in the app's code sets it)
    bool character_310{};     // app +310
};

struct CardWaitPollHost {
    std::function<void()> gui_frame;   // 002cbb30's body past its +28 / +2d test
    std::function<void()> gui_draw;    // 002cbad0's body past its +28 / +2d test (vt +5c, 0037a9f0)
};

enum class CardWaitPoll : std::uint8_t {
    reentered,      // 0051eeb0 was set
    character_skip, // app +310 with a1 != 0x3039
    idle,           // 002cbb30 / 002cbad0 ran and returned at once (+28 and +2d clear)
    drawn,          // the nested GUI frame and present ran
};

// 00441c10: 002cc260(0, 0). `guard` is byte 0051eeb0.
CardWaitPoll card_wait_poll(std::uint8_t& guard, const CardWaitApp& app, const CardWaitPollHost& host);

}  // namespace omega
