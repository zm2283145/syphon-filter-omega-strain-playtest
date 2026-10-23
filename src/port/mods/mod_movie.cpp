// Port-only (checkpoint 132md14): a mod's movie replacement (mod_movie.hpp, research/MODDING.md "Movie overrides").
#include "mod_movie.hpp"

#include "audio_files.hpp"
#include "mod_archive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace omega::mods {

namespace {

constexpr std::array<std::array<int, 2>, 8> frame_rates{{{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1},
                                                          {60000, 1001}, {60, 1}}};

std::string lower_extension(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string aspect_text(float a) {
    if (std::fabs(a - 4.0F / 3.0F) < 0.01F) return "4:3";
    if (std::fabs(a - 16.0F / 9.0F) < 0.01F) return "16:9";
    char text[32];
    std::snprintf(text, sizeof text, "%.2f:1", a);
    return text;
}

}  // namespace

float VideoInfo::display_aspect() const {
    switch (aspect_code) {
    case 2: return 4.0F / 3.0F;
    case 3: return 16.0F / 9.0F;
    case 4: return 2.21F;
    default: return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 0.0F;
    }
}

std::optional<VideoInfo> read_video_info(std::span<const std::uint8_t> head) {
    for (std::size_t i = 0; i + 8 <= head.size(); ++i) {
        if (head[i] != 0 || head[i + 1] != 0 || head[i + 2] != 1 || head[i + 3] != 0xb3) continue;
        VideoInfo info;
        info.width = (head[i + 4] << 4) | (head[i + 5] >> 4);
        info.height = ((head[i + 5] & 0x0f) << 8) | head[i + 6];
        info.aspect_code = head[i + 7] >> 4;
        info.rate_code = head[i + 7] & 0x0f;
        if (info.width <= 0 || info.height <= 0 || info.aspect_code < 1 || info.aspect_code > 4 || info.rate_code < 1 ||
            info.rate_code > 8)
            return std::nullopt;
        info.rate_num = frame_rates[std::size_t(info.rate_code - 1)][0];
        info.rate_den = frame_rates[std::size_t(info.rate_code - 1)][1];
        return info;
    }
    return std::nullopt;
}

std::uint64_t source_picture_at_pull(std::uint64_t pull, int rate_num, int rate_den) {
    return pull * std::uint64_t(rate_num) * 1001 / (std::uint64_t(rate_den) * 30000);
}

std::uint64_t audio_frames_through_pull(std::uint64_t pull, std::uint32_t rate) {
    return (pull + 1) * std::uint64_t(rate) * 1001 / 30000;
}

movie::PssMovie::Source mod_file_source(const std::filesystem::path& path) {
    std::string member;
    if (const auto zip = archive_holding(path, member)) {
        const ZipMember* m = zip->find(member);
        if (!m) throw std::runtime_error("no member " + member + " in " + zip->path().string());
        if (m->method == 0) {
            auto in = std::make_shared<std::ifstream>(zip->path(), std::ios::binary);
            std::array<std::uint8_t, 30> local{};
            in->seekg(m->local_offset);
            in->read(reinterpret_cast<char*>(local.data()), local.size());
            if (!*in || local[0] != 'P' || local[1] != 'K' || local[2] != 3 || local[3] != 4)
                throw std::runtime_error("bad local header of " + member + " in " + zip->path().string());
            const std::uint64_t data = std::uint64_t(m->local_offset) + 30 + (local[26] | local[27] << 8) + (local[28] | local[29] << 8);
            in->seekg(static_cast<std::streamoff>(data));
            return [in, left = std::uint64_t(m->size)](std::span<std::uint8_t> out) mutable -> std::size_t {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), left));
                if (!n) return 0;
                in->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
                const auto got = static_cast<std::size_t>(in->gcount());
                left -= got;
                return got;
            };
        }
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(zip->read(*m));
        return [bytes, done = std::size_t{0}](std::span<std::uint8_t> out) mutable -> std::size_t {
            const std::size_t n = std::min(out.size(), bytes->size() - done);
            std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(done), n, out.begin());
            done += n;
            return n;
        };
    }
    auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*in) throw std::runtime_error("cannot open " + path.string());
    return [in](std::span<std::uint8_t> out) -> std::size_t {
        in->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<std::size_t>(in->gcount());
    };
}

