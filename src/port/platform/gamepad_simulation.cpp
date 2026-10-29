#include "gamepad_simulation.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace omega {
namespace {

using namespace ds2;

constexpr std::array<std::pair<const char*, std::uint16_t>, 16> button_names{{
    {"l2", l2}, {"r2", r2}, {"l1", l1}, {"r1", r1}, {"triangle", triangle}, {"circle", circle}, {"cross", cross},
    {"square", square}, {"select", select}, {"l3", l3}, {"r3", r3}, {"start", start}, {"up", up}, {"right", right},
    {"down", down}, {"left", left}}};

std::uint8_t pressure_of(const Ds2Pad& pad, std::size_t k) { return pad.down(pressure_mask[k]) ? pad.pressure[k] : 0; }

// The DualShock 4 / DualSense hat (0 = N, clockwise, 8 = released) and button bytes (gamepad_reports.cpp).
std::uint8_t hat_of(const Ds2Pad& p) {
    const bool u = p.down(up), d = p.down(down) && !u, r = p.down(right), l = p.down(left) && !r;
    if (u) return r ? 1 : l ? 7 : 0;
    if (d) return r ? 3 : l ? 5 : 4;
    if (r) return 2;
    if (l) return 6;
    return 8;
}
std::uint8_t b0_of(const Ds2Pad& p) {
    return static_cast<std::uint8_t>(hat_of(p) | (p.down(square) ? 0x10 : 0) | (p.down(cross) ? 0x20 : 0) |
                                     (p.down(circle) ? 0x40 : 0) | (p.down(triangle) ? 0x80 : 0));
}
std::uint8_t b1_of(const Ds2Pad& p) {
    return static_cast<std::uint8_t>((p.down(l1) ? 0x01 : 0) | (p.down(r1) ? 0x02 : 0) | (p.down(l2) ? 0x04 : 0) |
                                     (p.down(r2) ? 0x08 : 0) | (p.down(select) ? 0x10 : 0) | (p.down(start) ? 0x20 : 0) |
                                     (p.down(l3) ? 0x40 : 0) | (p.down(r3) ? 0x80 : 0));
}

#ifdef _WIN32
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
#endif

}  // namespace

std::optional<SimulatedPadKind> simulated_pad_kind(std::string_view alias) {
    SimulatedPadKind k;
    if (alias.size() == 7 && alias.substr(0, 6) == "xinput" && alias[6] >= '0' && alias[6] <= '3') {
        k.xinput = true;
        k.slot = alias[6] - '0';
        return k;
    }
    if (alias == "ds4" || alias == "vigem") k = {false, 0, SonyPad::dualshock4, false, 64, 32};
    else if (alias == "ds4bt") k = {false, 0, SonyPad::dualshock4, true, 547, 547};
    else if (alias == "dualsense") k = {false, 0, SonyPad::dualsense, false, 64, 48};
    else if (alias == "dualsensebt") k = {false, 0, SonyPad::dualsense, true, 78, 78};
    else if (alias == "ds3") k = {false, 0, SonyPad::dualshock3, false, 49, 49};
    else return std::nullopt;
    return k;
}

std::vector<std::uint8_t> sony_report_from_ds2(SonyPad kind, bool bluetooth, std::size_t length, const Ds2Pad& p,
                                               std::uint8_t counter) {
    std::vector<std::uint8_t> r(length, 0);
    const auto need = [&](std::size_t n) {
        if (length < n) throw std::runtime_error("sony_report_from_ds2: the report length is too short");
    };
    switch (kind) {
    case SonyPad::dualshock3: {
        need(26);
        r[0] = 0x01;
        r[2] = static_cast<std::uint8_t>(p.held >> 8);
        r[3] = static_cast<std::uint8_t>(p.held);
        r[6] = p.left_x, r[7] = p.left_y, r[8] = p.right_x, r[9] = p.right_y;
        static constexpr std::array<std::size_t, 12> at{15, 17, 14, 16, 22, 23, 24, 25, 20, 21, 18, 19};
        for (std::size_t k = 0; k < at.size(); ++k) r[at[k]] = pressure_of(p, k);
        return r;
    }
    case SonyPad::dualshock4: {
        const std::size_t base = bluetooth ? 3 : 1;
        need(base + 9);
        r[0] = bluetooth ? 0x11 : 0x01;
        r[base] = p.left_x, r[base + 1] = p.left_y, r[base + 2] = p.right_x, r[base + 3] = p.right_y;
        r[base + 4] = b0_of(p), r[base + 5] = b1_of(p), r[base + 6] = static_cast<std::uint8_t>(counter << 2);
        r[base + 7] = pressure_of(p, p_l2), r[base + 8] = pressure_of(p, p_r2);
        return r;
    }
    case SonyPad::dualsense: {
        const std::size_t base = bluetooth ? 2 : 1;
        need(base + 10);
        r[0] = bluetooth ? 0x31 : 0x01;
        r[base] = p.left_x, r[base + 1] = p.left_y, r[base + 2] = p.right_x, r[base + 3] = p.right_y;
        r[base + 4] = pressure_of(p, p_l2), r[base + 5] = pressure_of(p, p_r2), r[base + 6] = counter;
        r[base + 7] = b0_of(p), r[base + 8] = b1_of(p);
        return r;
    }
    }
    return r;
}

