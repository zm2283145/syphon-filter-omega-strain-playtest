#include "movie_audio_stage.hpp"
#include "audio_output.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace omega::movie {
namespace {
constexpr std::size_t grain = 512;
constexpr std::size_t audio_ahead = 48000;  // one second; at most one extra 32 KiB PSS read is added
constexpr std::uint64_t no_origin = ~std::uint64_t{0};
constexpr std::uint64_t output_lag = 1920;  // Windows' four 480-frame XAudio2 buffers (PS5 has less)
constexpr double picture_rate = 30000.0 / 1001.0;
}

std::size_t movie_audio_prime_padding(std::size_t pcm_frames, std::size_t prime_frames) {
    const std::size_t minimum = prime_frames + grain;
    return pcm_frames && pcm_frames < minimum ? minimum - pcm_frames : 0;
}

MovieAudioStage::MovieAudioStage(audio::Device& device) : device_(device), thread_([this] { run(); }) {}
MovieAudioStage::~MovieAudioStage() { stop(); }

void MovieAudioStage::push(std::vector<std::int16_t> pcm) {
    if (pcm.empty()) return;
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    ++stats_.pcm_calls;
    stats_.pcm_frames += pcm.size() / 2;
    stats_.max_pcm_frames = std::max(stats_.max_pcm_frames, pcm.size() / 2);
    fifo_.insert(fifo_.end(), std::make_move_iterator(pcm.begin()), std::make_move_iterator(pcm.end()));
}

void MovieAudioStage::finish_input() {
    std::lock_guard lock(mutex_);
    if (input_finished_) return;
    input_finished_ = true;
    // PcmRing starts after 150 ms are queued. A shorter movie must carry its final real samples through that
    // threshold; silence is appended only after the true PCM and is not counted as part of the movie payload.
    const auto prime = std::size_t(device_.latency_seconds() * audio::Mixer::output_rate);
    if (!failed_) {
        const std::size_t padding = movie_audio_prime_padding(std::size_t(stats_.pcm_frames), prime);
        fifo_.insert(fifo_.end(), padding * 2, std::int16_t{0});
        stats_.padding_frames += padding;
    }
}

bool MovieAudioStage::take_grain(std::vector<std::int16_t>& out) {
    std::lock_guard lock(mutex_);
    const std::size_t n = std::min((fifo_.size() - fifo_at_) / 2, grain);
    if (!n) return false;
    out.assign(fifo_.begin() + std::ptrdiff_t(fifo_at_), fifo_.begin() + std::ptrdiff_t(fifo_at_ + 2 * n));
    fifo_at_ += 2 * n;
    if (fifo_at_ == fifo_.size()) {
        fifo_.clear();
        fifo_at_ = 0;
    } else if (fifo_at_ >= (1u << 18)) {
        fifo_.erase(fifo_.begin(), fifo_.begin() + std::ptrdiff_t(fifo_at_));
        fifo_at_ = 0;
    }
    return true;
}

