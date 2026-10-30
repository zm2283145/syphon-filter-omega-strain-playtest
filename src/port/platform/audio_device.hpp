#pragma once

// Real-time host audio output (checkpoint 119j): the mixer's 48 kHz stereo
// PCM (audio_output.hpp) played on the default Windows device through an
// XAudio2 source voice. Nothing here is from the game (the PS2 SPU2 output is
// not modelled); omega_play opens it only for windowed play with --audio-out,
// so headless and regression runs never touch an audio device.
//
// The simulation writes each frame's mix into a PcmRing; the XAudio2 voice
// callback pulls fixed 10 ms chunks from it. Playback starts once `latency`
// seconds are queued. If the simulation falls behind real time the ring runs
// dry: the device plays silence, counts an underrun and waits for `latency`
// to be queued again (PcmRing). If the simulation runs ahead (an unpaced
// scripted run), submit() waits for room, so the run is paced by the audio
// clock; only after waiting a second does it drop and count the excess.
//
// Port-only (checkpoint 132win6): device changes. The output is lost when
// XAudio2 reports a critical error (the device unplugged, disabled or
// invalidated), a voice error, the default render device (eConsole) changes
// (IMMNotificationClient), or the device stops pulling audio for 250 ms while
// the ring is full. The next submit closes the voices and the engine, clears
// the ring and re-opens on the current default device; while that fails
// (no output device) it retries once a second and every submit drops its
// samples at once (OutputRecovery, audio_output.hpp). The simulation never
// waits on a lost device, so the game's step and timing are unchanged; the
// re-opened output starts from the simulation's present after `latency`.
//
// Port-only (checkpoint 132win7): no device at startup. When the first open fails (no output device) the
// Device is made anyway, in the lost state: it plays nothing, every submit drops its samples at once, and
// the open is retried through the same path (once a second, or at the next submit after a device arrives
// or the default output changes), so --audio-out and the front end start silently and pick up a device
// when one appears.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace omega::audio {

class Device {
public:
    struct Stats {
        std::size_t underruns{};
        std::uint64_t silent_frames{};     // silence played after the first priming
        std::uint64_t submitted_frames{};
        std::uint64_t dropped_frames{};
        std::size_t queued_frames{};
        // Port-only (checkpoint 132win6).
        std::size_t losses{};
        std::size_t reopens{};
        std::size_t failed_reopens{};
        std::uint64_t lost_frames{};       // dropped while the output was lost (part of dropped_frames)
        bool lost{};
        bool started_absent{};             // 132win7: no output device when the Device was made
        bool background_muted{};           // 132win12: silenced while the game window is unfocused
    };

    // Opens the default output device; with none (checkpoint 132win7) it starts silently in the lost state
    // and logs it (take_log). Throws only on a bad latency. `muted` (port-only tests) plays at master volume
    // 0; `simulate_absent` (port-only tests, "0:absent") starts as with no device until simulate_device_back.
    explicit Device(double latency_seconds = 0.2, bool muted = false, bool simulate_absent = false);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Queues interleaved stereo samples at Mixer::output_rate.
    void submit(const std::vector<std::int16_t>& samples);
    Stats stats() const;
    double latency_seconds() const { return latency_; }
    // The losses and re-opens since the last call, one line each ("Audio device: ...").
    std::vector<std::string> take_log();
    // Port-only (checkpoint 132win12): silences the output while the game window is unfocused (the source
    // voice at volume 0, kept across a re-open); the mixing and submits go on, so the sound stays in step with
    // the game and resumes without a gap or a burst. Logs the change (take_log).
    void set_background_muted(bool on);
    bool background_muted() const;

    // Port-only tests (--test-audio-device, tests/audio_device_tests.cpp): a loss reported as a device
    // invalidation (`gone`: the re-opens fail as with no output device until simulate_device_back), or as
    // a default device switch; the retry interval (1 s).
    void simulate_loss(bool gone);
    void simulate_default_change();
    void simulate_device_back();  // also a device arrival: a lost output retries at the next submit
    void set_retry_seconds(double seconds);

private:
    struct Impl;
    // Port-only: the movie feeder may call submit/stats beside the front-end's mute, device test and log controls.
    // The platform callback keeps using Impl's ring mutex; public calls take this outer lock before touching
    // recovery, voices or logs. Recursive because simulate_default_change delegates to simulate_loss on PS5.
    mutable std::recursive_mutex public_mutex_;
    std::unique_ptr<Impl> impl_;
    double latency_;
};

}