XInputGamepad xinput_from_ds2(const Ds2Pad& p) {
    XInputGamepad x;
    const auto bit = [&](std::uint16_t b, std::uint16_t v) { return p.down(b) ? v : std::uint16_t{0}; };
    x.buttons = static_cast<std::uint16_t>(bit(up, 0x0001) | bit(down, 0x0002) | bit(left, 0x0004) | bit(right, 0x0008) |
                                           bit(start, 0x0010) | bit(select, 0x0020) | bit(l3, 0x0040) | bit(r3, 0x0080) |
                                           bit(l1, 0x0100) | bit(r1, 0x0200) | bit(cross, 0x1000) | bit(circle, 0x2000) |
                                           bit(square, 0x4000) | bit(triangle, 0x8000));
    // ds2_from_xinput's threshold: a pressed L2 / R2 is above 30.
    x.left_trigger = p.down(l2) ? std::max<std::uint8_t>(pressure_of(p, p_l2), 31) : 0;
    x.right_trigger = p.down(r2) ? std::max<std::uint8_t>(pressure_of(p, p_r2), 31) : 0;
    const auto sx = [](std::uint8_t b) { return static_cast<std::int16_t>(b * 256 - 32768 + 128); };
    const auto sy = [](std::uint8_t b) { return static_cast<std::int16_t>(32767 - b * 256 - 128); };
    x.lx = sx(p.left_x), x.ly = sy(p.left_y), x.rx = sx(p.right_x), x.ry = sy(p.right_y);
    return x;
}

std::optional<std::pair<bool, std::uint8_t>> motors_from_sony_output(SonyPad kind, bool bluetooth,
                                                                     const std::vector<std::uint8_t>& r) {
    std::size_t weak = 0, strong = 0;
    if (kind == SonyPad::dualshock4 && !bluetooth && r.size() >= 6 && r[0] == 0x05 && (r[1] & 0x01))
        weak = 4, strong = 5;
    else if (kind == SonyPad::dualshock4 && bluetooth && r.size() >= 8 && r[0] == 0x11 && (r[1] & 0xc0) == 0xc0 &&
             (r[3] & 0x01))
        weak = 6, strong = 7;
    else if (kind == SonyPad::dualsense && !bluetooth && r.size() >= 5 && r[0] == 0x02 && (r[1] & 0x03))
        weak = 3, strong = 4;
    else if (kind == SonyPad::dualsense && bluetooth && r.size() >= 7 && r[0] == 0x31 && r[2] == 0x10 && (r[3] & 0x03))
        weak = 5, strong = 6;
    else if (kind == SonyPad::dualshock3 && r.size() >= 6 && r[0] == 0x01) weak = 3, strong = 5;
    else return std::nullopt;
    return std::pair{r[weak] != 0, r[strong]};
}

std::uint16_t parse_ds2_buttons(std::string_view spec) {
    if (spec == "none") return 0;
    std::uint16_t held = 0;
    std::size_t at = 0;
    while (at <= spec.size()) {
        const auto end = std::min(spec.find('+', at), spec.size());
        const auto name = spec.substr(at, end - at);
        const auto it = std::find_if(button_names.begin(), button_names.end(), [&](const auto& b) { return name == b.first; });
        if (it == button_names.end())
            throw std::runtime_error("Simulated pad devices: unknown button '" + std::string(name) +
                                     "' (l2 r2 l1 r1 triangle circle cross square select l3 r3 start up right down left, or none)");
        held = static_cast<std::uint16_t>(held | it->second);
        at = end + 1;
    }
    return held;
}

