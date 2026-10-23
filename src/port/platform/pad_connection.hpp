#pragma once
// Controller hot-plug and the controller-removed state (checkpoint 132win6).
//
// The original (research/FRONTEND.md "Checkpoint 132win6"): the pad driver 00369c50 asks libpad for the port's
// state (0026b4d0 / 0026b5a0) every frame and stores it at pad +1ac (pad 0: 0055a4ec); while the state is not
// "ready" it returns before the read, so the held mask +10c, the presses +108, the previous mask +118, the sticks
// and the pressures keep their last values. The canvas update's controller check 00465870 counts the frames in
// which 0055a4ec is 0 (no pad) or 3 (the constructor's 0036a160 "not yet seen") in 00587b20; at 3 it opens the
// "NoCtrlTitle" / "NoCtrlMessage" popup (002cae30) and pauses the world (00131470(G, 1)); when the pad is back it
// closes the popup (002cac80) and resumes (00131470(0)) unless the start menu is open. GuiRuntime::controller_check
// hosts 00465870; the hosts feed it pad_state() here.
//
// Port-only policy (no PS2 counterpart): the keyboard and mouse are a controller that is always plugged in. With
// no gamepad at the start the keyboard is the pad (state 6, no prompt). The removed state (state 0) starts when
// the gamepad in use is lost and no other gamepad is present, and ends when any gamepad connects or a bound key
// or mouse button is newly pressed (the keyboard takes over). PadConnection::prompt = false (the port setting
// controller_prompt=0) never enters it.
//
// SimulatedPads: a scripted device list for hidden tests (--test-pads / --frontend-test-pads), the events
// "<frame>:+<pad>", "<frame>:-<pad>" and "<frame>:key" separated by commas. <pad> is xinput0..xinput3, ds3,
// ds4, ds4bt, dualsense, dualsensebt or vigem (a ViGEm virtual DualShock 4, 054c:05c4 over USB). The active pad
// follows GamepadDevice::read(): a Sony HID pad before XInput (the most recently connected Sony pad), else the
// XInput slot in use while it stays connected, else the lowest connected slot.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

struct PortSettings;

// The port setting controller_prompt=0|1 (default 1: PadConnection's removed state is on); throws on another value.
bool controller_prompt_setting(const PortSettings& settings);

class PadConnection {
public:
    explicit PadConnection(bool prompt = true) : prompt_(prompt) {}
    // One frame: the active gamepad's name (empty: none) and whether a bound key or mouse button was newly
    // pressed. Returns the log line of a change.
    std::optional<std::string> update(const std::string& source, bool key_pressed);
    // Checkpoint 132win11: the Port Settings page changes controller_prompt during a session.
    void set_prompt(bool prompt) { prompt_ = prompt; }
    bool removed() const { return removed_; }
    // 0055a4ec as 00465870 reads it: 0 while removed, else 6 (the port's "ready").
    std::int32_t pad_state() const { return removed_ ? 0 : 6; }
    const std::string& source() const { return source_; }
    unsigned disconnects() const { return disconnects_; }
    unsigned connects() const { return connects_; }

private:
    bool prompt_;
    bool started_ = false;
    bool removed_ = false;
    std::string source_;
    unsigned disconnects_ = 0, connects_ = 0;
};

struct PadPresenceEvent {
    enum class Kind { connect, disconnect, key };
    std::uint64_t frame = 0;
    Kind kind = Kind::connect;
    std::string pad;   // the alias (connect / disconnect)
};

// Throws on a malformed list or an unknown pad alias.
std::vector<PadPresenceEvent> parse_pad_events(std::string_view text);
// The source name GamepadDevice gives the pad, tagged as simulated ("DualShock 4 (USB) [simulated vigem]").
std::string simulated_pad_source(const std::string& alias);

class SimulatedPads {
public:
    explicit SimulatedPads(std::vector<PadPresenceEvent> events) : events_(std::move(events)) {}
    // Applies the events up to `frame` (frames are visited in increasing order); returns the active pad's
    // source (empty: none) and whether a "key" event fell in this frame.
    struct Frame {
        std::string source;
        bool key = false;
        std::vector<std::string> log;   // "connected vigem", "disconnected xinput0"
    };
    Frame at(std::uint64_t frame);
    std::size_t present() const { return sony_.size() + xinput_.size(); }

private:
    std::vector<PadPresenceEvent> events_;
    std::size_t next_ = 0;
    std::vector<std::string> sony_;   // in connection order
    std::set<int> xinput_;
    int xinput_slot_ = -1;
};

}  // namespace omega
