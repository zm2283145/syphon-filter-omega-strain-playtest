#pragma once
// Port-only (checkpoint 132win3, not the original): frame pacing of the interactive loops (the play, the front
// end, the loading screen). The original paces on the PS2's vertical blank: the video frame begin 0037a9f0
// waits on GS CSR VSINT while the frame rate it measures (576000 / T1 counts, +1e8 = 005383d8) is above the
// limiter +1e4 (60 in the front end, 002cc3e0; app +260 in a level, 30 in the savestate), so a frame is never
// shorter than 1 / limiter. The port keeps the game's fixed step (the play's dt, 1/60 by default; the front
// end's 1/60) and only decides when the host runs a loop:
//
// - FrameClock: the step accumulator the loops already used (steps = elapsed / dt, at most 4 per loop, a
//   backlog over 0.25 s dropped). wait_seconds() is the time until the next step is due; the loop sleeps that
//   long instead of drawing frames that step nothing (a spin at the present rate, or a busy loop when the
//   present does not block). With snap on (vsync on a display whose refresh is a whole multiple of the game
//   rate, within 2 %), an interval within 2 % of dt counts as exactly dt, so a 59.94 Hz display steps once
//   per refresh (as the PS2's NTSC field clock does) instead of twice every ~17 s.
// - FramePacer: sleeps on a high-resolution waitable timer (no spin); a plain Sleep where that is missing.
// - FrameTimes: the loop's frame intervals (diagnostic: --frame-times, the pacing summary line).
// - LoopPacing: the policy. Locked (vsync on, the refresh a whole multiple n of the rate): the frame is presented
//   with sync interval n, so the present's wait on the vertical blank holds the rate (as the PS2's VSINT wait
//   does) and the host sleeps only when the present did not block (the swap chain's queue not yet full, an
//   occluded or invisible window). Otherwise (vsync off, a refresh under the rate or over 4 times it, or a cadence
//   display under the diagnostic --frame-cadence off): the host sleeps on the timer
//   until the next step is due, then presents with sync interval 1 (vsync on) or 0 (off).
// - Checkpoint 132win13: cadence (vsync on, the refresh 1..4 times the rate but not a whole multiple: 75 / 100 /
//   144 / 165 Hz for 60). The presents' sync intervals follow a Bresenham schedule of the refresh over the rate
//   (144 / 60 = 2.4: 2, 2, 3, 2, 3, ...; the precise refresh, display_refresh_hz), so every game frame is shown
//   for the whole refreshes that average exactly 1 / rate, and the loop steps once per presented frame. The
//   time shown beyond one step per frame is kept (FrameClock::advance_cadence): any run of the schedule's
//   intervals is within one refresh of the steps' time, so it stays inside a band of +-((ratio + 1) / 2 + 0.2)
//   refreshes (wide enough that a correction lands inside it: a narrower band oscillates 0 / 2 steps) and the
//   loop steps exactly once per frame; missed vertical blanks push it out and the next frame steps twice (a
//   frame presented early: none), so the game's rate is held to the wall clock. Before 132win13 these displays
//   were timer-paced, presenting at interval 1 on whatever vertical blank followed the timer (an irregular 2/3
//   refresh pattern with the timer's jitter, 0 / 2-step frames when a wake crossed a refresh).
//
// The original's own rate (research/FRONTEND.md "Checkpoint 132win13"): Game_LoadIni 00165630 sets the level's
// limiter app +260 = 30 (NTSC and PROSCAN; 50 for PAL), 0037a9f0 ends each frame on a vertical blank (GS CSR
// VSINT) once 576000 / T1 is not above it, so a level frame lasts 2 NTSC fields (1001 / 30000 s) and the world dt
// 00133340 (T1 / 5.76 ten-microsecond ticks, clamped to 0..0.05 s) reads 0.033367; the front end's limiter is 60
// (002cc3e0; 50 PAL): one frame per field. The port's play steps a fixed 1/60 by default (twice the original's
// steps at half its dt; --dt 0.033367 or --dt-trace replays the original's), which these loops only pace.
//
// Nothing here changes a step: each runs with the same dt, input and state as before; only the host's wait
// between loops and the present's sync interval change. Scripted (--script) hidden runs are not paced unless
// asked (--frame-pacing on; their steps stay 1 per loop).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace omega {