std::vector<PadDeviceEvent> parse_pad_device_events(std::string_view text) {
    std::vector<PadDeviceEvent> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = std::min(text.find(',', at), text.size());
        const auto item = text.substr(at, end - at);
        at = end + 1;
        const auto colon = item.find(':');
        const auto bad = [&] {
            return std::runtime_error("Simulated pad devices: malformed event '" + std::string(item) +
                                      "' (<frame>:+<pad>, <frame>:-<pad> or <frame>:<pad>=<buttons>)");
        };
        if (colon == std::string_view::npos || colon == 0) throw bad();
        PadDeviceEvent e;
        try {
            std::size_t used = 0;
            e.frame = std::stoll(std::string(item.substr(0, colon)), &used);
            if (used != colon || e.frame < 0) throw bad();
        } catch (const std::invalid_argument&) {
            throw bad();
        }
        auto rest = item.substr(colon + 1);
        if (rest.empty()) throw bad();
        if (rest[0] == '+' || rest[0] == '-') {
            e.kind = rest[0] == '+' ? PadDeviceEvent::Kind::connect : PadDeviceEvent::Kind::disconnect;
            e.alias = std::string(rest.substr(1));
        } else {
            const auto eq = rest.find('=');
            if (eq == std::string_view::npos) throw bad();
            e.kind = PadDeviceEvent::Kind::buttons;
            e.alias = std::string(rest.substr(0, eq));
            e.held = parse_ds2_buttons(rest.substr(eq + 1));
        }
        if (!simulated_pad_kind(e.alias))
            throw std::runtime_error("Simulated pad devices: unknown pad '" + e.alias +
                                     "' (xinput0..xinput3, ds3, ds4, ds4bt, dualsense, dualsensebt, vigem)");
        if (!out.empty() && e.frame < out.back().frame) throw std::runtime_error("Simulated pad devices: frames must not decrease");
        out.push_back(std::move(e));
    }
    return out;
}

// --- the simulation ---

#ifdef _WIN32
struct GamepadSimulation::Impl {
    struct Pad {
        std::string alias;
        SimulatedPadKind kind;
        std::wstring path;
        HANDLE pipe = INVALID_HANDLE_VALUE;   // the server end (the device's side opens the client end)
        HANDLE stop = nullptr;
        std::thread outputs;                  // reads the device's output reports
        Ds2Pad pad;
        std::uint8_t counter = 0;
        bool opened = false, lost = false;
        std::uint64_t reports_written = 0, reports_read = 0;
        std::optional<std::pair<bool, std::uint8_t>> motors;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, std::unique_ptr<Pad>> pads;   // connected
    HANDLE rescan = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    Pad* by_path(const std::wstring& path) {
        for (auto& [_, p] : pads)
            if (p->path == path) return p.get();
        return nullptr;
    }

    template <class Done> void wait(std::unique_lock<std::mutex>& lock, Done done, const std::string& what) {
        if (!changed.wait_for(lock, std::chrono::seconds(5), done))
            throw std::runtime_error("Simulated pad devices: the controller device did not see " + what + " within 5 s");
    }

    void read_outputs(Pad* p) {
        std::vector<std::uint8_t> buffer(std::max<std::size_t>(p->kind.output_length, 78) + 16);
        OVERLAPPED o{};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (;;) {
            ResetEvent(o.hEvent);
            DWORD n = 0;
            if (!ReadFile(p->pipe, buffer.data(), DWORD(buffer.size()), nullptr, &o) && GetLastError() != ERROR_IO_PENDING) {
                if (GetLastError() == ERROR_PIPE_LISTENING) {   // not opened yet
                    if (WaitForSingleObject(p->stop, 5) == WAIT_OBJECT_0) break;
                    continue;
                }
                break;
            }
            const HANDLE waits[2]{p->stop, o.hEvent};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                CancelIoEx(p->pipe, &o);
                GetOverlappedResult(p->pipe, &o, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(p->pipe, &o, &n, FALSE)) break;
            const std::vector<std::uint8_t> report(buffer.begin(), buffer.begin() + n);
            if (const auto m = motors_from_sony_output(p->kind.kind, p->kind.bluetooth, report)) {
                std::lock_guard lock(mutex);
                p->motors = m;
            }
        }
        CloseHandle(o.hEvent);
    }

    void close_pipe(Pad& p) {
        if (p.stop) SetEvent(p.stop);
        if (p.outputs.joinable()) p.outputs.join();
        if (p.pipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(p.pipe);
            CloseHandle(p.pipe);
            p.pipe = INVALID_HANDLE_VALUE;
        }
        if (p.stop) CloseHandle(p.stop);
        p.stop = nullptr;
    }

