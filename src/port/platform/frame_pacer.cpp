#include "frame_pacer.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32) && !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace omega {

double FrameClock::wait_seconds(Clock::time_point now) const {
    const double elapsed = std::chrono::duration<double>(now - last_).count();
    return std::max(0.0, dt_ - accumulator_ - elapsed);
}

int FrameClock::advance(Clock::time_point now) {
    double elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    if (snap_ && std::fabs(elapsed - dt_) <= dt_ * snap_tolerance) {
        elapsed = dt_;
        ++snapped_;
    }
    accumulator_ += elapsed;
    const int steps = std::min(max_steps_, static_cast<int>(accumulator_ / dt_));
    accumulator_ -= steps * dt_;
    if (accumulator_ > 0.25) accumulator_ = 0;
    return steps;
}

int FrameClock::advance_cadence(Clock::time_point now, double band) {
    const double shown = elapsed(now);
    last_ = now;
    accumulator_ += shown - dt_;
    int steps = 1;
    if (accumulator_ < -band) {
        steps = 0;
        accumulator_ += dt_;
    }
    while (accumulator_ > band && steps < max_steps_) {
        ++steps;
        accumulator_ -= dt_;
    }
    if (accumulator_ > band + 0.25) accumulator_ = 0;
    if (steps == 1) ++snapped_;
    return steps;
}

bool frame_rate_locks_to_refresh(double refresh_hz, double rate_hz) {
    if (!(refresh_hz > 0.0) || !(rate_hz > 0.0)) return false;
    const double n = std::round(refresh_hz / rate_hz);
    if (n < 1.0 || n > 4.0) return false;
    return std::fabs(refresh_hz / n - rate_hz) <= rate_hz * FrameClock::snap_tolerance;
}

bool frame_rate_cadence_fits(double refresh_hz, double rate_hz) {
    if (!(refresh_hz > 0.0) || !(rate_hz > 0.0) || frame_rate_locks_to_refresh(refresh_hz, rate_hz)) return false;
    const double ratio = refresh_hz / rate_hz;
    return ratio > 1.0 && ratio < 4.0;
}

PresentCadence::PresentCadence(double refresh_hz, double rate_hz)
    : num_(std::max<std::int64_t>(1, std::llround(refresh_hz * 1000.0))),
      den_(std::max<std::int64_t>(1, std::llround(rate_hz * 1000.0))) {}

unsigned PresentCadence::next() {
    err_ += num_;
    const std::int64_t n = err_ / den_;
    err_ -= n * den_;
    return static_cast<unsigned>(std::clamp<std::int64_t>(n, 1, 4));
}

#ifdef _WIN32  // the Vita versions are in vita/frame_pacer_vita.cpp
namespace {

// The display path's rational refresh for a GDI device name (\\.\DISPLAY1), 0 when not found.
double display_config_refresh_hz(const wchar_t* device) {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 0.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) != ERROR_SUCCESS)
        return 0.0;
    for (UINT32 k = 0; k < paths; ++k) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = path[k].sourceInfo.adapterId;
        source.header.id = path[k].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, device) != 0)
            continue;
        const auto& rate = path[k].targetInfo.refreshRate;
        if (rate.Numerator > 0 && rate.Denominator > 0) return double(rate.Numerator) / double(rate.Denominator);
    }
    return 0.0;
}

}  // namespace

double display_refresh_hz(void* window) {
    HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
    // 0 and 1 mean the hardware's default rate (unknown here).
    if (mode.dmDisplayFrequency <= 1) return 0.0;
    // 132win13: the exact rate when the display path has it and it rounds to the whole one reported.
    const double exact = display_config_refresh_hz(info.szDevice);
    return exact > 0.0 && std::fabs(exact - double(mode.dmDisplayFrequency)) < 1.0 ? exact : double(mode.dmDisplayFrequency);
}

FramePacer::FramePacer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    high_resolution_ = timer_ != nullptr;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

FramePacer::~FramePacer() {
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
}

void FramePacer::sleep_for(double seconds) {
    if (!(seconds > 0.0)) return;
    if (timer_) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::ceil(seconds * 1e7));  // relative, 100 ns units
        if (SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(std::ceil(seconds * 1000.0)));
}

