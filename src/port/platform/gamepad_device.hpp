#pragma once
#include "gamepad_reports.hpp"

#include <memory>
#include <optional>
#include <string>

// Host controller input for the PC executables: a Sony controller over HID
// (DualShock 3 / DualShock 4 / DualSense, USB or Bluetooth, read on a
// background thread and reopened when it reconnects), else the first XInput
// pad. Either becomes the DualShock 2 report the game reads (gamepad_reports.hpp).
namespace omega {
class GamepadSimulation;
class GamepadDevice {
public:
    struct Reading {
        Ds2Pad pad;
        std::string source;   // e.g. "DualSense (Bluetooth)", "XInput pad 0"
    };
    // Checkpoint 132win12 (port-only tests): with a simulation (gamepad_simulation.hpp) the Sony HID pads and the
    // XInput slots are the simulation's; everything else runs as with real devices.
    explicit GamepadDevice(GamepadSimulation* simulation = nullptr);
    ~GamepadDevice();
    GamepadDevice(const GamepadDevice&)=delete;
    GamepadDevice& operator=(const GamepadDevice&)=delete;
    // The latest state; nullopt when no controller is connected.
    std::optional<Reading> read();
    // The DualShock 2 actuators (00369920) on the connected controller's motors (gamepad_reports.hpp):
    // the Sony pad's output report from the reader thread, else XInputSetState. A DualShock 3 or a pad
    // opened read-only does not vibrate.
    void set_vibration(bool small_on,std::uint8_t large);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace omega