    void write_report(Pad& p) {
        const auto report = sony_report_from_ds2(p.kind.kind, p.kind.bluetooth, p.kind.input_length, p.pad, p.counter++);
        OVERLAPPED o{};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        if (!WriteFile(p.pipe, report.data(), DWORD(report.size()), nullptr, &o) && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(o.hEvent);
            throw std::runtime_error("Simulated pad devices: writing " + p.alias + "'s input report failed");
        }
        const bool done = WaitForSingleObject(o.hEvent, 5000) == WAIT_OBJECT_0 && GetOverlappedResult(p.pipe, &o, &n, FALSE);
        if (!done) CancelIoEx(p.pipe, &o), GetOverlappedResult(p.pipe, &o, &n, TRUE);
        CloseHandle(o.hEvent);
        if (!done) throw std::runtime_error("Simulated pad devices: " + p.alias + "'s input report was not read");
        ++p.reports_written;
    }
};

GamepadSimulation::GamepadSimulation() : impl_(std::make_unique<Impl>()) {}

GamepadSimulation::~GamepadSimulation() {
    for (auto& [_, p] : impl_->pads) impl_->close_pipe(*p);
    CloseHandle(impl_->rescan);
}

void GamepadSimulation::connect(const std::string& alias) {
    const auto kind = simulated_pad_kind(alias);
    if (!kind) throw std::runtime_error("Simulated pad devices: unknown pad '" + alias + "'");
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    if (d.pads.count(alias)) throw std::runtime_error("Simulated pad devices: " + alias + " is already connected");
    auto p = std::make_unique<Impl::Pad>();
    p->alias = alias;
    p->kind = *kind;
    if (kind->xinput) {
        p->opened = true;   // XInputGetState reports it from now on
        d.pads[alias] = std::move(p);
        return;
    }
    // The alias's fixed path: a re-plugged pad comes back at the same device path.
    p->path = L"\\\\.\\pipe\\omega_simpad_" + std::to_wstring(GetCurrentProcessId()) + L"_" + widen(alias);
    p->pipe = CreateNamedPipeW(p->path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    if (p->pipe == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Simulated pad devices: CreateNamedPipe failed for " + alias + " (" +
                                 std::to_string(GetLastError()) + ")");
    p->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Impl::Pad* raw = p.get();
    d.pads[alias] = std::move(p);
    raw->outputs = std::thread([&d, raw] { d.read_outputs(raw); });
    SetEvent(d.rescan);
    d.wait(lock, [raw] { return raw->opened; }, alias + "'s connection");
}

void GamepadSimulation::disconnect(const std::string& alias) {
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto it = d.pads.find(alias);
    if (it == d.pads.end()) throw std::runtime_error("Simulated pad devices: " + alias + " is not connected");
    auto p = std::move(it->second);
    d.pads.erase(it);
    if (p->kind.xinput) return;
    // Unplugged: the pipe's server end closes, the device's pending read fails. The pad is kept (under a "#" key,
    // not listed as a device) until the device reports the loss.
    Impl::Pad* raw = p.get();
    const std::string key = alias + "#unplugged";
    d.pads[key] = std::move(p);
    lock.unlock();
    d.close_pipe(*raw);
    lock.lock();
    d.wait(lock, [raw] { return raw->lost; }, alias + "'s removal");
    d.pads.erase(key);
}

void GamepadSimulation::set_pad(const std::string& alias, const Ds2Pad& pad) {
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto it = d.pads.find(alias);
    if (it == d.pads.end()) throw std::runtime_error("Simulated pad devices: " + alias + " is not connected");
    auto& p = *it->second;
    p.pad = pad;
    if (p.kind.xinput) return;
    const auto target = p.reports_written + 1;
    lock.unlock();
    d.write_report(p);
    lock.lock();
    d.wait(lock, [&p, target] { return p.reports_read >= target; }, alias + "'s input report");
}

std::string GamepadSimulation::apply(const PadDeviceEvent& e) {
    std::ostringstream line;
    switch (e.kind) {
    case PadDeviceEvent::Kind::connect:
        connect(e.alias);
        line << "connected " << e.alias;
        break;
    case PadDeviceEvent::Kind::disconnect:
        disconnect(e.alias);
        line << "disconnected " << e.alias;
        break;
    case PadDeviceEvent::Kind::buttons: {
        Ds2Pad pad;
        pad.held = e.held;
        for (std::size_t k = 0; k < pressure_mask.size(); ++k)
            if (pad.down(pressure_mask[k])) pad.pressure[k] = 0xff;
        set_pad(e.alias, pad);
        char held[8];
        std::snprintf(held, sizeof held, "%04x", unsigned(e.held));
        line << e.alias << " held 0x" << held;
        break;
    }
    }
    return line.str();
}

std::optional<std::pair<bool, std::uint8_t>> GamepadSimulation::vibration(const std::string& alias) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->pads.find(alias);
    if (it == impl_->pads.end()) return std::nullopt;
    return it->second->motors;
}

std::size_t GamepadSimulation::connected() const {
    std::lock_guard lock(impl_->mutex);
    return std::count_if(impl_->pads.begin(), impl_->pads.end(),
                         [](const auto& p) { return p.first.find('#') == std::string::npos; });
}

std::vector<GamepadSimulation::SonyDevice> GamepadSimulation::sony_devices() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<SonyDevice> out;
    for (const auto& [alias, p] : impl_->pads)
        if (!p->kind.xinput && alias.find('#') == std::string::npos)
            out.push_back({p->path, alias, p->kind.kind, p->kind.bluetooth, p->kind.input_length, p->kind.output_length});
    return out;
}

void* GamepadSimulation::rescan_event() const { return impl_->rescan; }

void GamepadSimulation::device_opened(const std::wstring& path) {
    std::lock_guard lock(impl_->mutex);
    if (auto* p = impl_->by_path(path)) p->opened = true;
    impl_->changed.notify_all();
}

void GamepadSimulation::device_report(const std::wstring& path) {
    std::lock_guard lock(impl_->mutex);
    if (auto* p = impl_->by_path(path)) ++p->reports_read;
    impl_->changed.notify_all();
}

void GamepadSimulation::device_lost(const std::wstring& path) {
    std::lock_guard lock(impl_->mutex);
    for (auto& [alias, p] : impl_->pads)
        if (p->path == path && alias.find('#') != std::string::npos) p->lost = true;
    impl_->changed.notify_all();
}

unsigned long GamepadSimulation::xinput_get(unsigned long slot, XInputGamepad& pad, unsigned long& packet) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto& [_, p] : impl_->pads)
        if (p->kind.xinput && p->kind.slot == int(slot)) {
            pad = xinput_from_ds2(p->pad);
            packet = static_cast<unsigned long>(p->reports_written);
            return ERROR_SUCCESS;
        }
    return ERROR_DEVICE_NOT_CONNECTED;
}