movie::MovieFile mod_movie_file(const std::filesystem::path& path) {
    movie::MovieFile file;
    file.name = path.filename().string();
    std::string member;
    if (const auto zip = archive_holding(path, member)) {
        const ZipMember* m = zip->find(member);
        if (!m) throw std::runtime_error("no member " + member + " in " + zip->path().string());
        file.name = std::filesystem::path(member).filename().string();
        file.size = m->size;
        if (m->method == 0) {
            auto in = std::make_shared<std::ifstream>(zip->path(), std::ios::binary);
            std::array<std::uint8_t, 30> local{};
            in->seekg(m->local_offset);
            in->read(reinterpret_cast<char*>(local.data()), local.size());
            if (!*in || local[0] != 'P' || local[1] != 'K' || local[2] != 3 || local[3] != 4)
                throw std::runtime_error("bad local header of " + member + " in " + zip->path().string());
            const std::uint64_t data = std::uint64_t(m->local_offset) + 30 + (local[26] | local[27] << 8) + (local[28] | local[29] << 8);
            const std::uint64_t size = file.size;
            file.read = [in, data, size](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
                if (offset >= size) return 0;
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size - offset));
                in->clear();
                in->seekg(static_cast<std::streamoff>(data + offset));
                in->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
                return static_cast<std::size_t>(in->gcount());
            };
            return file;
        }
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(zip->read(*m));
        file.size = bytes->size();
        file.read = [bytes](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= bytes->size()) return 0;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), bytes->size() - offset));
            std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
            return n;
        };
        return file;
    }
    auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*in) throw std::runtime_error("cannot open " + path.string());
    file.size = std::filesystem::file_size(path);
    file.read = [in](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
        in->clear();
        in->seekg(static_cast<std::streamoff>(offset));
        in->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<std::size_t>(in->gcount());
    };
    return file;
}

// ------------------------------------------------------------------ PlatformVideo

PlatformVideo::PlatformVideo(const movie::PlatformMovieBackend& backend, movie::MovieFile file, std::size_t queue)
    : queue_max_(std::max<std::size_t>(queue, 1)), backend_name_(backend.name) {
    worker_ = std::thread([this, backend, file = std::move(file)]() mutable { run(std::move(backend), std::move(file)); });
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return opened_; });
    if (!open_error_.empty()) {
        const auto why = open_error_;
        lock.unlock();
        worker_.join();
        throw std::runtime_error(why);
    }
}

