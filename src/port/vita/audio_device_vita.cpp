// Port-only (checkpoint 132vt1, research/VITA.md "Platform layer"): audio::Device (audio_device.hpp) on the PS Vita's
// SceAudioOut main port, replacing the Windows build's XAudio2 audio_device.cpp. The game's mixer output (16-bit
// interleaved stereo at Mixer::output_rate, 48 kHz) goes through the same PcmRing (audio_output.hpp) as on Windows:
// submit() appends to it (past a full ring into a bounded FIFO, 132vt2; it waits only beyond that, as the XAudio2
// device does), and an output thread pulls one
// 512-frame grain at a time (SceAudioOut grains are multiples of 64 frames; 512 at 48 kHz is 10.7 ms, the Windows
// device's buffers are 10 ms) and blocks in sceAudioOutOutput until the hardware takes it. A Vita has one fixed audio
// output, so the loss / default-device-switch paths of the Windows device do not occur here: the simulate_* hooks
// only drive the same OutputRecovery bookkeeping so the shared code sees the same states.

#include "audio_device.hpp"

#include "audio_output.hpp"
#include "thread_roles.hpp"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace omega::audio {
namespace {

constexpr int grain_frames = 512;

double seconds_now() { return double(sceKernelGetProcessTimeWide()) * 1e-6; }

}  // namespace

struct Device::Impl {
    Impl(std::size_t prime, bool mute) : ring(prime * 2, prime), muted(mute), grain(buffers * grain_frames * 2) {}

    ~Impl() { close(); }

    void open() {
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, grain_frames, Mixer::output_rate,
                                   SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0) throw std::runtime_error("Audio device: sceAudioOutOpenPort failed (" + std::to_string(port) + ")");
        apply_volume();
        closing = false;
        thread = std::thread([this] { run(); });
    }

    void close() {
        closing = true;
        drained.notify_all();
        if (thread.joinable()) thread.join();
        if (port >= 0) sceAudioOutReleasePort(port);
        port = -1;
    }

    void apply_volume() {
        if (port < 0) return;
        const int v = (muted || background_muted) ? 0 : SCE_AUDIO_VOLUME_0DB;
        int volume[2] = {v, v};
        sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                             volume);
    }

    // The output thread: one grain from the ring (silence while priming or on an underrun), then the blocking output.
    // Pinned to core 2 at the highest user priority (thread_roles.hpp): it shares the core with the worker pool.
    // sceAudioOutOutput returns once the grain is queued, while the hardware still plays the grain before it from
    // that buffer: the grains rotate through `buffers` so a grain is never rewritten while it plays (132vt1b; with one
    // buffer every grain was overwritten mid-play, the handheld's crackle; Vita3K copies at the call and hid it).
    void run() {
        enter_thread_role(ThreadRole::audio, "audio out");
        for (std::size_t next = 0; !closing; next = (next + 1) % buffers) {
            std::int16_t* out = grain.data() + next * grain_frames * 2;
            {
                ThreadBusyScope busy_time;
                std::lock_guard lock(mutex);
                feed();
                ring.read(out, grain_frames);
                feed();
                last_pull = seconds_now();
            }
            drained.notify_all();
            sceAudioOutOutput(port, out);
        }
        sceAudioOutOutput(port, nullptr);  // waits until the last grain has played before the buffers go
    }

    // 132vt2: what does not fit in the ring waits here (in order) instead of in submit, up to `overflow_cap` frames:
    // a movie's PCM arrives in bursts (a demuxed packet at a time) and the play host's front end submits it on the
    // session thread, which waited up to a second in each burst (132vt1b's hardware log: movie audio submit 15 ms
    // mean, 1005 ms max). The output thread moves it into the ring as the ring drains. Only a submit beyond the cap
    // (a simulation far ahead of real time) still waits, as on Windows. Called with `mutex` held.
    void feed() {
        while (overflow_at < overflow.size()) {
            const std::size_t frames = (overflow.size() - overflow_at) / 2;
            const std::size_t wrote = ring.write(overflow.data() + overflow_at, frames);
            if (wrote == 0) break;
            overflow_at += wrote * 2;
        }
        if (overflow_at == overflow.size()) {
            overflow.clear();
            overflow_at = 0;
        } else if (overflow_at > overflow.size() / 2) {
            overflow.erase(overflow.begin(), overflow.begin() + std::ptrdiff_t(overflow_at));
            overflow_at = 0;
        }
    }
    std::size_t overflow_frames() const { return (overflow.size() - overflow_at) / 2; }

    static constexpr std::size_t buffers = 3;
    static constexpr std::size_t overflow_cap = 2 * Mixer::output_rate;  // 2 s
    std::vector<std::int16_t> overflow;
    std::size_t overflow_at = 0;
    PcmRing ring;
    bool muted = false;
    bool background_muted = false;
    std::vector<std::int16_t> grain;
    int port = -1;
    std::thread thread;
    std::atomic<bool> closing{false};
    mutable std::mutex mutex;
    std::condition_variable drained;
    std::uint64_t submitted = 0, dropped = 0, lost_frames = 0;
    std::atomic<double> last_pull{0.0};
    OutputRecovery recovery;
    bool simulate_gone = false;
    std::vector<std::string> log;
};