#endif

void FramePacer::sleep_until(std::chrono::steady_clock::time_point t) {
    sleep_for(std::chrono::duration<double>(t - std::chrono::steady_clock::now()).count());
}

void FrameTimes::mark(std::chrono::steady_clock::time_point t) {
    if (have_last_) intervals_ms_.push_back(std::chrono::duration<double, std::milli>(t - last_).count());
    last_ = t;
    have_last_ = true;
}

FrameTimeStats FrameTimes::stats(double target_ms) const {
    FrameTimeStats s;
    s.count = intervals_ms_.size();
    if (s.count == 0) return s;
    double sum = 0;
    s.min_ms = intervals_ms_.front();
    s.max_ms = intervals_ms_.front();
    for (const double v : intervals_ms_) {
        sum += v;
        s.min_ms = std::min(s.min_ms, v);
        s.max_ms = std::max(s.max_ms, v);
        s.max_jitter_ms = std::max(s.max_jitter_ms, std::fabs(v - target_ms));
        if (v > 1.5 * target_ms) ++s.late;
    }
    s.mean_ms = sum / double(s.count);
    double square = 0;
    for (const double v : intervals_ms_) square += (v - s.mean_ms) * (v - s.mean_ms);
    s.stddev_ms = std::sqrt(square / double(s.count));
    std::vector<double> sorted = intervals_ms_;
    std::sort(sorted.begin(), sorted.end());
    s.p99_ms = sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(std::ceil(0.99 * double(sorted.size()))) - 1)];
    return s;
}

std::string frame_time_summary(const FrameTimeStats& s, double target_ms) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << s.count << " intervals, target " << target_ms << " ms, mean "
        << s.mean_ms << " ms, stddev " << s.stddev_ms << " ms, min " << s.min_ms << " ms, max " << s.max_ms
        << " ms, p99 " << s.p99_ms << " ms, max jitter " << s.max_jitter_ms << " ms, " << s.late
        << " over 1.5 x target";
    return out.str();
}

void FrameTimes::write(const std::filesystem::path& file, double target_ms) const {
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write frame times " + file.string());
    out << "# frame interval ms (port-only frame pacing, frame_pacer.hpp); " << frame_time_summary(stats(target_ms), target_ms)
        << "\n";
    out << std::fixed << std::setprecision(4);
    for (const double v : intervals_ms_) out << v << "\n";
    if (!out) throw std::runtime_error("Cannot write frame times " + file.string());
}

