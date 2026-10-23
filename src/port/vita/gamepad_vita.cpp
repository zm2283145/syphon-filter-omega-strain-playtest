// Port-only (checkpoint 132vt1, research/VITA.md "Input"): gamepad_device.hpp on the PS Vita. The handheld's built-in
// controls (SceCtrl port 0, analog mode) and both touch panels (SceTouch) become the DualShock 2 report through
// vita_pad_map.hpp; the touch settings come from ux0:data/omega_strain/vita_input.ini. The Vita has no rumble motor,
// so set_vibration does nothing. The test simulation (gamepad_simulation.hpp) is a Windows HID / XInput stand-in and
// is not used here.

#include "gamepad_device.hpp"
#include "vita_file.hpp"
#include "vita_pad_map.hpp"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace omega {

struct GamepadDevice::Impl {
    VitaTouchConfig touch;
    VitaTouchPanel front, rear;
};

namespace {

void panel_range(SceUInt32 port, VitaTouchPanel& panel) {
    SceTouchPanelInfo info{};
    if (sceTouchGetPanelInfo(port, &info) < 0 || info.maxAaX <= info.minAaX || info.maxAaY <= info.minAaY) return;
    panel.min_x = info.minAaX;
    panel.min_y = info.minAaY;
    panel.max_x = info.maxAaX;
    panel.max_y = info.maxAaY;
}

void read_panel(SceUInt32 port, VitaTouchPanel& panel) {
    SceTouchData data{};
    panel.count = 0;
    if (sceTouchPeek(port, &data, 1) < 1) return;
    const int n = std::min<int>(int(data.reportNum), int(panel.points.size()));
    for (int i = 0; i < n; ++i) panel.points[std::size_t(i)] = {data.report[i].x, data.report[i].y};
    panel.count = n;
}

}  // namespace

GamepadDevice::GamepadDevice(GamepadSimulation*) : impl_(std::make_unique<Impl>()) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    panel_range(SCE_TOUCH_PORT_FRONT, impl_->front);
    panel_range(SCE_TOUCH_PORT_BACK, impl_->rear);
    std::ifstream ini(std::string(vita_data_root) + "/vita_input.ini");
    if (ini) {
        std::ostringstream text;
        text << ini.rdbuf();
        std::string problems;
        impl_->touch = parse_vita_touch_config(text.str(), problems);
        if (!problems.empty()) std::fprintf(stderr, "vita_input.ini: %s", problems.c_str());
    }
}

GamepadDevice::~GamepadDevice() = default;

std::optional<GamepadDevice::Reading> GamepadDevice::read() {
    SceCtrlData ctrl{};
    if (sceCtrlPeekBufferPositive(0, &ctrl, 1) < 1) return std::nullopt;
    VitaPadInput in;
    in.buttons = ctrl.buttons;
    in.lx = ctrl.lx;
    in.ly = ctrl.ly;
    in.rx = ctrl.rx;
    in.ry = ctrl.ry;
    read_panel(SCE_TOUCH_PORT_FRONT, impl_->front);
    read_panel(SCE_TOUCH_PORT_BACK, impl_->rear);
    in.front = impl_->front;
    in.rear = impl_->rear;
    return Reading{ds2_from_vita(in, impl_->touch), "PS Vita"};
}

void GamepadDevice::set_vibration(bool, std::uint8_t) {}

}  // namespace omega
