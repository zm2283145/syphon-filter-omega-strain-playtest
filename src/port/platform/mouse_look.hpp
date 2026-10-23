#pragma once
// Port-only mouse look (checkpoint 132win7, not the original: the PS2 game reads only the DualShock 2 pad).
// The mouse's relative motion (raw input, host_window.hpp) moves the right analog stick: a frame's motion
// becomes the pad report's right-stick bytes 2 / 3 (0055a44c's report, gamepad_reports.hpp), so the camera
// sees exactly a controller's right-stick deflection and nothing else of the game changes. A frame with no
// motion leaves the bytes as they were (the controller's, the look keys' or the centre).
//
// The rule (per axis, per game frame; the stick is a rate, so the mouse's speed is the turn rate):
//   m = counts * sensitivity / mouse_look_full_counts, clamped to [-1, 1] (24 counts in one frame at
//       sensitivity 1 is a full deflection)
//   offset = sign(m) * (dead + (127.5 - dead) * |m|), with dead = mouse_look_dead_bytes (58, just past
//       the game's right-stick dead zone of 56 bytes, PlayerPadAxisTuning, caller 00165de8), so the
//       smallest motion still turns the camera
//   the base byte's own deflection is kept when it is outside the dead zone (a held stick plus the mouse),
//   byte = clamp(round(127.5 + base offset + offset), 0, 255)
// Y: the mouse moving down is the stick pushed down (report byte 3 up); invert_y flips it. The game's own FREELOOK
// X / Y options (input +125 / +126, 002503e0) then apply to the mouse as to the stick: with the default FREELOOK Y
// (0) the stick up, and so the mouse up, tilts the free look down, as on the PS2 pad.
// What the stick does is the game's: on foot (pad selectors 0-4, 6, 7, 9-11) 002503e0 makes the deflection a held
// free-look offset (up to 160 degrees yaw / 60 degrees pitch) that recentres when the stick is released, so the mouse
// glances while it moves and the view returns when it stops; in the aim view (selector 8, 0024e1d0) the deflection
// turns the aim at its rate.
//
// Port settings keys (port_settings.hpp keeps them as unknown keys): mouse_look=0|1 (default 1),
// mouse_sensitivity=<0.05..20> (default 1), mouse_invert_y=0|1 (default 0). The cursor is captured (hidden
// and clipped to the client area) only while the level plays in the focused window: it is released in the
// menus, the pause, the level end, the front end and whenever the window loses focus or is minimised.

#include <cstdint>
#include <string_view>
#include <vector>

namespace omega {

struct PortSettings;

inline constexpr float mouse_look_full_counts = 24.0F;
inline constexpr float mouse_look_dead_bytes = 58.0F;

struct MouseLookSettings {
    bool enabled = true;
    float sensitivity = 1.0F;
    bool invert_y = false;
};

// Throws on a malformed value.
MouseLookSettings mouse_look_settings(const PortSettings& settings);

struct StickBytes {
    std::uint8_t x = 0x80, y = 0x80;
    bool operator==(const StickBytes&) const = default;
};

// The right-stick bytes after a frame's mouse motion (dx right, dy down, in raw counts).
StickBytes mouse_look_stick(StickBytes base, int dx, int dy, const MouseLookSettings& settings);
// A key stick deflection (-1..1, +y up as the play input) as report bytes.
StickBytes stick_bytes_from_deflection(float x, float y);

// Port-only test (omega_play --test-mouse): simulated raw mouse motion by play frame, "F:dx/dy" or
// "F-G:dx/dy" (every frame F..G), comma separated, in frame order. Throws on a malformed list.
struct MouseMotionEvent {
    std::uint64_t first{}, last{};
    int dx{}, dy{};
};
std::vector<MouseMotionEvent> parse_mouse_motion(std::string_view list);

// Whether the cursor is captured: mouse look wanted (a level playing, no menu / pause) in a visible, focused,
// not minimised window.
inline bool mouse_capture_active(bool wanted, bool visible, bool focused, bool minimized) {
    return wanted && visible && focused && !minimized;
}

}  // namespace omega