void MovieAudioStage::run() {
    try {
        const std::size_t target = std::size_t(device_.latency_seconds() * audio::Mixer::output_rate) + grain;
        std::vector<std::int16_t> pcm;
        while (!stopping_) {
            const auto before = device_.stats();
            if (before.lost) {
                std::lock_guard lock(mutex_);
                failed_ = true;
            }
            bool failed;
            {
                std::lock_guard lock(mutex_);
                failed = failed_;
            }
            if (released_ && !paused_ && !failed && before.queued_frames < target && take_grain(pcm)) {
                const auto begun = std::chrono::steady_clock::now();
                {
                    std::lock_guard lock(mutex_);
                    if (origin_ == no_origin) origin_ = before.submitted_frames;
                }
                device_.submit(pcm);
                const auto after = device_.stats();
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
                std::lock_guard lock(mutex_);
                ++stats_.submit_calls;
                stats_.submitted_frames += pcm.size() / 2;
                stats_.submit_total_ms += ms;
                stats_.submit_max_ms = std::max(stats_.submit_max_ms, ms);
                stats_.max_device_queued = std::max(stats_.max_device_queued, after.queued_frames);
                if (after.dropped_frames > before.dropped_frames || after.lost) failed_ = true;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (...) {
        // A device error must not terminate the process from the output thread; PacedPssMovie falls back to fields.
        std::lock_guard lock(mutex_);
        failed_ = true;
    }
}

std::size_t MovieAudioStage::buffered_frames() const {
    const auto device = device_.stats();
    std::lock_guard lock(mutex_);
    return (fifo_.size() - fifo_at_) / 2 + device.queued_frames;
}

std::uint64_t MovieAudioStage::played_frames() const {
    const auto device = device_.stats();
    std::lock_guard lock(mutex_);
    if (origin_ == no_origin) return 0;
    const std::uint64_t taken = device.submitted_frames - std::min<std::uint64_t>(device.queued_frames, device.submitted_frames);
    return taken > origin_ + output_lag ? taken - origin_ - output_lag : 0;
}

bool MovieAudioStage::started() const {
    std::lock_guard lock(mutex_);
    return origin_ != no_origin;
}

bool MovieAudioStage::drained() const {
    const auto device = device_.stats();
    std::lock_guard lock(mutex_);
    return (fifo_.size() == fifo_at_) &&
           stats_.submitted_frames == stats_.pcm_frames + stats_.padding_frames && device.queued_frames <= grain;
}

bool MovieAudioStage::unavailable() const {
    if (device_.stats().lost) return true;
    std::lock_guard lock(mutex_);
    return failed_;
}

MovieAudioStageStats MovieAudioStage::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void MovieAudioStage::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    stats_.discarded_frames += (fifo_.size() - fifo_at_) / 2;
    fifo_.clear();
    fifo_at_ = 0;
}

PacedPssMovie::PacedPssMovie(std::unique_ptr<PssMovie> inner, MovieAudioSink& audio)
    : inner_(std::move(inner)), audio_(audio) {}

void PacedPssMovie::collect_audio() {
    std::vector<std::int16_t> pcm;
    inner_->take_audio(pcm);
    if (pcm.empty() && early_.empty()) return;
    int rate = 0, channels = 0;
    if (!inner_->audio_format(rate, channels)) {
        early_.insert(early_.end(), pcm.begin(), pcm.end());
        return;
    }
    if (rate != 48000 || channels != 2) supported_ = false;
    if (!early_.empty()) {
        early_.insert(early_.end(), pcm.begin(), pcm.end());
        pcm.swap(early_);
        early_.clear();
    }
    if (supported_ && !audio_.unavailable()) audio_.push(std::move(pcm));
    else passthrough_.insert(passthrough_.end(), pcm.begin(), pcm.end());
}

void PacedPssMovie::top_up() {
    if (!supported_ || audio_.unavailable()) return;
    // A missing audio header must not scan the entire movie before its first picture. Sixteen 32 KiB reads are
    // enough to find the SShd in the supported disc movies; later calls can top up another bounded slice.
    for (unsigned n = 0; n < 16 && supported_ && audio_.buffered_frames() < audio_ahead; ++n) {
        if (!inner_->read_ahead()) break;
        collect_audio();
    }
    if (inner_->source_done()) finish_audio_input();
}

void PacedPssMovie::finish_audio_input() {
    if (input_finished_) return;
    input_finished_ = true;
    audio_.finish_input();
}

bool PacedPssMovie::next_frame(mpeg2::Frame& frame) {
    repeated_ = false;
    top_up();
    if (first_) {
        first_ = false;
        const bool got = inner_->next_frame(frame);
        collect_audio();
        if (!got) {
            ended_ = true;
            finish_audio_input();
        }
        int rate = 0, channels = 0;
        if (!inner_->audio_format(rate, channels)) supported_ = false;  // silent PSS: field-paced, no clock wait
        return got;
    }
    if (!released_) {
        // The first picture has had a whole front-end field to reach present. Hidden/minimised runs also release.
        audio_.release();
        released_ = true;
    }
    const bool clock = supported_ && !audio_.unavailable();
    if (ended_) {
        if (!clock || audio_.drained()) return false;
        repeated_ = true;
        return true;
    }
    if (clock) {
        if (!audio_.started()) {
            repeated_ = true;
            return true;
        }
        auto due = std::uint64_t(double(audio_.played_frames()) * picture_rate / 48000.0);
        // The output keeps a few grains beyond Device::queued_frames. Once all PCM has gone to the device and its
        // queue is nearly empty, finish any last pictures by fields; the output-lag allowance must not strand EOF.
        if (audio_.drained()) due = std::max(due, picture_index_ + 1);
        if (picture_index_ >= due) {
            repeated_ = true;
            return true;
        }
        // A long host stall may put many pictures behind the sound. Bound one front-end call's decode work and
        // FIFO growth; later calls keep catching up without one multi-second frame.
        for (unsigned decoded = 0; picture_index_ < due && decoded < 4; ++decoded) {
            mpeg2::Frame next;
            const bool got = inner_->next_frame(next);
            collect_audio();
            if (!got) {
                ended_ = true;
                finish_audio_input();
                break;
            }
            frame = std::move(next);
            ++picture_index_;
        }
        // Hand the final picture to the renderer once; the next call observes ended_ and returns false.
        return true;
    }
    const bool got = inner_->next_frame(frame);
    collect_audio();
    if (got) ++picture_index_;
    else {
        ended_ = true;
        finish_audio_input();
    }
    return got;
}

void PacedPssMovie::take_audio(std::vector<std::int16_t>& out) {
    out.insert(out.end(), passthrough_.begin(), passthrough_.end());
    passthrough_.clear();
}

}  // namespace omega::movie