PlatformVideo::~PlatformVideo() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PlatformVideo::run(movie::PlatformMovieBackend backend, movie::MovieFile file) {
    std::unique_ptr<movie::PlatformMovieDecoder> decoder;
    try {
        if (!backend.open) throw std::runtime_error(backend.name + " opens nothing");
        decoder = backend.open(std::move(file));
        if (!decoder) throw std::runtime_error(backend.name + " did not open it");
        const auto& info = decoder->info();
        if (info.width <= 0 || info.height <= 0 || info.width > 1920 || info.height > 1152)
            throw std::runtime_error("its pictures are " + std::to_string(info.width) + "x" + std::to_string(info.height) +
                                     ", past the 1920x1152 the front end shows");
        std::lock_guard lock(mutex_);
        info_ = info;
        if (info_.audio_rate <= 0 || info_.audio_channels <= 0) info_.audio_rate = info_.audio_channels = 0;
        sound_done_ = info_.audio_rate == 0;
        opened_ = true;
    } catch (const std::exception& e) {
        decoder.reset();
        std::lock_guard lock(mutex_);
        open_error_ = e.what();
        if (open_error_.empty()) open_error_ = "it could not be opened";
        opened_ = true;
        changed_.notify_all();
        return;
    }
    changed_.notify_all();

    constexpr std::uint64_t out_rate = 48000;
    const std::uint64_t rate = static_cast<std::uint64_t>(info_.audio_rate);
    const auto channels = static_cast<std::size_t>(info_.audio_channels);
    bool have_first = false, sound_started = false, source_end = false;
    double first = 0;
    std::vector<float> source;               // stereo source frames [source_base, ...)
    std::uint64_t source_base = 0, source_skip = 0, resampled = 0;
    std::vector<std::int16_t> made;
    const auto resample = [&] {
        made.clear();
        const std::uint64_t source_end_frame = source_base + source.size() / 2;
        for (;;) {
            const std::uint64_t num = resampled * rate;
            const std::uint64_t i = num / out_rate;
            if (i >= source_end_frame || (i + 1 >= source_end_frame && !source_end)) break;
            const float frac = static_cast<float>(num % out_rate) / static_cast<float>(out_rate);
            const std::size_t a = static_cast<std::size_t>(i - source_base) * 2;
            const std::size_t b = i + 1 < source_end_frame ? a + 2 : a;
            for (int c = 0; c < 2; ++c) {
                const float v = source[a + c] + (source[b + c] - source[a + c]) * frac;
                made.push_back(static_cast<std::int16_t>(std::clamp(std::lround(v), -32768L, 32767L)));
            }
            ++resampled;
        }
        const std::uint64_t keep_from = resampled * rate / out_rate;
        if (keep_from > source_base + 4096) {
            const auto drop = static_cast<std::size_t>(std::min<std::uint64_t>(keep_from, source_end_frame) - source_base);
            source.erase(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(drop * 2));
            source_base += drop;
        }
    };
    try {
        for (;;) {
            bool want_picture = false, want_sound = false;
            {
                std::unique_lock lock(mutex_);
                const auto decide = [&] {
                    want_picture = !pictures_done_ && pictures_.size() < queue_max_;
                    const std::uint64_t ahead = static_cast<std::uint64_t>(std::max(0.0, last_picture_ + 0.5) * out_rate);
                    want_sound = !sound_done_ && (have_first || pictures_done_) && sound_end_ < std::max(sound_wanted_, ahead);
                    return stop_ || want_picture || want_sound;
                };
                changed_.wait(lock, decide);
                if (stop_) break;
            }
            if (want_picture) {
                Picture picture;
                const bool got = decoder->next_picture(picture.frame, picture.seconds);
                std::lock_guard lock(mutex_);
                if (got) {
                    if (!have_first) {
                        have_first = true;
                        first = picture.seconds;
                    }
                    picture.seconds -= first;
                    last_picture_ = std::max(last_picture_, picture.seconds);
                    pictures_.push_back(std::move(picture));
                } else {
                    pictures_done_ = true;
                    if (error_.empty()) error_ = decoder->error();
                }
                changed_.notify_all();
                continue;
            }
            if (want_sound) {
                std::vector<std::int16_t> pcm;
                double seconds = 0;
                const bool got = decoder->next_audio(pcm, seconds);
                std::uint64_t silence = 0;
                if (got) {
                    if (!sound_started) {
                        sound_started = true;
                        const double lead = seconds - first;
                        if (lead > 0) silence = static_cast<std::uint64_t>(std::llround(lead * out_rate));
                        else source_skip = static_cast<std::uint64_t>(std::llround(-lead * static_cast<double>(rate)));
                    }
                    const std::size_t frames = pcm.size() / channels;
                    for (std::size_t f = 0; f < frames; ++f) {
                        if (source_skip) {
                            --source_skip;
                            continue;
                        }
                        const std::int16_t* in = pcm.data() + f * channels;
                        source.push_back(static_cast<float>(in[0]));
                        source.push_back(static_cast<float>(channels > 1 ? in[1] : in[0]));
                    }
                } else {
                    source_end = true;
                }
                resample();
                std::lock_guard lock(mutex_);
                sound_.insert(sound_.end(), silence * 2, 0);
                sound_end_ += silence;
                sound_.insert(sound_.end(), made.begin(), made.end());
                sound_end_ += made.size() / 2;
                if (!got) {
                    sound_done_ = true;
                    if (error_.empty()) error_ = decoder->error();
                }
                changed_.notify_all();
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = e.what();
    }
    decoder.reset();
    std::lock_guard lock(mutex_);
    pictures_done_ = sound_done_ = true;
    changed_.notify_all();
}

bool PlatformVideo::pop(mpeg2::Frame& frame, double& seconds) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return !pictures_.empty() || pictures_done_; });
    if (pictures_.empty()) return false;
    frame = std::move(pictures_.front().frame);
    seconds = pictures_.front().seconds;
    pictures_.pop_front();
    changed_.notify_all();
    return true;
}

