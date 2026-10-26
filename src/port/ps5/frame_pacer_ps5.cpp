// Port-only (132ps5c, research/PS5.md "Interfaces"): frame_pacer.hpp's platform functions on the PS5 (the portable
// FrameClock / LoopPacing are frame_pacer.cpp's, shared with Windows and the Vita). The EGL window surface refreshes at
// 60 Hz (the ps5-opengl default display mode; param.json is written for 60 fps by mkapp.sh); the pacer sleeps with
// nanosleep, and the process CPU time is getrusage's.

#include "frame_pacer.hpp"

#include <sys/resource.h>
#include <time.h>

#include <cerrno>
#include <cmath>

namespace omega {

double display_refresh_hz(void*) { return 60.0; }

FramePacer::FramePacer() { high_resolution_ = true; }

FramePacer::~FramePacer() = default;

void FramePacer::sleep_for(double seconds) {
    if (!(seconds > 0.0)) return;
    const double whole = std::floor(seconds);
    timespec t{static_cast<time_t>(whole), static_cast<long>((seconds - whole) * 1e9)};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

double process_cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    const auto seconds = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) * 1e-6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

}  // namespace omega