class FrameClock {
public:
    using Clock = std::chrono::steady_clock;
    explicit FrameClock(double dt, int max_steps = 4) : dt_(dt), max_steps_(max_steps) {}
    void start(Clock::time_point now) { last_ = now; accumulator_ = 0.0; }
    // Seconds until the next step is due (0 when it is).
    double wait_seconds(Clock::time_point now) const;
    // The steps due at `now` (the accumulator moves on).
    int advance(Clock::time_point now);
    // Checkpoint 132win13 (cadence): one step per call while the time shown beyond one step per frame (the
    // accumulator, signed here) stays within +-band; past +band the frame steps more (at most max_steps; a backlog
    // over 0.25 s past the band dropped), past -band none.
    int advance_cadence(Clock::time_point now, double band);
    // Seconds since the last advance.
    double elapsed(Clock::time_point now) const { return std::chrono::duration<double>(now - last_).count(); }
    void set_snap(bool on) { snap_ = on; }
    void reset_accumulator() { accumulator_ = 0.0; }
    bool snap() const { return snap_; }
    double dt() const { return dt_; }
    double accumulator() const { return accumulator_; }
    std::uint64_t snapped() const { return snapped_; }

    static constexpr double snap_tolerance = 0.02;  // of dt

private:
    double dt_;
    int max_steps_;
    Clock::time_point last_{};
    double accumulator_ = 0.0;
    bool snap_ = false;
    std::uint64_t snapped_ = 0;
};

// Whether presenting on each vertical blank of a `refresh_hz` display holds `rate_hz` steadily: the refresh is
// a whole multiple 1..4 of the rate within 2 % (59.94 / 60 / 120 Hz for 60; 60 Hz for 30).
bool frame_rate_locks_to_refresh(double refresh_hz, double rate_hz);
// Checkpoint 132win13: whether the rate is held by a cadence of sync intervals instead (vsync on, the refresh
// over the rate between 1 and 4 and not a whole multiple: 75 / 100 / 144 / 165 Hz for 60).
bool frame_rate_cadence_fits(double refresh_hz, double rate_hz);
// The Bresenham schedule of sync intervals for a refresh over a rate (both to 1/1000 Hz): each interval is
// floor or ceil of the ratio, and any run of m intervals sums to floor or ceil of m x ratio.
class PresentCadence {
public:
    PresentCadence() = default;
    PresentCadence(double refresh_hz, double rate_hz);
    unsigned next();
    bool operator==(const PresentCadence& o) const { return num_ == o.num_ && den_ == o.den_; }

private:
    std::int64_t num_ = 1, den_ = 1, err_ = 0;
};
// The refresh rate of the monitor the window is on (0 when unknown). Checkpoint 132win13: the display path's
// exact rational rate (QueryDisplayConfig: 59.94, 143.856, ...) when Windows reports it, else the whole hertz of
// EnumDisplaySettings.
double display_refresh_hz(void* window);  // an HWND

class FramePacer {
public:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;
    void sleep_for(double seconds);
    void sleep_until(std::chrono::steady_clock::time_point t);
    bool high_resolution() const { return high_resolution_; }

private:
    void* timer_ = nullptr;  // a HANDLE
    bool high_resolution_ = false;
};

struct FrameTimeStats {
    std::size_t count = 0;  // intervals
    double mean_ms = 0, stddev_ms = 0, min_ms = 0, max_ms = 0, p99_ms = 0;
    double max_jitter_ms = 0;  // the largest |interval - target|
    std::size_t late = 0;      // intervals over 1.5 x target (a missed frame)
};