std::uint64_t PlatformVideo::take_sound(std::uint64_t from, std::uint64_t through, std::vector<std::int16_t>& out) {
    std::unique_lock lock(mutex_);
    if (through <= from) return 0;
    sound_wanted_ = std::max(sound_wanted_, through);
    changed_.notify_all();
    changed_.wait(lock, [&] { return sound_end_ >= through || sound_done_; });
    from = std::max(from, sound_base_);
    const std::uint64_t end = std::min(through, sound_end_);
    if (end <= from) return 0;
    const auto at = static_cast<std::ptrdiff_t>((from - sound_base_) * 2);
    const auto to = static_cast<std::ptrdiff_t>((end - sound_base_) * 2);
    out.insert(out.end(), sound_.begin() + at, sound_.begin() + to);
    sound_.erase(sound_.begin(), sound_.begin() + to);
    sound_base_ = end;
    return end - from;
}

std::string PlatformVideo::error() {
    std::lock_guard lock(mutex_);
    return error_;
}

// ------------------------------------------------------------------ ElementaryVideo

ElementaryVideo::ElementaryVideo(movie::PssMovie::Source source, std::size_t queue, std::size_t chunk)
    : source_(std::move(source)), queue_max_(std::max<std::size_t>(queue, 1)), chunk_(std::max<std::size_t>(chunk, 4096)) {
    worker_ = std::thread([this] { run(); });
}

ElementaryVideo::~ElementaryVideo() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    changed_.notify_all();
    worker_.join();
}

void ElementaryVideo::run() {
    try {
        std::vector<std::uint8_t> chunk(chunk_), head;
        mpeg2::Decoder decoder;
        bool have_info = false, stopped = false;
        while (!stopped) {
            {
                std::lock_guard lock(mutex_);
                if (stop_) break;
            }
            const std::size_t n = source_(std::span(chunk));
            if (!have_info) {
                head.insert(head.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
                const auto info = read_video_info(head);
                if (info || !n || head.size() >= (1u << 20)) {
                    have_info = true;
                    std::lock_guard lock(mutex_);
                    info_ = info;
                    info_done_ = true;
                    if (!info) error_ = "no MPEG-2 sequence header in the stream's first MiB";
                    changed_.notify_all();
                    if (!info) break;
                    head.clear();
                    head.shrink_to_fit();
                }
            }
            if (n) decoder.push(std::span(chunk).first(n));
            else decoder.finish();
            mpeg2::Frame frame;
            while (decoder.pop(frame)) {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop_ || frames_.size() < queue_max_; });
                if (stop_) {
                    stopped = true;
                    break;
                }
                frames_.push_back(std::move(frame));
                changed_.notify_all();
            }
            if (!n) break;
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        error_ = e.what();
    }
    std::lock_guard lock(mutex_);
    done_ = info_done_ = true;
    changed_.notify_all();
}

std::optional<VideoInfo> ElementaryVideo::info() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return info_done_; });
    return info_;
}

bool ElementaryVideo::pop(mpeg2::Frame& frame) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return !frames_.empty() || done_; });
    if (frames_.empty()) return false;
    frame = std::move(frames_.front());
    frames_.pop_front();
    changed_.notify_all();
    return true;
}

std::string ElementaryVideo::error() {
    std::lock_guard lock(mutex_);
    return error_;
}

// ------------------------------------------------------------------ ModMovie

