#pragma once
// Port-only diagnostic (checkpoint 132win13, not the original): the pacing probe. omega_play --pacing-probe
// <rate Hz> <frames> runs the interactive loops' pacing (frame_pacer.hpp LoopPacing) against the real display
// in a small game window placed off every monitor (shown without activation, never focused, destroyed at the
// end): each loop waits as the play's does, clears the back buffer and presents with the pacing's sync interval.
// Each frame is placed on the DWM's vertical-blank grid (DwmGetCompositionTimingInfo's qpcVBlank and refresh
// period, sampled after every present; IDXGIOutput::WaitForVBlank does not block on hybrid-GPU laptops): the first
// blank at least 0.25 ms after its submit and, with sync interval n, at least n blanks after the previous frame's.
// It reports how many blanks each frame was shown for, the present returns' intervals, the steps per presented
// frame, the steps' rate, the misses (a frame shown longer than ceil(refresh x dt) blanks, or a catch-up of more
// than 2 steps; the 8 frames after a miss are left out of the judder) and, between misses, the motion judder (the
// spread of steps x dt against the frames' blank times, in refreshes) and its 10-frame balance. The cadence of a
// 144 / 75 / 165 Hz display is so measured on a 60 Hz one at the rate with the same ratio (a 25 Hz rate is
// 144 / 60 = 2.4 refreshes per frame). With --frame-times it also writes <file>.frames.csv (frame, submit_ms,
// return_ms, blank_ms, blanks_shown, steps, sync_interval). Nothing of the game runs.

#include <filesystem>

namespace omega {

struct PacingProbeOptions {
    double rate_hz = 60.0;
    unsigned frames = 300;
    bool vsync = true;
    bool cadence = true;         // --frame-cadence off: the 132win3 rule (timer-paced unless a whole multiple)
    double work_ms = 0.0;        // --pacing-probe-work: a simulated frame's CPU time (half to all of it, varying)
    std::filesystem::path frame_times;  // --frame-times: the present-to-present intervals (ms)
};

// Returns the process exit code (0 when the probe ran).
int run_pacing_probe(const PacingProbeOptions& options);

}  // namespace omega
