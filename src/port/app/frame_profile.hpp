#pragma once
// Port-only (checkpoint 132win9, not the original): --profile <file>, the play loop's per-frame CPU split. Each loop
// (one game step in a scripted run) is timed in four phases on the host clock:
//   sim      the game steps (the frame's world step, minus the script part)
//   script   the mission scripts' event pump (MissionScriptHost::pump) and the frame's message deliveries to them
//   render   the render pass's submission (render_begin .. present_begin: uploads, draws, the GUI overlay, captures)
//   present  the swap chain's Present (a hidden run presents at sync interval 0 with --profile so that it is measured)
// Nothing here is read by the game; the rows go to a CSV (frame, QPC at the loop's start / end, the four phases in ms)
// and a summary line (mean / p50 / p95 / p99 / max per phase). The QPC columns let tools/profile/omega_sampler.cpp
// restrict its stack samples to a frame range.
// Checkpoint 132win10: the frame-time overlay (frame_overlay.hpp) reads the rolling averages (averages(): the
// per-frame means over the last window of at least half a second); a profile made for the overlay alone keeps no
// rows (keep_rows false), so a long session does not grow.

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace omega {

class FrameProfile {
public:
    enum Phase : int { sim, script, render, present, phase_count };
    static constexpr std::array<const char*, phase_count> names{"sim", "script", "render", "present"};

    struct Row {
        std::uint64_t frame = 0;
        std::int64_t qpc_begin = 0, qpc_end = 0;
        std::array<double, phase_count> ms{};
    };
    struct Stats {
        double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    };

    struct Averages {
        std::array<double, phase_count> ms{};
        double total = 0, max_total = 0, fps = 0;
        unsigned frames = 0;
    };

    explicit FrameProfile(bool keep_rows = true) : keep_rows_(keep_rows) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency_ = f.QuadPart;
    }

    static std::int64_t qpc() {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    void begin_frame() {
        current_ = Row{};
        current_.qpc_begin = qpc();
    }
    // The steps' total: sim is stored as this minus the script time added during the steps.
    void add(Phase p, double seconds) { current_.ms[std::size_t(p)] += seconds * 1000.0; }
    void end_frame(std::uint64_t frame) {
        current_.frame = frame;
        current_.qpc_end = qpc();
        current_.ms[sim] = (std::max)(0.0, current_.ms[sim] - current_.ms[script]);
        if (keep_rows_) rows_.push_back(current_);
        roll(current_);
    }
    // The last completed window's per-frame means (frames 0 until the first window closes).
    const Averages& averages() const { return averages_; }
    // Folds one row into the rolling window; the window closes once it spans window_seconds of loop starts.
    static constexpr double window_seconds = 0.5;
    void roll(const Row& r) {
        if (window_.frames == 0) window_.qpc_begin = r.qpc_begin;
        for (std::size_t p = 0; p < phase_count; ++p) window_.sum[p] += r.ms[p];
        window_.max_total = (std::max)(window_.max_total, total(r));
        ++window_.frames;
        const double span = double(r.qpc_end - window_.qpc_begin) / double(frequency_ ? frequency_ : 1);
        if (span < window_seconds) return;
        Averages a;
        a.frames = window_.frames;
        for (std::size_t p = 0; p < phase_count; ++p) {
            a.ms[p] = window_.sum[p] / double(window_.frames);
            a.total += a.ms[p];
        }
        a.max_total = window_.max_total;
        a.fps = span > 0 ? double(window_.frames) / span : 0;
        averages_ = a;
        window_ = Window{};
    }
    const std::vector<Row>& rows() const { return rows_; }

    // Statistics over rows [first, rows().size()) (skip a warm-up).
    Stats stats(int phase, std::size_t first = 0) const {
        std::vector<double> v;
        for (std::size_t i = first; i < rows_.size(); ++i)
            v.push_back(phase < 0 ? total(rows_[i]) : rows_[i].ms[std::size_t(phase)]);
        return stats_of(v);
    }
    static double total(const Row& r) {
        double t = 0;
        for (double m : r.ms) t += m;
        return t;
    }
    static Stats stats_of(std::vector<double> v) {
        Stats s;
        if (v.empty()) return s;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        s.mean = sum / double(v.size());
        auto pct = [&](double p) { return v[(std::min)(v.size() - 1, std::size_t(p * double(v.size() - 1) + 0.5))]; };
        s.p50 = pct(0.50);
        s.p95 = pct(0.95);
        s.p99 = pct(0.99);
        s.max = v.back();
        return s;
    }

    std::string summary(std::size_t first = 0) const {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << rows_.size() - (std::min)(first, rows_.size()) << " frames (ms mean / p50 / p95 / p99 / max):";
        for (int p = -1; p < phase_count; ++p) {
            const auto s = stats(p, first);
            out << " " << (p < 0 ? "total" : names[std::size_t(p)]) << " " << s.mean << " / " << s.p50 << " / " << s.p95 << " / " << s.p99
                << " / " << s.max << ";";
        }
        return out.str();
    }

    void write(const std::filesystem::path& file) const {
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("--profile: cannot write " + file.string());
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        out << "# omega_play --profile (port-only, checkpoint 132win9); qpc_frequency " << f.QuadPart << "\n";
        out << "frame,qpc_begin,qpc_end,sim_ms,script_ms,render_ms,present_ms\n";
        out << std::fixed << std::setprecision(4);
        for (const auto& r : rows_)
            out << r.frame << "," << r.qpc_begin << "," << r.qpc_end << "," << r.ms[sim] << "," << r.ms[script] << "," << r.ms[render] << ","
                << r.ms[present] << "\n";
    }

private:
    struct Window {
        std::array<double, phase_count> sum{};
        double max_total = 0;
        unsigned frames = 0;
        std::int64_t qpc_begin = 0;
    };
    Row current_;
    std::vector<Row> rows_;
    bool keep_rows_ = true;
    std::int64_t frequency_ = 0;
    Window window_;
    Averages averages_;
};

// Adds the scope's duration to a phase of an optional profile.
class ProfileScope {
public:
    ProfileScope(FrameProfile* profile, FrameProfile::Phase phase)
        : profile_(profile), phase_(phase), begin_(profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~ProfileScope() {
        if (profile_) profile_->add(phase_, std::chrono::duration<double>(std::chrono::steady_clock::now() - begin_).count());
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    FrameProfile* profile_;
    FrameProfile::Phase phase_;
    std::chrono::steady_clock::time_point begin_;
};

}  // namespace omega