ModMovie::ModMovie(const ModSet::MovieSource& source, const std::function<std::unique_ptr<movie::PssMovie>()>& disc)
    : gain_(source.gain) {
    if (!source.video.empty()) {
        video_name_ = source.video.filename().string();
        if (lower_extension(source.video) == ".pss") {
            pictures_ = std::make_unique<movie::PssMovie>(mod_file_source(source.video), 256 * 1024);
            pictures_->set_video_slice(2048);
            pictures_sound_ = source.audio.empty();
        } else if (movie::is_platform_movie_name(video_name_)) {
            const auto* backend = movie::platform_movie_backend();
            if (!backend)
                throw std::runtime_error("this platform has no " + lower_extension(source.video) +
                                         " movie decoder (research/MODDING.md \"Platform movie decoders\")");
            platform_ = std::make_unique<PlatformVideo>(*backend, mod_movie_file(source.video));
            const float a = platform_->info().display_aspect;
            aspect_ = a <= 0 || std::fabs(a - 4.0F / 3.0F) < 0.01F ? 0.0F : a;
            platform_sound_ = source.audio.empty() && platform_->has_sound();
        } else {
            elementary_ = std::make_unique<ElementaryVideo>(mod_file_source(source.video));
        }
    } else {
        pictures_ = disc ? disc() : nullptr;
        if (!pictures_) throw std::runtime_error("the disc has no movie " + source.target);
        gain_ = 1;   // pictures_sound_ stays false: the disc's sound is dropped for the mod's
    }
    if (!source.audio.empty()) {
        audio_name_ = source.audio.filename().string();
        sound_ = std::make_unique<FittedAudio>(read_file(source.audio), 48000, 2, std::nullopt, source.gain);
    } else if (elementary_ || (platform_ && !platform_sound_)) {
        disc_sound_ = disc ? disc() : nullptr;
    }
}

ModMovie::~ModMovie() = default;

bool ModMovie::next_frame(mpeg2::Frame& frame) {
    if (ended_) return false;
    const std::uint64_t pull = pulls_++;
    if (elementary_) {
        if (pull == 0) {
            if (!info_) info_ = elementary_->info();
            if (!info_) {
                note("movie picture " + video_name_ + ": " + elementary_->error() + "; the movie ends");
                ended_ = true;
                return false;
            }
            const float a = info_->display_aspect();
            aspect_ = std::fabs(a - 4.0F / 3.0F) < 0.01F ? 0.0F : a;
        }
        const auto want = static_cast<std::int64_t>(source_picture_at_pull(pull, info_->rate_num, info_->rate_den));
        while (current_index_ < want) {
            if (!elementary_->pop(current_)) {
                if (const auto e = elementary_->error(); !e.empty())
                    note("movie picture " + video_name_ + ": " + e + " after " + std::to_string(current_index_ + 1) +
                         " picture(s); the movie ends there");
                ended_ = true;
                return false;
            }
            ++current_index_;
        }
        frame = current_;
        return true;
    }
    if (platform_) {
        // The last picture whose time is <= the pull's (half a millisecond of slack for the container's rounding).
        const double now = static_cast<double>(pull) * 1001.0 / 30000.0 + 0.0005;
        if (pull == 0) {
            if (!platform_->pop(current_, current_seconds_)) {
                if (const auto e = platform_->error(); !e.empty()) note("movie picture " + video_name_ + ": " + e + "; the movie ends");
                ended_ = true;
                return false;
            }
            current_seconds_ = 0;
            current_index_ = 0;
        }
        for (;;) {
            if (!pending_ && !platform_end_) {
                if (platform_->pop(pending_frame_, pending_seconds_)) pending_ = true;
                else platform_end_ = true;
            }
            if (!pending_ || pending_seconds_ > now) break;
            interval_ = pending_seconds_ - current_seconds_;
            current_ = std::move(pending_frame_);
            current_seconds_ = pending_seconds_;
            ++current_index_;
            pending_ = false;
        }
        // The end: no picture after the last, once its time (the pictures' last interval; 1/30 s with one) is past.
        if (platform_end_ && !pending_ && now >= current_seconds_ + (interval_ > 0 ? interval_ : 1.0 / 30.0)) {
            if (const auto e = platform_->error(); !e.empty())
                note("movie picture " + video_name_ + ": " + e + " after " + std::to_string(current_index_ + 1) +
                     " picture(s); the movie ends there");
            ended_ = true;
            return false;
        }
        frame = current_;
        return true;
    }
    if (!pictures_->next_frame(frame)) {
        ended_ = true;
        return false;
    }
    return true;
}

bool ModMovie::audio_format(int& rate, int& channels) {
    if (pictures_sound_) return pictures_->audio_format(rate, channels);
    if (platform_sound_) {
        rate = 48000;
        channels = 2;
        return true;
    }
    if (disc_sound_) return disc_sound_->audio_format(rate, channels);
    if (!sound_) return false;
    rate = 48000;
    channels = 2;
    return true;
}

