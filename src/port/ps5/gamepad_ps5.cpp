// Port-only (132ps5c, research/PS5.md "Interfaces"): gamepad_device.hpp on the PS5. The DualSense is read natively
// through scePad (the first logged-in user's controller, opened again when users log in or out) and becomes the
// DualShock 2 report with the PC's mapping for the same controller (gamepad_reports.hpp "DualShock 4 / DualSense: the
// PlayStation layout as printed"): face buttons, D-pad, L1 / R1, L2 / R2 with their analog pressure, L3 / R3, OPTIONS
// as START. The Create button belongs to the system on the PS5, so the touch pad click is SELECT (the PC maps
// Create / Share to SELECT). Sticks as reported (0 = left / up, 0xff = right / down, as the DualShock 2's bytes).
// set_vibration drives the DualSense's motors through scePadSetVibration (large motor = the DualShock 2's large one,
// small motor on = full speed, as xinput_vibration). The test simulation (gamepad_simulation.hpp) is a Windows HID /
// XInput stand-in and is not used here. While the system menu owns the pad (the intercepted bit) it reads as released.

#include "gamepad_device.hpp"

#include "ps5_log.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {

struct Ps5PadTouch {
    std::uint16_t x, y;
    std::uint8_t finger;
    std::uint8_t pad[3];
};

// ScePadData (the layout of the public PS4 / PS5 homebrew SDKs and the Melee PS5 port).
struct Ps5PadData {
    std::uint32_t buttons;
    struct {
        std::uint8_t x, y;
    } left_stick, right_stick;
    struct {
        std::uint8_t l2, r2;
    } analog_buttons;
    std::uint16_t padding;
    float quat[4];
    float vel[3];
    float accel[3];
    struct {
        std::uint8_t fingers;
        std::uint8_t pad1[3];
        std::uint32_t pad2;
        Ps5PadTouch touch[2];
    } touch;
    std::uint8_t connected;
    std::uint64_t timestamp;
    std::uint8_t ext[16];
    std::uint8_t count;
    std::uint8_t unknown[15];
};

struct Ps5PadVibration {
    std::uint8_t large_motor;
    std::uint8_t small_motor;
};

}  // namespace

extern "C" {
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, void* param);
int scePadReadState(int handle, Ps5PadData* data);
int scePadClose(int handle);
int scePadSetVibration(int handle, const Ps5PadVibration* param);
}

namespace omega {
namespace {

// ScePadButtonDataOffset.
constexpr std::uint32_t pad_l3 = 0x2, pad_r3 = 0x4, pad_options = 0x8, pad_up = 0x10, pad_right = 0x20,
                        pad_down = 0x40, pad_left = 0x80, pad_l2 = 0x100, pad_r2 = 0x200, pad_l1 = 0x400,
                        pad_r1 = 0x800, pad_triangle = 0x1000, pad_circle = 0x2000, pad_cross = 0x4000,
                        pad_square = 0x8000, pad_touch_pad = 0x100000, pad_intercepted = 0x80000000u;
constexpr int user_invalid = -1;

void press(Ds2Pad& pad, std::uint16_t button, bool on, std::uint8_t pressure = 0xff) {
    if (!on) return;
    pad.held |= button;
    for (std::size_t k = 0; k < ds2::pressure_mask.size(); ++k)
        if (ds2::pressure_mask[k] == button) pad.pressure[k] = pressure;
}

}  // namespace

struct GamepadDevice::Impl {
    std::mutex mutex;
    bool ready = false;
    int user = user_invalid;
    int handle = -1;
    unsigned reads = 0;
    std::uint32_t last_buttons = 0;
    unsigned logged = 0;

    // The first logged-in user's pad (opened again when that user logs out or another logs in first).
    void detect() {
        int users[4]{user_invalid, user_invalid, user_invalid, user_invalid};
        if (sceUserServiceGetLoginUserIdList(users) != 0) return;
        int first = user_invalid;
        for (int u : users)
            if (u != user_invalid) {
                first = u;
                break;
            }
        if (first == user && handle >= 0) return;
        if (handle >= 0) scePadClose(handle);
        handle = -1;
        user = first;
        if (user == user_invalid) return;
        handle = scePadOpen(user, 0, 0, nullptr);
        if (handle < 0) {
            ps5::log("Gamepad (PS5): scePadOpen(user 0x%08x) failed: 0x%08x", unsigned(user), unsigned(handle));
            handle = -1;
        } else {
            ps5::log("Gamepad (PS5): DualSense of user 0x%08x, handle %d", unsigned(user), handle);
        }
    }
};

GamepadDevice::GamepadDevice(GamepadSimulation*) : impl_(std::make_unique<Impl>()) {
    const int init = scePadInit();
    impl_->ready = init == 0;
    if (!impl_->ready) ps5::log("Gamepad (PS5): scePadInit failed: 0x%08x", unsigned(init));
    std::lock_guard lock(impl_->mutex);
    if (impl_->ready) impl_->detect();
}

GamepadDevice::~GamepadDevice() {
    if (impl_->handle >= 0) scePadClose(impl_->handle);
}

std::optional<GamepadDevice::Reading> GamepadDevice::read() {
    auto& d = *impl_;
    std::lock_guard lock(d.mutex);
    if (!d.ready) return std::nullopt;
    if (d.handle < 0 || ++d.reads % 60 == 0) d.detect();  // users log in and out at any time: once a second
    if (d.handle < 0) return std::nullopt;
    Ps5PadData data;
    std::memset(&data, 0, sizeof data);
    if (scePadReadState(d.handle, &data) != 0 || !data.connected) return std::nullopt;
    std::uint32_t b = data.buttons;
    if (b & pad_intercepted) b = 0;  // the system menu owns the pad
    if (b != d.last_buttons && d.logged < 40) {
        ++d.logged;
        ps5::log("Gamepad (PS5): buttons 0x%08x", unsigned(b));
    }
    d.last_buttons = b;
    Ds2Pad pad;
    pad.left_x = data.left_stick.x;
    pad.left_y = data.left_stick.y;
    pad.right_x = data.right_stick.x;
    pad.right_y = data.right_stick.y;
    press(pad, ds2::up, (b & pad_up) != 0);
    press(pad, ds2::right, (b & pad_right) != 0);
    press(pad, ds2::down, (b & pad_down) != 0);
    press(pad, ds2::left, (b & pad_left) != 0);
    press(pad, ds2::square, (b & pad_square) != 0);
    press(pad, ds2::cross, (b & pad_cross) != 0);
    press(pad, ds2::circle, (b & pad_circle) != 0);
    press(pad, ds2::triangle, (b & pad_triangle) != 0);
    press(pad, ds2::l1, (b & pad_l1) != 0);
    press(pad, ds2::r1, (b & pad_r1) != 0);
    press(pad, ds2::l2, (b & pad_l2) != 0, data.analog_buttons.l2);
    press(pad, ds2::r2, (b & pad_r2) != 0, data.analog_buttons.r2);
    press(pad, ds2::select, (b & pad_touch_pad) != 0);
    press(pad, ds2::start, (b & pad_options) != 0);
    press(pad, ds2::l3, (b & pad_l3) != 0);
    press(pad, ds2::r3, (b & pad_r3) != 0);
    return Reading{pad, "DualSense (PS5)"};
}

void GamepadDevice::set_vibration(bool small_on, std::uint8_t large) {
    auto& d = *impl_;
    std::lock_guard lock(d.mutex);
    if (d.handle < 0) return;
    const Ps5PadVibration v{large, std::uint8_t(small_on ? 0xff : 0)};
    scePadSetVibration(d.handle, &v);
}

}  // namespace omega