unsigned long GamepadSimulation::xinput_set(unsigned long slot, const XInputVibration& motors) {
    std::lock_guard lock(impl_->mutex);
    for (auto& [_, p] : impl_->pads)
        if (p->kind.xinput && p->kind.slot == int(slot)) {
            p->motors = std::pair{motors.right != 0, static_cast<std::uint8_t>(motors.left / 257)};
            return ERROR_SUCCESS;
        }
    return ERROR_DEVICE_NOT_CONNECTED;
}

#else
// Port-only (132vt2): the simulated devices are Windows named pipes and an XInput table (the hidden hot-plug tests);
// the PS Vita build has neither, so --test-pad-devices / --frontend-test-pad-devices stop here with this error.
struct GamepadSimulation::Impl {};
namespace {
[[noreturn]] void no_simulation() {
    throw std::runtime_error("Simulated pad devices (--test-pad-devices, gamepad_simulation.hpp) are Windows-only");
}
}  // namespace
GamepadSimulation::GamepadSimulation() { no_simulation(); }
GamepadSimulation::~GamepadSimulation() = default;
void GamepadSimulation::connect(const std::string&) { no_simulation(); }
void GamepadSimulation::disconnect(const std::string&) { no_simulation(); }
void GamepadSimulation::set_pad(const std::string&, const Ds2Pad&) { no_simulation(); }
std::string GamepadSimulation::apply(const PadDeviceEvent&) { no_simulation(); }
std::optional<std::pair<bool, std::uint8_t>> GamepadSimulation::vibration(const std::string&) const { no_simulation(); }
std::size_t GamepadSimulation::connected() const { no_simulation(); }
std::vector<GamepadSimulation::SonyDevice> GamepadSimulation::sony_devices() const { no_simulation(); }
void* GamepadSimulation::rescan_event() const { no_simulation(); }
void GamepadSimulation::device_opened(const std::wstring&) { no_simulation(); }
void GamepadSimulation::device_report(const std::wstring&) { no_simulation(); }
void GamepadSimulation::device_lost(const std::wstring&) { no_simulation(); }
unsigned long GamepadSimulation::xinput_get(unsigned long, XInputGamepad&, unsigned long&) const { no_simulation(); }
unsigned long GamepadSimulation::xinput_set(unsigned long, const XInputVibration&) { no_simulation(); }
#endif  // _WIN32

}  // namespace omega
