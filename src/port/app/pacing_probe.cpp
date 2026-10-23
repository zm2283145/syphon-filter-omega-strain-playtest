#include "pacing_probe.hpp"

#include "frame_pacer.hpp"
#include "host_window.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace omega {

namespace {

using Clock = std::chrono::steady_clock;

// The display's vertical blanks from the DWM (DwmGetCompositionTimingInfo's last blank and refresh period,
// sampled after each present; IDXGIOutput::WaitForVBlank does not block on a hybrid-GPU laptop whose outputs
// belong to the other adapter): the grid the presents' frames land on.
class VBlankSamples {
public:
    VBlankSamples() { QueryPerformanceFrequency(&frequency_); }
    void sample() {
        DWM_TIMING_INFO t{};
        t.cbSize = sizeof t;
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &t)) || t.qpcVBlank == 0 || t.qpcRefreshPeriod == 0) return;
        if (!anchors_.empty() && anchors_.back().first == t.qpcVBlank) return;
        anchors_.push_back({t.qpcVBlank, t.qpcRefreshPeriod});
    }
    // The blanks from the first sample to `until`: each sampled blank, the period's grid between them.
    std::vector<Clock::time_point> blanks(Clock::time_point until) const {
        std::vector<Clock::time_point> out;
        for (std::size_t i = 0; i < anchors_.size(); ++i) {
            const auto [qpc, period] = anchors_[i];
            const std::uint64_t end = i + 1 < anchors_.size() ? anchors_[i + 1].first : ~std::uint64_t{0};
            for (std::uint64_t q = qpc; q + period / 2 < end; q += period) {
                const auto t = to_clock(q);
                if (t > until) return out;
                out.push_back(t);
            }
        }
        return out;
    }

private:
    Clock::time_point to_clock(std::uint64_t qpc) const {
        // MSVC's steady_clock is the performance counter in nanoseconds.
        const std::uint64_t f = static_cast<std::uint64_t>(frequency_.QuadPart);
        const std::uint64_t ns = qpc / f * 1000000000ull + qpc % f * 1000000000ull / f;
        return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns)));
    }
    LARGE_INTEGER frequency_{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> anchors_;
};

}  // namespace

