#pragma once

// Port-only movie output for Windows and PS5. PSS demux produces PCM in bursts; the front-end's frame thread must
// not submit a whole burst to a real-time device. The Vita uses its own AheadMovie / SoundPump implementation.
#include "audio_device.hpp"
#include "movie_pss.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace omega::movie {

// Silence needed after a short PSS payload so PcmRing reaches its initial prime without losing the real tail.
std::size_t movie_audio_prime_padding(std::size_t pcm_frames, std::size_t prime_frames);

struct MovieAudioStageStats {
    std::uint64_t pcm_calls = 0, pcm_frames = 0, submit_calls = 0, submitted_frames = 0, padding_frames = 0,
                  discarded_frames = 0;
    std::size_t max_pcm_frames = 0, max_device_queued = 0;
    double submit_total_ms = 0.0, submit_max_ms = 0.0;
};

// Narrow clock/sink contract lets PacedPssMovie's picture and EOF behavior be tested without an output device.
class MovieAudioSink {
public:
    virtual ~MovieAudioSink() = default;
    virtual void push(std::vector<std::int16_t> pcm) = 0;
    virtual void release() = 0;             // after the first picture has been handed to the renderer
    virtual void finish_input() = 0;        // natural EOF; a short clip may need silent priming after its PCM
    virtual std::size_t buffered_frames() const = 0;
    virtual std::uint64_t played_frames() const = 0;
    virtual bool started() const = 0;
    virtual bool drained() const = 0;
    virtual bool unavailable() const = 0;   // lost output or a failed submit: advance pictures by fields
};

class MovieAudioStage final : public MovieAudioSink {
public:
    explicit MovieAudioStage(audio::Device& device);
    ~MovieAudioStage() override;
    MovieAudioStage(const MovieAudioStage&) = delete;
    MovieAudioStage& operator=(const MovieAudioStage&) = delete;

    void push(std::vector<std::int16_t> pcm) override;
    void release() override { released_.store(true); }
    void finish_input() override;
    std::size_t buffered_frames() const override;
    std::uint64_t played_frames() const override;
    bool started() const override;
    bool drained() const override;
    bool unavailable() const override;
    MovieAudioStageStats stats() const;
    void set_paused(bool paused) { paused_ = paused; }  // stop feeding; the queued residual still drains while muted
    void stop();  // joins the sole movie submitter before the front end resumes its sound mix

private:
    void run();
    bool take_grain(std::vector<std::int16_t>& out);
    audio::Device& device_;
    mutable std::mutex mutex_;
    std::vector<std::int16_t> fifo_;
    std::size_t fifo_at_ = 0;
    MovieAudioStageStats stats_;
    std::uint64_t origin_ = ~std::uint64_t{0};
    bool failed_ = false, input_finished_ = false;
    std::atomic<bool> released_{false}, stopping_{false}, paused_{false};
    std::thread thread_;
};

// One built-in PSS movie. It preserves the native MovieStream interface for FrontendSession: repeated_picture()
// holds a picture when the sound is not due, and natural EOF waits for the staged PCM rather than cutting its tail.
class PacedPssMovie final : public MovieStream {
public:
    PacedPssMovie(std::unique_ptr<PssMovie> inner, MovieAudioSink& audio);
    bool next_frame(mpeg2::Frame& frame) override;
    bool audio_format(int& rate, int& channels) override { return inner_->audio_format(rate, channels); }
    void take_audio(std::vector<std::int16_t>& out) override;
    bool repeated_picture() const override { return repeated_; }

private:
    void collect_audio();
    void top_up();
    void finish_audio_input();
    std::unique_ptr<PssMovie> inner_;
    MovieAudioSink& audio_;
    std::vector<std::int16_t> early_, passthrough_;
    bool repeated_ = false, first_ = true, released_ = false, ended_ = false, supported_ = true,
         input_finished_ = false;
    std::uint64_t picture_index_ = 0;
};

}  // namespace omega::movie
