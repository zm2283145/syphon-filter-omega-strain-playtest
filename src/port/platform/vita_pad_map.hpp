#pragma once
// Port-only (checkpoint 132vt1, research/VITA.md "Input"): the PS Vita's buttons, sticks and touch panels as the
// DualShock 2 report the game reads (gamepad_reports.hpp). Host choice, not game data; portable (built and unit
// tested on every platform), the SceCtrl / SceTouch reads are in vita/gamepad_vita.cpp.
//
// Buttons: Cross / Circle / Square / Triangle, the D-pad, Select and Start map 1:1; the handheld's L / R shoulder
// buttons (SCE_CTRL_LTRIGGER / RTRIGGER) are the DualShock 2's L1 / R1; a PS TV's controller L3 / R3 clicks are
// L3 / R3. Digital buttons press with full pressure (0xff), as ds2_from_xinput's buttons do. The sticks' bytes are
// the report's (0 = left / up, 0x80 centre).
// Touch (the Vita has no L2 / R2 / L3 / R3): each panel is split into quadrants, top-left L2, top-right R2,
// bottom-left L3, bottom-right R3; a touch inside the edge dead zone (a fraction of the panel's width / height
// from each edge, where the grip rests) presses nothing. The rear and the front panel can each be switched off.

#include "gamepad_reports.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace omega {

namespace vita_ctrl {
// SceCtrlButtons (psp2/ctrl.h).
inline constexpr std::uint32_t select = 0x00000001, l3 = 0x00000002, r3 = 0x00000004, start = 0x00000008,
                               up = 0x00000010, right = 0x00000020, down = 0x00000040, left = 0x00000080,
                               ltrigger = 0x00000100, rtrigger = 0x00000200, triangle = 0x00001000,
                               circle = 0x00002000, cross = 0x00004000, square = 0x00008000;
}

struct VitaTouchPoint {
    int x = 0, y = 0;
};

// One panel's reports (SceTouchData) and its coordinate range (SceTouchPanelInfo min/max, inclusive).
struct VitaTouchPanel {
    std::array<VitaTouchPoint, 8> points{};
    int count = 0;
    int min_x = 0, min_y = 0, max_x = 1919, max_y = 1087;
};

struct VitaPadInput {
    std::uint32_t buttons = 0;  // SceCtrlData.buttons
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    VitaTouchPanel front, rear;
};

struct VitaTouchConfig {
    bool front = true, rear = true;
    float edge_dead_zone = 0.08F;  // of the panel's width / height, from each edge
};

// The DualShock 2 button (ds2::l2 / r2 / l3 / r3) a touch at (x, y) on that panel presses; 0 for none (dead zone).
std::uint16_t vita_touch_button(const VitaTouchPanel& panel, const VitaTouchPoint& point, float edge_dead_zone);
Ds2Pad ds2_from_vita(const VitaPadInput& in, const VitaTouchConfig& config);

// The touch settings from ux0:data/omega_strain/vita_input.ini lines (key=value, '#' comments): touch_front=0/1,
// touch_rear=0/1, touch_edge_dead_zone=0..0.45. Unknown keys and malformed values are reported in `problems`
// (one line each) and leave the setting as it was.
VitaTouchConfig parse_vita_touch_config(std::string_view text, std::string& problems);

}  // namespace omega