int run_pacing_probe(const PacingProbeOptions& o) {
    if (!(o.rate_hz >= 10.0 && o.rate_hz <= 240.0) || o.frames < 60)
        throw std::runtime_error("--pacing-probe needs a rate of 10..240 Hz and at least 60 frames");
    HostWindowOptions w;
    w.title = std::wstring(game_window_title) + L" (pacing probe)";
    w.width = 320;
    w.height = 240;
    HostWindow window(w);
    window.allow_hotkeys(false);
    window.set_windowed_rect({-7000, -7000, -6680, -6760});  // off every monitor: never seen, never focused
    window.show(false);
    const double refresh = display_refresh_hz(window.handle());
    double period = refresh > 0 ? 1.0 / refresh : 0.0;
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof timing;
    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0)
        period = double(timing.qpcRefreshPeriod) / double(qpf.QuadPart);
    if (!(period > 0)) throw std::runtime_error("Pacing probe: the display's refresh is unknown");

    const double dt = 1.0 / o.rate_hz;
    LoopPacing pacing(dt, o.vsync);
    pacing.set_cadence_allowed(o.cadence);
    pacing.start(Clock::now());
    pacing.set_refresh(refresh);
    std::cout << "Pacing probe (port-only): rate " << std::fixed << std::setprecision(3) << o.rate_hz << " Hz ("
              << refresh * dt << " refreshes per step), display " << refresh << " Hz (DWM refresh period "
              << period * 1000.0 << " ms), " << o.frames << " frames, simulated work " << o.work_ms << " ms, vsync "
              << (o.vsync ? "on" : "off") << ", " << pacing.mode_text() << "; swap chain " << window.swap_description()
              << ", " << window.backend() << "\n";

    std::vector<Clock::time_point> submitted;  // each present's call
    std::vector<Clock::time_point> returned;   // each present's return
    std::vector<std::uint64_t> stepped;        // the steps' total at each present
    std::vector<unsigned> intervals;           // the sync interval each present used
    std::uint64_t total = 0, lost = 0, occluded = 0;
    std::uint32_t seed = 12345;
    FramePacer worker;
    VBlankSamples vblank;
    vblank.sample();
    const auto begin = Clock::now();
    const double cpu_begin = process_cpu_seconds();
    for (unsigned k = 0; k < o.frames; ++k) {
        total += static_cast<std::uint64_t>(std::max(0, pacing.next_frame()));
        if (o.work_ms > 0) {
            seed = seed * 1664525u + 1013904223u;
            worker.sleep_for(o.work_ms * (0.5 + 0.5 * double(seed >> 8) / double(1u << 24)) / 1000.0);
        }
        const float shade = float(total % 60) / 60.0F;
        const float colour[4] = {shade, 0.2F, 1.0F - shade, 1.0F};
        const gfx::Texture* target = window.back_buffer();
        gfx::CommandList list;
        list.set_target(target);
        list.clear_target({target, 0}, colour);
        window.gfx()->submit(list);
        const unsigned interval = pacing.sync_interval();
        submitted.push_back(Clock::now());
        const PresentResult r = window.present(interval);
        lost += r == PresentResult::device_lost;
        occluded += r == PresentResult::occluded;
        returned.push_back(Clock::now());
        vblank.sample();
        stepped.push_back(total);
        intervals.push_back(interval);
    }
    const double wall = std::chrono::duration<double>(returned.back() - begin).count();
    const double cpu = process_cpu_seconds() - cpu_begin;
    const auto blanks = vblank.blanks(returned.back() + std::chrono::milliseconds(200));
    if (blanks.size() < 2) throw std::runtime_error("Pacing probe: no vertical blanks from the DWM's timing");
    // Each present's frame on the blank grid: the first blank 0.25 ms or more after its call, and with a sync
    // interval n at least n blanks after the previous frame's (DXGI's sync interval; vsync off: the first blank,
    // a later present on the same blank replacing it).
    std::vector<std::size_t> shown_at(submitted.size());
    for (std::size_t k = 0, b = 0; k < submitted.size(); ++k) {
        while (b < blanks.size() && blanks[b] < submitted[k] + std::chrono::microseconds(250)) ++b;
        std::size_t at = b;
        if (k > 0 && intervals[k] > 0) at = std::max(at, shown_at[k - 1] + intervals[k]);
        shown_at[k] = std::min(at, blanks.size() - 1);
    }

    // From the 20th present on (the swap chain's queue full, the cadence settled).
    const std::size_t from = 20;
    std::map<long, std::uint64_t> shown_for, returns, steps;
    FrameTimes times;
    std::vector<double> shown, error;  // per frame from `from`: the blanks shown; the steps' time - the blank's (refreshes)
    std::vector<bool> stall;
    const std::size_t longest = static_cast<std::size_t>(std::ceil(dt / period - 0.02));
    for (std::size_t k = from; k < returned.size(); ++k) {
        const double ms = std::chrono::duration<double, std::milli>(returned[k] - returned[k - 1]).count();
        times.mark(returned[k - 1]);
        ++returns[std::lround(ms / (period * 1000.0))];
        ++steps[static_cast<long>(stepped[k] - stepped[k - 1])];
        const std::size_t blanks_shown = shown_at[k] - shown_at[k - 1];  // 0: replaced on the same blank
        ++shown_for[static_cast<long>(blanks_shown)];
        shown.push_back(double(blanks_shown));
        error.push_back((double(stepped[k]) * dt - std::chrono::duration<double>(blanks[shown_at[k]] - begin).count()) / period);
        // A miss (the machine's, not the schedule's; the same rule for every mode): a frame held longer than any
        // frame of the rate is (ceil(refreshes per step) blanks), or a catch-up frame.
        stall.push_back(blanks_shown > longest || stepped[k] - stepped[k - 1] > 2);
    }
    times.mark(returned.back());
    // Between misses (runs of 30 frames or more after the 8 frames of recovery): the judder (max - min of the steps' time against the shown
    // blank's, in refreshes; median and largest of the runs) and the balance (the largest |blanks of 10
    // consecutive frames - their steps' time in refreshes|).
    std::vector<double> judders;
    double balance = 0;
    std::size_t run_frames = 0, stalls = 0;
    for (std::size_t a = 0; a < shown.size();) {
        if (stall[a]) {
            ++stalls;
            ++a;
            continue;
        }
        std::size_t b = a;
        while (b < shown.size() && !stall[b]) ++b;
        // After a miss the swap chain's queue refills and the pacing re-locks (a re-present of 0 steps within a
        // few frames): its 8 frames are the recovery, not the cadence.
        const std::size_t first = a == 0 ? a : a + 8;
        if (b >= first + 30) {
            const auto [lo, hi] = std::minmax_element(error.begin() + first, error.begin() + b);
            judders.push_back(*hi - *lo);
            run_frames += b - first;
            for (std::size_t k = first; k + 10 <= b; ++k) {
                double sum = 0;
                for (std::size_t j = k; j < k + 10; ++j) sum += shown[j];
                const double stepped_refreshes = double(stepped[from + k + 9] - stepped[from + k - 1]) * dt / period;
                balance = std::max(balance, std::fabs(sum - stepped_refreshes));
            }
        }
        a = b;
    }
    std::sort(judders.begin(), judders.end());
    const double judder_median = judders.empty() ? 0.0 : judders[judders.size() / 2];
    const double judder_max = judders.empty() ? 0.0 : judders.back();
    const auto stats = times.stats(dt * 1000.0);
    const double steps_rate = double(stepped.back() - stepped[from - 1]) /
                              std::chrono::duration<double>(returned.back() - returned[from - 1]).count();
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << "Pacing probe (port-only): " << blanks.size() << " blanks; frames shown for";
    for (const auto& [n, count] : shown_for) out << " " << n << ":" << count;
    out << " (blanks: count; 0 = replaced); present returns every";
    for (const auto& [n, count] : returns) out << " " << n << ":" << count;
    out << " (refreshes: count); steps per present";
    for (const auto& [n, count] : steps) out << " " << n << ":" << count;
    out << "; steps " << steps_rate << " per second (rate " << o.rate_hz << "); " << stalls << " misses (a frame over " << longest
        << " blanks or a catch-up); between them (" << judders.size() << " runs, " << run_frames << " frames) judder "
        << judder_median << " refreshes median, " << judder_max << " largest; 10-frame balance " << balance
        << " refreshes; present returns " << frame_time_summary(stats, dt * 1000.0) << "; " << lost << " device lost, "
        << occluded << " occluded; process CPU " << std::setprecision(2) << cpu << " s in " << wall << " s ("
        << (wall > 0 ? 100.0 * cpu / wall : 0.0) << " % of one core)\n";
    std::cout << out.str();
    std::cout << "Pacing probe (port-only): sync intervals from frame " << from << ":";
    for (std::size_t k = from; k < std::min<std::size_t>(from + 15, intervals.size()); ++k) std::cout << " " << intervals[k];
    std::cout << "; " << pacing.summary() << "\n";
    if (!o.frame_times.empty()) {
        times.write(o.frame_times, dt * 1000.0);
        auto csv = o.frame_times;
        csv += ".frames.csv";
        std::ofstream f(csv);
        f << "frame,submit_ms,return_ms,blank_ms,blanks_shown,steps,sync_interval\n" << std::fixed << std::setprecision(3);
        const auto ms = [&](Clock::time_point t) { return std::chrono::duration<double, std::milli>(t - begin).count(); };
        for (std::size_t k = 0; k < submitted.size(); ++k)
            f << k << "," << ms(submitted[k]) << "," << ms(returned[k]) << "," << ms(blanks[shown_at[k]]) << ","
              << (k ? shown_at[k] - shown_at[k - 1] : 0) << "," << (k ? stepped[k] - stepped[k - 1] : stepped[0]) << ","
              << intervals[k] << "\n";
        std::cout << "Pacing probe (port-only): present intervals written to " << o.frame_times.string() << ", the frames to "
                  << csv.string() << "\n";
    }
    return 0;
}

}  // namespace omega
