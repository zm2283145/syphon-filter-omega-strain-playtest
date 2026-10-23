#pragma once
// Port-only (checkpoint 132win3; backend-neutral part split out in 132mt1): the session's device-lost state. A
// backend call that sees the device removed (D3D11: device_lost.hpp's check_device) marks the loss and throws
// DeviceLost, which each renderer's public entry point catches (a function-try-block): that call's GPU work is
// dropped, nothing else is. While the mark is pending every renderer skips its draws; the owner of the frame (the
// play loop, the front end) sees device_lost_pending(), re-creates the device (HostWindow::recover_device, which
// clears the mark) and resets its renderers. No game state is touched.
//
// device_lost_inject is the test hook (--test-device-lost-mid, host_window_tests): the n-th checked backend call
// from then on reports the device removed.
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace omega {

class DeviceLost : public std::runtime_error {
public:
    DeviceLost(long code, const std::string& what) : std::runtime_error(what), result(code) {}
    long result;  // the backend's code (D3D11: the HRESULT)
};

inline std::atomic<bool> device_lost_flag{false};
inline std::atomic<std::uint64_t> device_lost_events{0};
inline std::atomic<const char*> device_lost_operation{nullptr};  // the call that saw the last loss (a literal)
inline std::atomic<std::int64_t> device_lost_injection{-1};      // checked calls left before an injected loss
inline std::atomic<const char*> device_lost_injection_label{nullptr};  // the hook's own label (a literal)
inline std::atomic<const char*> device_lost_forced_label{nullptr};     // set when the pending loss is the hook's

// The test hook: the call `after` checked calls from now (0: the next) reports the device removed.
// Checkpoint 132win4: `label` (a literal, e.g. "--test-device-lost-mid") names the forced loss, so the owner
// reports it apart from a real one (device_lost_forced) while device_lost_by still names the call it hit.
inline void device_lost_inject(std::int64_t after, const char* label = "device_lost_inject") {
    device_lost_injection_label.store(label);
    device_lost_injection.store(after);
}

// Marks the device lost by `operation` (a string literal).
inline void device_lost_mark(const char* operation) {
    if (!device_lost_flag.exchange(true)) {
        device_lost_operation.store(operation);
        device_lost_events.fetch_add(1);
    }
}
inline bool device_lost_pending() { return device_lost_flag.load(); }
inline const char* device_lost_by() {
    const char* op = device_lost_operation.load();
    return op ? op : "?";
}
// The pending loss's test-hook label (device_lost_inject), or nullptr for a real loss.
inline const char* device_lost_forced() { return device_lost_flag.load() ? device_lost_forced_label.load() : nullptr; }
// The device was re-created (HostWindow::recover_device).
inline void device_lost_recovered() {
    device_lost_forced_label.store(nullptr);
    device_lost_flag.store(false);
}
// One checked call of the test hook: true when the injected loss fires on it (the backend then reports the device
// removed for that call).
inline bool device_lost_injected_now() {
    std::int64_t left = device_lost_injection.load();
    while (left >= 0 && !device_lost_injection.compare_exchange_weak(left, left - 1)) {
    }
    if (left == 0) {
        if (!device_lost_flag.load()) device_lost_forced_label.store(device_lost_injection_label.load());
        return true;
    }
    return false;
}

}  // namespace omega