#ifdef _WIN32
double process_cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    const auto seconds = [](const FILETIME& f) {
        return double((std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
    };
    return seconds(kernel) + seconds(user);
}
#endif

LoopPacing::LoopPacing(double dt, bool vsync, int max_steps)
    : clock_(dt, max_steps), vsync_(vsync), histogram_(static_cast<std::size_t>(max_steps) + 1, 0) {}

void LoopPacing::start(Clock::time_point now) {
    clock_.start(now);
    times_.reset();
    wall_start_ = now;
    cpu_start_ = process_cpu_seconds();
}

void LoopPacing::set_refresh(double hz) {
    const double rate = 1.0 / clock_.dt();
    const bool whole = vsync_ && frame_rate_locks_to_refresh(hz, rate);
    const bool cadence = vsync_ && cadence_allowed_ && frame_rate_cadence_fits(hz, rate);
    // 132win13: the same cadence again (the 2 s refresh re-read) keeps its place in the schedule.
    const PresentCadence schedule = cadence ? PresentCadence(hz, rate) : PresentCadence();
    const bool same = cadence && cadence_on_ && schedule == cadence_;
    refresh_hz_ = hz;
    if (same) return;
    const bool was_cadence = cadence_on_;
    locked_ = whole || cadence;
    cadence_on_ = cadence;
    if (cadence) {
        cadence_ = schedule;
        interval_ = cadence_.next();
    } else {
        interval_ = whole ? static_cast<unsigned>(std::lround(hz / rate)) : 1u;
    }
    clock_.set_snap(whole);
    if (was_cadence != cadence) clock_.reset_accumulator();  // the signed cadence error is not a step backlog
}

unsigned LoopPacing::sync_interval() const { return vsync_ ? interval_ : 0u; }

void LoopPacing::sleep(double seconds, Clock::time_point& now) {
    const auto due = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    pacer_.sleep_for(seconds);
    now = Clock::now();
    slept_ += seconds;
    const double late = std::max(0.0, std::chrono::duration<double, std::milli>(now - due).count());
    ++sleeps_;
    wake_late_sum_ms_ += late;
    wake_late_max_ms_ = std::max(wake_late_max_ms_, late);
}

int LoopPacing::next_frame() {
    auto now = Clock::now();
    return next_frame(now, [this](double seconds, Clock::time_point& t) { this->sleep(seconds, t); });
}

int LoopPacing::next_frame(Clock::time_point& now, const std::function<void(double, Clock::time_point&)>& sleep) {
    const double dt = clock_.dt();
    int steps = 0;
    if (cadence_on_) {
        // 132win13: the present blocks on the vertical blanks; a frame returned more than half a refresh before
        // the cadence's shortest interval (a present that did not block) waits the rest on the timer.
        const double refresh = 1.0 / refresh_hz_;
        const double shortest = std::floor(refresh_hz_ * dt) * refresh;
        const double wait = shortest - clock_.elapsed(now);
        if (wait > 0.5 * refresh) sleep(wait, now);
        // The error band: one refresh (any run of the schedule) plus a step's correction must fit inside it.
        steps = clock_.advance_cadence(now, (0.5 * (refresh_hz_ * dt + 1.0) + 0.2) * refresh);
        interval_ = cadence_.next();
    } else {
        const double wait = clock_.wait_seconds(now);
        if (wait > (locked_ ? dt * 0.25 : 0.0002)) sleep(wait, now);
        steps = clock_.advance(now);
        // Unlocked: a timer that woke a little early sleeps the rest (never a frame that steps nothing).
        for (int k = 0; steps == 0 && !locked_ && k < 8; ++k) {
            sleep(std::max(clock_.wait_seconds(now), 0.0002), now);
            steps = clock_.advance(now);
        }
    }
    ++histogram_[static_cast<std::size_t>(std::clamp(steps, 0, static_cast<int>(histogram_.size()) - 1))];
    if (steps > 0) times_.mark(now);
    return steps;
}

std::string LoopPacing::mode_text() const {
    if (cadence_on_) {
        std::ostringstream out;
        out << "cadence of present intervals " << std::fixed << std::setprecision(3) << refresh_hz_ * clock_.dt()
            << " refreshes per frame (";
        PresentCadence c = cadence_;
        for (int k = 0; k < 5; ++k) out << (k ? "," : "") << c.next();
        out << ",...)";
        return out.str();
    }
    return locked_ ? "locked, present interval " + std::to_string(interval_) : std::string("timer-paced");
}

std::string LoopPacing::summary() const {
    const double target = clock_.dt() * 1000.0;
    std::ostringstream out;
    out << frame_time_summary(times_.stats(target), target) << "; steps per loop";
    for (std::size_t k = 0; k < histogram_.size(); ++k) out << " " << k << ":" << histogram_[k];
    const double wall = std::chrono::duration<double>(Clock::now() - wall_start_).count();
    const double cpu = process_cpu_seconds() - cpu_start_;
    out << std::fixed << std::setprecision(2) << "; " << clock_.snapped() << " intervals snapped to dt; vsync "
        << (vsync_ ? "on" : "off") << ", refresh " << refresh_hz_ << " Hz, "
        << mode_text()
        << "; slept " << slept_ << " s in " << sleeps_ << " timer waits (woke " << std::setprecision(3)
        << wake_late_mean_ms() << " ms late on average, " << wake_late_max_ms_ << " ms at most)" << std::setprecision(2)
        << "; process CPU " << cpu << " s in " << wall << " s wall ("
        << (wall > 0 ? 100.0 * cpu / wall : 0.0) << " % of one core)";
    return out.str();
}

}  // namespace omega