Device::Device(double latency_seconds, bool muted, bool simulate_absent) : latency_(latency_seconds) {
    const auto prime = static_cast<std::size_t>(latency_seconds * Mixer::output_rate);
    if (prime < Mixer::output_rate / 100) throw std::runtime_error("Audio device: latency must be at least 10 ms");
    impl_ = std::make_unique<Impl>(prime, muted);
    auto& d = *impl_;
    d.simulate_gone = simulate_absent;
    try {
        if (d.simulate_gone) throw std::runtime_error("Audio device: no output device (simulated, port-only test)");
        d.open();
        d.log.push_back("Audio device (port-only, Vita): SceAudioOut main port " + std::to_string(d.port) + ", " +
                        std::to_string(grain_frames) + "-frame grains at " + std::to_string(Mixer::output_rate) + " Hz");
    } catch (const std::exception& e) {
        d.close();
        d.recovery.start_absent(seconds_now());
        d.log.push_back(std::string(e.what()) + "; starting without audio output (silent)");
    }
}

Device::~Device() = default;

void Device::submit(const std::vector<std::int16_t>& samples) {
    if (samples.size() % 2) throw std::runtime_error("Audio device: odd sample count");
    const auto frames = samples.size() / 2;
    auto& d = *impl_;
    const double now = seconds_now();
    if (d.recovery.lost() && !d.simulate_gone && d.recovery.try_now(now)) {
        try {
            d.close();
            d.ring.clear();
            d.overflow.clear();
            d.overflow_at = 0;
            d.open();
            d.recovery.reopened();
        } catch (const std::exception& e) {
            d.recovery.reopen_failed(now);
            d.log.push_back(e.what());
        }
    }
    if (d.recovery.lost()) {
        d.dropped += frames;
        d.lost_frames += frames;
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::unique_lock lock(d.mutex);
    std::size_t done = 0;
    while (true) {
        d.feed();
        if (d.overflow_frames() == 0) done += d.ring.write(samples.data() + done * 2, frames - done);
        if (done < frames && d.overflow_frames() + (frames - done) <= Impl::overflow_cap) {
            d.overflow.insert(d.overflow.end(), samples.begin() + std::ptrdiff_t(done * 2), samples.end());
            done = frames;
        }
        if (done == frames) break;
        if (d.drained.wait_for(lock, std::chrono::milliseconds(50)) == std::cv_status::timeout &&
            std::chrono::steady_clock::now() >= deadline)
            break;
    }
    d.submitted += done;
    d.dropped += frames - done;
}

Device::Stats Device::stats() const {
    std::lock_guard lock(impl_->mutex);
    const auto& d = *impl_;
    Stats s{d.ring.underruns(), d.ring.silent_frames(), d.submitted, d.dropped, d.ring.queued() + d.overflow_frames()};
    s.losses = d.recovery.losses();
    s.reopens = d.recovery.reopens();
    s.failed_reopens = d.recovery.failed_reopens();
    s.lost_frames = d.lost_frames;
    s.lost = d.recovery.lost();
    s.started_absent = d.recovery.started_absent();
    s.background_muted = d.background_muted;
    return s;
}

void Device::set_background_muted(bool on) {
    auto& d = *impl_;
    if (d.background_muted == on) return;
    d.background_muted = on;
    d.apply_volume();
    d.log.push_back(on ? "Audio device (port-only): output silenced while the game is in the background"
                       : "Audio device (port-only): output back in the foreground");
}

bool Device::background_muted() const { return impl_->background_muted; }

std::vector<std::string> Device::take_log() { return std::exchange(impl_->log, {}); }

void Device::simulate_loss(bool gone) {
    auto& d = *impl_;
    if (gone) d.simulate_gone = true;
    d.close();
    d.recovery.lose(seconds_now());
}

void Device::simulate_default_change() { simulate_loss(false); }

void Device::simulate_device_back() {
    impl_->simulate_gone = false;
    impl_->recovery.retry_soon(seconds_now());
}

void Device::set_retry_seconds(double seconds) { impl_->recovery.set_retry(seconds); }

}  // namespace omega::audio
