#pragma once

// Host audio output (checkpoint 119i): a small stereo mixer for the decoded
// streams of the 989snd host (src/port/play/sound_iop_host.*) and a 16-bit
// PCM WAV writer. Nothing here is from the game: the IOP's SPU2 mixing,
// 989snd.irx's pan table and its resampling are not ported, so the output
// rate, the pan law and the interpolation below are host choices
// (research/GENERATOR_RUNTIME.md, checkpoint 119i). Headless: without a WAV
// path the mixer still advances the voices (their ends drive the stream
// callbacks) but writes nothing.

#include "audio_formats.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega::audio {

// 989snd volume (0..0x400) and pan (degrees, 90 right, 270 left; -1 no pan)
// to left / right gains: equal-power over sin(pan), scaled so that the
// centre is unity on both sides, clipped to 1.
std::pair<float, float> pan_gains(std::int32_t volume, std::int32_t pan);

class Mixer {
public:
    static constexpr std::uint32_t output_rate = 48000;
    using Voice = std::uint32_t;

    // A looping voice wraps to its first frame and never ends.
    Voice start(std::shared_ptr<const PcmClip> clip, float left, float right, bool loop = false);
    void set_gains(Voice voice, float left, float right);
    // The gains of one source channel to the left / right output (a stereo
    // clip starts with channel 0 left only and channel 1 right only).
    void set_channel_gains(Voice voice, std::size_t channel, float left, float right);
    // A paused voice holds its position and adds nothing to the mix.
    void pause(Voice voice, bool paused);
    void stop(Voice voice);
    bool playing(Voice voice) const { return voices_.count(voice) != 0; }
    // [source channel][output side] gains of a playing voice (zero if none).
    std::array<std::array<float, 2>, 2> channel_gains(Voice voice) const;
    std::size_t voices() const { return voices_.size(); }
    // The source frames [0, n) the next mix of rames output frames can read from clip (n may pass the
    // clip's end when a voice loops; 0 when no voice plays it). Port-only (checkpoint 132win5).
    std::size_t frames_read_by(const PcmClip* clip, std::size_t frames) const;
    // Mixes `frames` output frames (appended to `out`, interleaved stereo) and
    // returns the voices that reached the end of their clip.
    std::vector<Voice> mix(std::size_t frames, std::vector<std::int16_t>& out);

private:
    struct State {
        std::shared_ptr<const PcmClip> clip;
        std::uint64_t position{};  // 32.32 in source frames
        std::uint64_t step{};
        float gain[2][2]{};  // [source channel][output side]
        bool loop{};
        bool paused{};
    };
    std::map<Voice, State> voices_;
    Voice next_{1};
};

// Interleaved stereo PCM between the simulation (writer) and a real-time
// output (reader). The reader outputs silence until `prime_frames` are queued;
// once playing, a read that finds fewer frames than it needs plays what is
// there, fills the rest with silence, counts one underrun and primes again, so
// a simulation slower than real time gives counted gaps of at least the
// priming latency instead of a stream of tiny glitches. Not thread-safe (the
// device wraps it in a lock); host-only, not from the game.
class PcmRing {
public:
    PcmRing(std::size_t capacity_frames, std::size_t prime_frames);
    std::size_t queued() const { return queued_; }
    std::size_t space() const { return capacity_ - queued_; }
    bool playing() const { return playing_; }
    // Appends up to `frames` frames; returns how many fitted.
    std::size_t write(const std::int16_t* samples, std::size_t frames);
    // Fills exactly `frames` frames of `out` (interleaved stereo).
    void read(std::int16_t* out, std::size_t frames);
    std::size_t underruns() const { return underruns_; }
    std::uint64_t silent_frames() const { return silent_; }  // after the first priming
    // Port-only (checkpoint 132win6): drops what is queued and primes again (an output device re-opened after
    // a loss starts from the simulation's present, not from the audio queued before the loss). Counts kept.
    void clear() {
        head_ = queued_ = 0;
        playing_ = false;
    }

private:
    std::vector<std::int16_t> data_;
    std::size_t capacity_, prime_, head_{}, queued_{};
    bool playing_{}, started_{};
    std::size_t underruns_{};
    std::uint64_t silent_{};
};

// Port-only (checkpoint 132win6): the output device's loss and re-open schedule (audio_device.cpp). A loss
// (the device unplugged or invalidated, the default output switched, or the device no longer pulling audio)
// closes the output; the next submit re-opens it on the current default device, and while that fails it is
// retried at most once per `retry_seconds`. While lost, submits drop their samples at once (the simulation
// never waits on a missing device, so its step and the game's timing are unchanged). Times are seconds on
// any monotonic clock.
class OutputRecovery {
public:
    explicit OutputRecovery(double retry_seconds = 1.0) : retry_(retry_seconds) {}
    void set_retry(double seconds) { retry_ = seconds; }
    // A loss seen at `now` (one loss until the output is re-opened).
    void lose(double now) {
        if (lost_) return;
        lost_ = true;
        since_ = now;
        next_try_ = now;
        ++losses_;
    }
    // Port-only (checkpoint 132win7): no output device when the output was made. The first open is retried
    // like a re-open after `retry_seconds` (it is not counted as a loss).
    void start_absent(double now) {
        lost_ = true;
        absent_ = true;
        since_ = now;
        next_try_ = now + retry_;
    }
    // A device arrived or the default output changed while lost: the next submit tries at once.
    void retry_soon(double now) {
        if (lost_ && next_try_ > now) next_try_ = now;
    }
    bool started_absent() const { return absent_; }
    bool lost() const { return lost_; }
    bool try_now(double now) const { return lost_ && now >= next_try_; }
    void reopened() {
        lost_ = false;
        ++reopens_;
    }
    void reopen_failed(double now) {
        next_try_ = now + retry_;
        ++failed_;
    }
    double lost_since() const { return since_; }
    std::size_t losses() const { return losses_; }
    std::size_t reopens() const { return reopens_; }
    std::size_t failed_reopens() const { return failed_; }

private:
    double retry_;
    bool lost_ = false, absent_ = false;
    double since_ = 0, next_try_ = 0;
    std::size_t losses_ = 0, reopens_ = 0, failed_ = 0;
};

// Port-only test events for the output device (omega_play --test-audio-device "F:event,..."; F is the play
// frame): lost (a device invalidation), gone (lost with no device until "back"), back, switch (the default
// output device switched), and (checkpoint 132win7) absent: no output device when the output is made (only
// "0:absent", first; the output starts silently until "back"). Throws on a malformed list or frames out of order.
struct AudioDeviceEvent {
    enum class Kind { lost, gone, back, default_switch, absent };
    std::uint64_t frame{};
    Kind kind{};
};
std::vector<AudioDeviceEvent> parse_audio_device_events(std::string_view list);
inline bool starts_absent(const std::vector<AudioDeviceEvent>& events) {
    return !events.empty() && events.front().kind == AudioDeviceEvent::Kind::absent;
}

class WavWriter {
public:
    WavWriter(const std::string& path, std::uint32_t rate, std::uint16_t channels);
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    void write(const std::vector<std::int16_t>& samples);
    void close();
    std::uint64_t frames() const { return bytes_ / (2ULL * channels_); }

private:
    std::ofstream file_;
    std::uint32_t rate_;
    std::uint16_t channels_;
    std::uint64_t bytes_{};
    bool open_{};
};

}
