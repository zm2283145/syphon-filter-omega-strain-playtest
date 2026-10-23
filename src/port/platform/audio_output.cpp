#include "audio_output.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega::audio {

std::pair<float, float> pan_gains(std::int32_t volume, std::int32_t pan) {
    const float v = std::clamp(static_cast<float>(volume) / 1024.0F, 0.0F, 1.0F);
    if (pan == -1) return {v, v};
    constexpr double pi = 3.14159265358979323846;
    const double s = std::sin(static_cast<double>(pan) * pi / 180.0);
    const double phi = (s + 1.0) * pi / 4.0;
    const auto side = [](double g) { return static_cast<float>(std::min(1.0, g * std::sqrt(2.0))); };
    return {v * side(std::cos(phi)), v * side(std::sin(phi))};
}

Mixer::Voice Mixer::start(std::shared_ptr<const PcmClip> clip, float left, float right, bool loop) {
    if (!clip || !clip->rate || clip->channels < 1 || clip->channels > 2)
        throw std::runtime_error("Mixer: only mono or stereo clips are mixed");
    if (loop && clip->frames() == 0) throw std::runtime_error("Mixer: an empty clip cannot loop");
    State s;
    s.step = (std::uint64_t(clip->rate) << 32) / output_rate;
    s.clip = std::move(clip);
    s.gain[0][0] = left;
    s.gain[0][1] = s.clip->channels == 2 ? 0.0F : right;
    s.gain[1][1] = s.clip->channels == 2 ? right : 0.0F;
    s.loop = loop;
    const auto id = next_++;
    voices_.emplace(id, std::move(s));
    return id;
}

void Mixer::set_gains(Voice voice, float left, float right) {
    const auto found = voices_.find(voice);
    if (found == voices_.end()) return;
    auto& s = found->second;
    s.gain[0][0] = left;
    s.gain[0][1] = s.clip->channels == 2 ? 0.0F : right;
    s.gain[1][0] = 0.0F;
    s.gain[1][1] = s.clip->channels == 2 ? right : 0.0F;
}

std::array<std::array<float, 2>, 2> Mixer::channel_gains(Voice voice) const {
    std::array<std::array<float, 2>, 2> g{};
    const auto found = voices_.find(voice);
    if (found != voices_.end())
        for (std::size_t c = 0; c < 2; ++c) g[c] = {found->second.gain[c][0], found->second.gain[c][1]};
    return g;
}

void Mixer::set_channel_gains(Voice voice, std::size_t channel, float left, float right) {
    const auto found = voices_.find(voice);
    if (found == voices_.end()) return;
    if (channel >= found->second.clip->channels) throw std::runtime_error("Mixer: no such source channel");
    found->second.gain[channel][0] = left;
    found->second.gain[channel][1] = right;
}

void Mixer::pause(Voice voice, bool paused) {
    const auto found = voices_.find(voice);
    if (found != voices_.end()) found->second.paused = paused;
}

void Mixer::stop(Voice voice) { voices_.erase(voice); }

std::size_t Mixer::frames_read_by(const PcmClip* clip, std::size_t frames) const {
    std::size_t n = 0;
    for (const auto& [voice, s] : voices_) {
        if (s.clip.get() != clip || s.paused) continue;
        // mix() reads frames i and i + 1 for positions up to the last step's.
        n = std::max<std::size_t>(n, static_cast<std::size_t>((s.position + std::uint64_t(frames) * s.step) >> 32) + 2);
    }
    return n;
}

std::vector<Mixer::Voice> Mixer::mix(std::size_t frames, std::vector<std::int16_t>& out) {
    std::vector<float> acc(frames * 2, 0.0F);
    std::vector<Voice> ended;
    for (auto it = voices_.begin(); it != voices_.end();) {
        auto& s = it->second;
        if (s.paused) {
            ++it;
            continue;
        }
        const auto& clip = *s.clip;
        const std::size_t count = clip.frames();
        bool done = false;
        for (std::size_t f = 0; f < frames; ++f) {
            const std::size_t i = static_cast<std::size_t>(s.position >> 32);
            if (i >= count) {
                if (!s.loop) {
                    done = true;
                    break;
                }
                s.position -= std::uint64_t(count) << 32;
                --f;
                continue;
            }
            const float t = static_cast<float>(s.position & 0xffffffffULL) / 4294967296.0F;
            const auto sample = [&](std::size_t frame, std::size_t ch) {
                if (frame >= count) frame = s.loop ? frame - count : count - 1;
                return static_cast<float>(clip.samples[frame * clip.channels + ch]);
            };
            float l = sample(i, 0) + (sample(i + 1, 0) - sample(i, 0)) * t;
            float r = 0.0F;
            if (clip.channels == 2) r = sample(i, 1) + (sample(i + 1, 1) - sample(i, 1)) * t;
            acc[2 * f] += l * s.gain[0][0] + r * s.gain[1][0];
            acc[2 * f + 1] += l * s.gain[0][1] + r * s.gain[1][1];
            s.position += s.step;
        }
        if (done || (!s.loop && (s.position >> 32) >= count)) {
            ended.push_back(it->first);
            it = voices_.erase(it);
        } else {
            ++it;
        }
    }
    out.reserve(out.size() + acc.size());
    for (const float v : acc)
        out.push_back(static_cast<std::int16_t>(std::lround(std::clamp(v, -32768.0F, 32767.0F))));
    return ended;
}

