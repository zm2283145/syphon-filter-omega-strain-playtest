#pragma once
// Port-only (checkpoint 132win12, not the original): simulated controllers at the device level, for hidden tests of
// the controller hot-plug (--test-pad-devices / --frontend-test-pad-devices, tests/gamepad_simulation_tests.cpp).
//
// pad_connection.hpp's SimulatedPads (132win6) replaces GamepadDevice::read()'s result by a scripted source name.
// This goes one layer lower: a GamepadDevice made with a GamepadSimulation runs its own scan thread, reader threads,
// report parsing, active-pad choice, vibration writes and loss handling, but on simulated devices:
//   * a Sony pad (DualShock 3 / DualShock 4 / DualSense, USB or Bluetooth; "vigem" is a ViGEm virtual DualShock 4)
//     is a duplex message-mode named pipe \\.\pipe\omega_simpad_<pid>_<alias> standing for its HID device path. The
//     simulation writes the HID input reports a pad of that kind sends (sony_report_from_ds2, parsed back by
//     ds2_from_sony_report) and reads the output reports (the motors) the device writes; closing the pipe's server
//     end is the unplug (the reader's ReadFile fails as on a removed HID device). A reconnect reuses the alias's
//     path, as a pad re-plugged into the same port or re-paired over Bluetooth does;
//   * an XInput pad is a slot of a table that replaces XInputGetState / XInputSetState.
// Connecting a pad signals the device's scan at once (the real scan runs once a second), and XInput slots are
// polled every read (the real device looks for a new slot once a second because polling absent slots is slow).
// Each change waits until the device has seen it, so a hidden run's frame-by-frame log is deterministic.
#include "gamepad_reports.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega {

// The aliases of pad_connection.hpp: xinput0..xinput3, ds3, ds4, ds4bt, dualsense, dualsensebt, vigem.
struct SimulatedPadKind {
    bool xinput = false;
    int slot = 0;                    // XInput
    SonyPad kind = SonyPad::dualshock4;
    bool bluetooth = false;
    std::size_t input_length = 0;    // the HID collection's report lengths, report ID included
    std::size_t output_length = 0;
};
std::optional<SimulatedPadKind> simulated_pad_kind(std::string_view alias);

// The input report a Sony pad of `kind` sends for `pad` (report ID first, `length` bytes): DualShock 4 USB 0x01 /
// Bluetooth 0x11, DualSense USB 0x01 / Bluetooth 0x31, DualShock 3 0x01. The DualShock 4 / DualSense carry the
// pressures of L2 / R2 only (the other pressed buttons read back at 0xff); opposite D-pad directions cannot be sent
// on their hat (up wins over down, right over left).
std::vector<std::uint8_t> sony_report_from_ds2(SonyPad kind, bool bluetooth, std::size_t length, const Ds2Pad& pad,
                                               std::uint8_t counter = 0);
// The XINPUT_GAMEPAD that ds2_from_xinput reads back as `pad` (the triggers carry the L2 / R2 pressures).
XInputGamepad xinput_from_ds2(const Ds2Pad& pad);
// The motors (weak on, strong 0..255) in an output report of sony_vibration_report's layouts.
std::optional<std::pair<bool, std::uint8_t>> motors_from_sony_output(SonyPad kind, bool bluetooth,
                                                                     const std::vector<std::uint8_t>& report);
// "cross+start", "l2+up", "none": DualShock 2 button names joined by '+'. Throws on an unknown name.
std::uint16_t parse_ds2_buttons(std::string_view spec);

struct PadDeviceEvent {
    enum class Kind { connect, disconnect, buttons };
    std::int64_t frame = 0;
    Kind kind = Kind::connect;
    std::string alias;
    std::uint16_t held = 0;   // buttons
};
// "<frame>:+<alias>", "<frame>:-<alias>", "<frame>:<alias>=<buttons>" separated by commas, frames in increasing
// order. Throws on a malformed list or an unknown alias.
std::vector<PadDeviceEvent> parse_pad_device_events(std::string_view text);

class GamepadSimulation {
public:
    GamepadSimulation();
    ~GamepadSimulation();
    GamepadSimulation(const GamepadSimulation&) = delete;
    GamepadSimulation& operator=(const GamepadSimulation&) = delete;

    // Each waits (up to 5 s) until the GamepadDevice made with this simulation has seen the change; throws when the
    // alias is unknown, already connected / not connected, or the device does not follow.
    void connect(const std::string& alias);
    void disconnect(const std::string& alias);
    void set_pad(const std::string& alias, const Ds2Pad& pad);
    // One event; returns its log line.
    std::string apply(const PadDeviceEvent& event);
    // The motors last written to the pad (its output reports / XInputSetState); nullopt before any.
    std::optional<std::pair<bool, std::uint8_t>> vibration(const std::string& alias) const;
    std::size_t connected() const;

    // --- the GamepadDevice side (gamepad_device.cpp) ---
    struct SonyDevice {
        std::wstring path;
        std::string alias;
        SonyPad kind = SonyPad::dualshock4;
        bool bluetooth = false;
        std::size_t input_length = 0, output_length = 0;
    };
    std::vector<SonyDevice> sony_devices() const;
    void* rescan_event() const;  // a HANDLE set when a Sony pad connects
    void device_opened(const std::wstring& path);
    void device_report(const std::wstring& path);
    void device_lost(const std::wstring& path);
    // XInputGetState / XInputSetState over the table: 0 (ERROR_SUCCESS) or ERROR_DEVICE_NOT_CONNECTED.
    unsigned long xinput_get(unsigned long slot, XInputGamepad& pad, unsigned long& packet) const;
    unsigned long xinput_set(unsigned long slot, const XInputVibration& motors);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omega