void ModMovie::take_audio(std::vector<std::int16_t>& out) {
    if (pictures_sound_) {
        const std::size_t at = out.size();
        pictures_->take_audio(out);
        if (gain_ != 1.0F)
            for (std::size_t i = at; i < out.size(); ++i)
                out[i] = static_cast<std::int16_t>(std::clamp(std::lround(out[i] * gain_), -32768L, 32767L));
        return;
    }
    if (pictures_) {   // the disc's picture under the mod's sound: its own sound is dropped
        std::vector<std::int16_t> dropped;
        pictures_->take_audio(dropped);
    }
    if (ended_ || pulls_ == 0) return;
    const std::uint64_t through = audio_frames_through_pull(pulls_ - 1);
    if (platform_sound_) {
        const std::size_t at = out.size();
        audio_given_ += platform_->take_sound(audio_given_, through, out);
        if (gain_ != 1.0F)
            for (std::size_t i = at; i < out.size(); ++i)
                out[i] = static_cast<std::int16_t>(std::clamp(std::lround(out[i] * gain_), -32768L, 32767L));
        return;
    }
    if (sound_) {
        const auto& clip = *sound_->clip();
        const std::uint64_t target = std::min<std::uint64_t>(through, clip.frames());
        if (target <= audio_given_) return;
        sound_->decode_to(static_cast<std::size_t>(target));
        out.insert(out.end(), clip.samples.begin() + static_cast<std::ptrdiff_t>(audio_given_ * 2),
                   clip.samples.begin() + static_cast<std::ptrdiff_t>(target * 2));
        audio_given_ = target;
        return;
    }
    if (!disc_sound_) return;
    int rate = 48000, channels = 2;
    while (audio_given_ + disc_pcm_.size() / std::size_t(channels) < through) {
        const bool more = disc_sound_->pump_audio_only();
        disc_sound_->take_audio(disc_pcm_);
        if (disc_sound_->audio_format(rate, channels) && channels <= 0) channels = 2;
        if (!more) break;
    }
    const std::uint64_t n = std::min<std::uint64_t>(through - std::min(through, audio_given_), disc_pcm_.size() / std::size_t(channels));
    out.insert(out.end(), disc_pcm_.begin(), disc_pcm_.begin() + static_cast<std::ptrdiff_t>(n * std::size_t(channels)));
    disc_pcm_.erase(disc_pcm_.begin(), disc_pcm_.begin() + static_cast<std::ptrdiff_t>(n * std::size_t(channels)));
    audio_given_ += n;
}

std::string ModMovie::describe() {
    std::string text = "picture ";
    if (elementary_) {
        if (!info_) info_ = elementary_->info();
        text += video_name_;
        if (info_) {
            char fps[32];
            std::snprintf(fps, sizeof fps, "%.3f", static_cast<double>(info_->rate_num) / info_->rate_den);
            text += " " + std::to_string(info_->width) + "x" + std::to_string(info_->height) + " " + fps + " fps " +
                    aspect_text(info_->display_aspect());
        } else {
            text += " (" + elementary_->error() + ")";
        }
    } else if (platform_) {
        const auto& i = platform_->info();
        text += video_name_ + " " + std::to_string(i.width) + "x" + std::to_string(i.height) + " " + i.video_codec;
        if (i.frame_rate > 0) {
            char fps[32];
            std::snprintf(fps, sizeof fps, " %.3f fps", i.frame_rate);
            text += fps;
        }
        text += " " + aspect_text(i.display_aspect > 0 ? i.display_aspect : 4.0F / 3.0F) + " (" + platform_->backend_name() + ")";
    } else {
        text += video_name_.empty() ? "the disc's" : video_name_ + " (PSS)";
    }
    text += ", sound ";
    if (sound_) text += audio_name_ + (gain_ != 1.0F ? " at " + std::to_string(std::lround(gain_ * 100)) + "%" : "");
    else if (pictures_sound_) text += video_name_ + "'s own";
    else if (platform_sound_)
        text += video_name_ + "'s own (" + platform_->info().audio_codec + " " + std::to_string(platform_->info().audio_rate) + " Hz x" +
                std::to_string(platform_->info().audio_channels) + ")" +
                (gain_ != 1.0F ? " at " + std::to_string(std::lround(gain_ * 100)) + "%" : "");
    else text += disc_sound_ ? "the disc's" : "none";
    return text;
}

}  // namespace omega::mods