PcmRing::PcmRing(std::size_t capacity_frames, std::size_t prime_frames)
    : data_(capacity_frames * 2), capacity_(capacity_frames), prime_(prime_frames) {
    if (prime_ == 0 || prime_ > capacity_) throw std::runtime_error("PcmRing: priming must be 1..capacity frames");
}

std::size_t PcmRing::write(const std::int16_t* samples, std::size_t frames) {
    const auto n = std::min(frames, space());
    auto tail = (head_ + queued_) % capacity_;
    for (std::size_t i = 0; i < n; ++i) {
        data_[tail * 2] = samples[i * 2];
        data_[tail * 2 + 1] = samples[i * 2 + 1];
        tail = tail + 1 == capacity_ ? 0 : tail + 1;
    }
    queued_ += n;
    return n;
}

void PcmRing::read(std::int16_t* out, std::size_t frames) {
    if (!playing_) {
        if (queued_ < prime_) {
            std::fill(out, out + frames * 2, std::int16_t{0});
            if (started_) silent_ += frames;
            return;
        }
        playing_ = started_ = true;
    }
    const auto n = std::min(frames, queued_);
    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2] = data_[head_ * 2];
        out[i * 2 + 1] = data_[head_ * 2 + 1];
        head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
    }
    queued_ -= n;
    if (n < frames) {
        std::fill(out + n * 2, out + frames * 2, std::int16_t{0});
        ++underruns_;
        silent_ += frames - n;
        playing_ = false;
    }
}

std::vector<AudioDeviceEvent> parse_audio_device_events(std::string_view list) {
    std::vector<AudioDeviceEvent> out;
    std::size_t at = 0;
    while (at < list.size()) {
        const auto end = std::min(list.find(',', at), list.size());
        std::string item(list.substr(at, end - at));
        at = end + 1;
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (item.empty()) continue;
        const auto colon = item.find(':');
        const auto bad = [&] { return std::runtime_error("--test-audio-device: bad event '" + item + "' (F:lost|gone|back|switch, 0:absent first)"); };
        if (colon == 0 || colon == std::string::npos ||
            item.find_first_not_of("0123456789") != colon)
            throw bad();
        AudioDeviceEvent e;
        e.frame = std::stoull(item.substr(0, colon));
        const std::string kind = item.substr(colon + 1);
        if (kind == "lost") e.kind = AudioDeviceEvent::Kind::lost;
        else if (kind == "gone") e.kind = AudioDeviceEvent::Kind::gone;
        else if (kind == "back") e.kind = AudioDeviceEvent::Kind::back;
        else if (kind == "switch") e.kind = AudioDeviceEvent::Kind::default_switch;
        else if (kind == "absent" && e.frame == 0 && out.empty()) e.kind = AudioDeviceEvent::Kind::absent;
        else throw bad();
        if (!out.empty() && e.frame < out.back().frame) throw std::runtime_error("--test-audio-device: frames out of order at '" + item + "'");
        out.push_back(e);
    }
    return out;
}

namespace {

void put32(std::ofstream& f, std::uint32_t v) {
    const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
    f.write(b, 4);
}

void put16(std::ofstream& f, std::uint16_t v) {
    const char b[2] = {char(v), char(v >> 8)};
    f.write(b, 2);
}

}

WavWriter::WavWriter(const std::string& path, std::uint32_t rate, std::uint16_t channels)
    : file_(path, std::ios::binary | std::ios::trunc), rate_(rate), channels_(channels) {
    if (!file_) throw std::runtime_error("Cannot write WAV " + path);
    file_.write("RIFF", 4);
    put32(file_, 0);
    file_.write("WAVEfmt ", 8);
    put32(file_, 16);
    put16(file_, 1);
    put16(file_, channels_);
    put32(file_, rate_);
    put32(file_, rate_ * channels_ * 2U);
    put16(file_, static_cast<std::uint16_t>(channels_ * 2U));
    put16(file_, 16);
    file_.write("data", 4);
    put32(file_, 0);
    open_ = true;
}

WavWriter::~WavWriter() {
    try {
        close();
    } catch (...) {
    }
}

void WavWriter::write(const std::vector<std::int16_t>& samples) {
    if (!open_) throw std::runtime_error("WAV writer is closed");
    for (const auto s : samples) put16(file_, static_cast<std::uint16_t>(s));
    bytes_ += samples.size() * 2;
}

void WavWriter::close() {
    if (!open_) return;
    open_ = false;
    if (bytes_ > 0xffffffffULL - 36) throw std::runtime_error("WAV data over 4 GB");
    file_.seekp(4);
    put32(file_, static_cast<std::uint32_t>(36 + bytes_));
    file_.seekp(40);
    put32(file_, static_cast<std::uint32_t>(bytes_));
    file_.close();
}

}
