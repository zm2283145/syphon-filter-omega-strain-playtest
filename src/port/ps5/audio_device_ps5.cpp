// Port-only (132ps5c, research/PS5.md "Interfaces"): audio::Device (audio_device.hpp) on the PS5's own audio output,
// sceAudioOut (the system user's main port, 48 kHz signed 16-bit stereo), in place of the Windows build's XAudio2
// audio_device.cpp. The game's mixer output (16-bit interleaved stereo at Mixer::output_rate, 48 kHz) goes through the
// same PcmRing (audio_output.hpp) as on Windows: submit() appends to it (past a full ring into a bounded FIFO; it
// waits only beyond that, as the XAudio2 device does), and an output thread pulls one 512-frame grain at a time
// (10.7 ms; the Windows device's buffers are 10 ms) and blocks in sceAudioOutOutput until the system takes it. The
// grains rotate through three buffers, so a grain is never rewritten while it may still be read. The PS5's output is
// the system's (it follows the TV / headset itself), so the loss / default-device-switch paths of the Windows device do
// not occur: the simulate_* hooks only drive the same OutputRecovery bookkeeping so the shared code sees the same
// states. Muting (the port-only test switch, the background mute) scales the grain to zero in software.
// 132ps5c hw10 (the first in-game run had audio skips): the output thread runs at a raised priority (scePthreadSetprio;
// the 7 draw workers, the render and GL threads otherwise share the cores with it at the default), paces itself to
// real time when sceAudioOutOutput returns early (as the Melee PS5 port's audio does), and logs every 10 s: the grains
// sent against the wall clock, the output call's time, early returns, the longest gap between grains, and the ring's
// underruns / silent frames / queue (an underrun is the game's mixer not keeping up: the simulation behind).

#include "audio_device.hpp"

#include "audio_output.hpp"
#include "thread_roles.hpp"
#include "ps5_log.hpp"

#include <pthread.h>
#include <unistd.h>

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// libSceAudioOut (ps5-payload-sdk imports).
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned length, unsigned frequency, unsigned param);
int sceAudioOutOutput(int handle, const void* samples);
int sceAudioOutClose(int handle);
int scePthreadSetprio(pthread_t thread, int priority);  // libkernel: 256 (highest) .. 767, default 700
}

namespace omega::audio {
namespace {

constexpr int grain_frames = 512;
constexpr int audio_user_system = 0xff;   // SCE_USER_SERVICE_USER_ID_SYSTEM
constexpr int audio_port_main = 0;        // SCE_AUDIO_OUT_PORT_TYPE_MAIN
constexpr unsigned audio_s16_stereo = 1;  // SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
constexpr int audio_thread_priority = 383;  // above the default 700 (game threads), below the system's highest

double seconds_now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

}  // namespace
struct Device::Impl {
    Impl(std::size_t prime, bool mute) : ring(prime * 2, prime), muted(mute), grain(buffers * grain_frames * 2) {}

    ~Impl() { close(); }

    void open() {
        static const int init = sceAudioOutInit();  // once per process (0x8026000e: already initialised)
        (void)init;
        port = sceAudioOutOpen(audio_user_system, audio_port_main, 0, grain_frames, Mixer::output_rate, audio_s16_stereo);
        if (port <= 0) {
            const int code = port;
            port = -1;
            throw std::runtime_error("Audio device: sceAudioOutOpen failed (" + std::to_string(code) + ")");
        }
        closing = false;
        thread = std::thread([this] { run(); });
    }

    void close() {
        closing = true;
        drained.notify_all();
        if (thread.joinable()) thread.join();
        if (port >= 0) sceAudioOutClose(port);
        port = -1;
    }

    // The output is silenced in software (the grain zeroed after the ring read): muted or in the background.
    bool silent() const { return muted || background_muted.load(); }

    // The output thread: one grain from the ring (silence while priming or on an underrun), then the blocking output.
    void run() {
        enter_thread_role(ThreadRole::audio, "audio out");
        const int prio = scePthreadSetprio(pthread_self(), audio_thread_priority);
        ps5::log("Audio device (port-only, PS5): output thread priority %d: 0x%08x", audio_thread_priority, unsigned(prio));
        const double grain_seconds = double(grain_frames) / double(Mixer::output_rate);
        double window_start = seconds_now(), last_return = window_start, due = 0.0;
        std::uint64_t grains = 0, early = 0, paced = 0, last_underruns = 0;
        double call_sum = 0.0, call_max = 0.0, gap_max = 0.0;
        for (std::size_t next = 0; !closing; next = (next + 1) % buffers) {
            std::int16_t* out = grain.data() + next * grain_frames * 2;
            {
                ThreadBusyScope busy_time;
                std::lock_guard lock(mutex);
                feed();
                ring.read(out, grain_frames);
                if (silent()) std::fill(out, out + grain_frames * 2, std::int16_t(0));
                feed();
                last_pull = seconds_now();
            }
            drained.notify_all();
            const double before = seconds_now();
            sceAudioOutOutput(port, out);
            double now = seconds_now();
            const double call = now - before;
            call_sum += call;
            call_max = std::max(call_max, call);
            gap_max = std::max(gap_max, now - last_return);
            ++grains;
            // A blocking port returns when the system has taken the grain (about a grain's time); one that returns
            // at once would let this loop drain the ring faster than real time, so it is held to the grain clock.
            if (due == 0.0 || now > due + 4 * grain_seconds) due = now;
            due += grain_seconds;
            if (call < 0.001) ++early;
            if (due > now + 0.002) {
                ++paced;
                usleep(useconds_t((due - now - 0.001) * 1e6));
                now = seconds_now();
            }
            last_return = now;
            if (now - window_start >= 10.0) {
                std::uint64_t underruns, silent;
                std::size_t queued;
                {
                    std::lock_guard lock(mutex);
                    underruns = ring.underruns();
                    silent = ring.silent_frames();
                    queued = ring.queued() + overflow_frames();
                }
                const double span = now - window_start;
                ps5::log("Audio out (PS5): %llu grains in %.2f s (%.1f %% of real time), output call mean %.2f ms max %.2f ms, "
                         "%llu early returns, %llu paced, longest gap %.1f ms; ring underruns %llu (+%llu), silent frames %llu, "
                         "queued %.0f ms",
                         (unsigned long long)grains, span, 100.0 * double(grains) * grain_seconds / span,
                         1000.0 * call_sum / double(grains), 1000.0 * call_max, (unsigned long long)early,
                         (unsigned long long)paced, 1000.0 * gap_max, (unsigned long long)underruns,
                         (unsigned long long)(underruns - last_underruns), (unsigned long long)silent,
                         1000.0 * double(queued) / double(Mixer::output_rate));
                last_underruns = underruns;
                window_start = now;
                grains = early = paced = 0;
                call_sum = call_max = gap_max = 0.0;
            }
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
    std::atomic<bool> background_muted{false};
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
        d.log.push_back("Audio device (port-only, PS5): sceAudioOut main port " + std::to_string(d.port) + ", " +
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
    s.background_muted = d.background_muted.load();
    return s;
}

void Device::set_background_muted(bool on) {
    auto& d = *impl_;
    if (d.background_muted == on) return;
    d.background_muted = on;
    d.log.push_back(on ? "Audio device (port-only): output silenced while the game is in the background"
                       : "Audio device (port-only): output back in the foreground");
}

bool Device::background_muted() const { return impl_->background_muted.load(); }

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