class FrameTimes {
public:
    void mark(std::chrono::steady_clock::time_point t);
    void reset() { have_last_ = false; intervals_ms_.clear(); }
    const std::vector<double>& intervals_ms() const { return intervals_ms_; }
    FrameTimeStats stats(double target_ms) const;
    void write(const std::filesystem::path& file, double target_ms) const;

private:
    bool have_last_ = false;
    std::chrono::steady_clock::time_point last_{};
    std::vector<double> intervals_ms_;
};

std::string frame_time_summary(const FrameTimeStats& s, double target_ms);

class LoopPacing {
public:
    using Clock = std::chrono::steady_clock;
    LoopPacing(double dt, bool vsync, int max_steps = 4);
    void start(Clock::time_point now);
    // The window's display refresh (0: unknown or nothing presented, never locked).
    void set_refresh(double hz);
    bool vsync() const { return vsync_; }
    // Checkpoint 132win4: the VSYNC options row turns vsync on or off in a running loop (the lock is re-decided
    // for the current refresh; the steps are unchanged).
    void set_vsync(bool on) {
        vsync_ = on;
        set_refresh(refresh_hz_);
    }
    bool locked() const { return locked_; }
    // Checkpoint 132win13: locked by a cadence of sync intervals (not a whole multiple).
    bool cadence() const { return cadence_on_; }
    // Port-only diagnostic (132win13, --frame-cadence off and the comparison test): off keeps the 132win3 rule
    // (such displays timer-paced at interval 1).
    void set_cadence_allowed(bool on) {
        cadence_allowed_ = on;
        set_refresh(refresh_hz_);
    }
    double refresh_hz() const { return refresh_hz_; }
    // The present's sync interval: 0 with vsync off, n when locked (the cadence's current one), else 1.
    unsigned sync_interval() const;
    // Waits (on the timer) until a step is due and returns the steps due (0 only when locked: the present
    // returned early within a quarter step and the frame is presented again).
    int next_frame();
    // The same with the caller's clock: `now` is the loop's time, `sleep(seconds, now)` waits and moves it on
    // (the unit tests' simulated display; next_frame() passes the steady clock and the timer).
    int next_frame(Clock::time_point& now, const std::function<void(double, Clock::time_point&)>& sleep);
    const FrameClock& clock() const { return clock_; }
    const FrameTimes& times() const { return times_; }
    double slept_seconds() const { return slept_; }
    // The timer's accuracy: how late the sleeps woke after their due time (ms; mean and largest).
    double wake_late_mean_ms() const { return sleeps_ ? wake_late_sum_ms_ / double(sleeps_) : 0.0; }
    double wake_late_max_ms() const { return wake_late_max_ms_; }
    // The steps-per-loop histogram (0..max) and the summary line (frame intervals, CPU against wall time).
    const std::vector<std::uint64_t>& steps_histogram() const { return histogram_; }
    std::string summary() const;
    // "locked, present interval n", "cadence of present intervals 2.400 refreshes per frame (2,3,2,2,3,...)" or
    // "timer-paced" (132win13: the log lines).
    std::string mode_text() const;
    void write_times(const std::filesystem::path& file) const { times_.write(file, clock_.dt() * 1000.0); }

private:
    FrameClock clock_;
    FramePacer pacer_;
    FrameTimes times_;
    bool vsync_;
    bool locked_ = false;
    bool cadence_on_ = false, cadence_allowed_ = true;
    PresentCadence cadence_;
    double refresh_hz_ = 0.0;
    unsigned interval_ = 1;
    double slept_ = 0.0;
    std::uint64_t sleeps_ = 0;
    double wake_late_sum_ms_ = 0.0, wake_late_max_ms_ = 0.0;
    void sleep(double seconds, Clock::time_point& now);
    std::vector<std::uint64_t> histogram_;
    Clock::time_point wall_start_{};
    double cpu_start_ = 0.0;
};
// The process's user + kernel CPU seconds so far.
double process_cpu_seconds();

}  // namespace omega
