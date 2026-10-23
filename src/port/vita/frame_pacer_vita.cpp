// Port-only (checkpoint 132vt1, research/VITA.md "Platform layer"): frame_pacer.hpp's platform functions on the
// PS Vita (the portable FrameClock / LoopPacing are frame_pacer.cpp's, shared with Windows). The display refreshes at
// the rate sceDisplayGetRefreshRate reports (59.94 Hz on the handheld's OLED / LCD); the pacer sleeps with
// sceKernelDelayThread (microsecond resolution, the kernel's tick granularity).

#include "frame_pacer.hpp"

#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>

#include <cmath>

namespace omega {

double display_refresh_hz(void*) {
    float fps = 0.0F;
    if (sceDisplayGetRefreshRate(&fps) < 0 || !(fps > 1.0F)) return 0.0;
    return double(fps);
}

FramePacer::FramePacer() { high_resolution_ = true; }

FramePacer::~FramePacer() = default;

void FramePacer::sleep_for(double seconds) {
    if (!(seconds > 0.0)) return;
    sceKernelDelayThread(static_cast<SceUInt>(std::ceil(seconds * 1e6)));
}

// The Vita reports no per-process CPU time to user code: the pacing summary's CPU share reads 0.
double process_cpu_seconds() { return 0.0; }

}  // namespace omega
