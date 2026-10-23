#include "movie_pss.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace omega::movie {
namespace {

std::uint64_t timestamp(const std::uint8_t* b) {
    if (!((b[0] & 1) && (b[2] & 1) && (b[4] & 1))) throw std::runtime_error("PSS: PTS marker bits");
    return (std::uint64_t((b[0] >> 1) & 7) << 30) | (std::uint64_t(b[1]) << 22) | (std::uint64_t(b[2] >> 1) << 15) |
           (std::uint64_t(b[3]) << 7) | std::uint64_t(b[4] >> 1);
}

std::int32_t le32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
                                     std::uint32_t(p[3]) << 24);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

}  // namespace

// ----------------------------------------------------------------- demuxer
void PssDemuxer::push(std::span<const std::uint8_t> bytes) {
    if (stats_.end_code) {
        for (const auto b : bytes)
            if (b) throw std::runtime_error("PSS: non-zero bytes after the program end code");
        stats_.trailing_bytes += bytes.size();
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    while (!stats_.end_code && parse_one()) {
    }
    if (stats_.end_code) {
        for (std::size_t i = pos_; i < buffer_.size(); ++i)
            if (buffer_[i]) throw std::runtime_error("PSS: non-zero bytes after the program end code");
        stats_.trailing_bytes += buffer_.size() - pos_;
        buffer_.clear();
        pos_ = 0;
        return;
    }
    if (pos_ > 65536) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
        consumed_ += pos_;
        pos_ = 0;
    }
}

bool PssDemuxer::parse_one() {
    const std::size_t avail = buffer_.size() - pos_;
    if (avail < 4) return false;
    const std::uint8_t* p = buffer_.data() + pos_;
    if (p[0] || p[1] || p[2] != 1)
        throw std::runtime_error("PSS: desync at byte " + std::to_string(consumed_ + pos_));
    const std::uint8_t sid = p[3];
    if (sid == 0xBA) {
        if (avail < 14) return false;
        const std::uint8_t* b = p + 4;
        if ((b[0] >> 6) != 1) throw std::runtime_error("PSS: not an MPEG-2 pack header");
        const std::size_t length = 14 + (b[9] & 7);
        if (avail < length) return false;
        const std::uint64_t scr = (std::uint64_t((b[0] >> 3) & 7) << 30) | (std::uint64_t(b[0] & 3) << 28) |
                                  (std::uint64_t(b[1]) << 20) | (std::uint64_t(b[2] >> 3) << 15) |
                                  (std::uint64_t(b[2] & 3) << 13) | (std::uint64_t(b[3]) << 5) | (b[4] >> 3);
        if (!stats_.first_scr) stats_.first_scr = scr;
        stats_.last_scr = scr;
        stats_.mux_rate = ((std::uint64_t(b[6]) << 14) | (std::uint64_t(b[7]) << 6) | (b[8] >> 2)) * 50;
        ++stats_.packs;
        pos_ += length;
        return true;
    }
    if (sid == 0xB9) {
        stats_.end_code = true;
        pos_ += 4;
        return false;
    }
    if (avail < 6) return false;
    const std::size_t length = (std::size_t(p[4]) << 8) | p[5];
    if (avail < 6 + length) return false;
    if (sid == 0xBB) {
        ++stats_.system_headers;
    } else if (sid == 0xBE) {
        ++stats_.padding_pes;
        stats_.padding_bytes += length;
    } else if (sid == 0xE0 || sid == 0xBD) {
        if (length < 3 || (p[6] >> 6) != 2) throw std::runtime_error("PSS: not an MPEG-2 PES header");
        const std::uint8_t flags = p[7];
        const std::size_t header = p[8];
        if (9 + header > 6 + length) throw std::runtime_error("PSS: PES header longer than the packet");
        std::optional<std::uint64_t> pts;
        if (flags & 0x80) {
            if (header < 5) throw std::runtime_error("PSS: PES PTS outside the header");
            pts = timestamp(p + 9);
        }
        const std::uint8_t* payload = p + 9 + header;
        const std::size_t payload_size = 6 + length - (9 + header);
        if (sid == 0xE0) {
            ++stats_.video_pes;
            if (pts) {
                ++stats_.video_pts;
                if (!stats_.first_video_pts) stats_.first_video_pts = pts;
            }
            video_.insert(video_.end(), payload, payload + payload_size);
            stats_.video_bytes += payload_size;
        } else {
            static constexpr std::uint8_t sub[4]{0xff, 0xa0, 0x00, 0x00};
            if (payload_size < 4 || std::memcmp(payload, sub, 4) != 0)
                throw std::runtime_error("PSS: unexpected audio sub-header");
            ++stats_.audio_pes;
            if (pts) {
                ++stats_.audio_pts;
                if (!stats_.first_audio_pts) stats_.first_audio_pts = pts;
            }
            audio_.insert(audio_.end(), payload + 4, payload + payload_size);
            stats_.audio_bytes += payload_size - 4;
        }
    } else {
        throw std::runtime_error("PSS: unexpected stream id " + std::to_string(sid));
    }
    pos_ += 6 + length;
    return true;
}

void PssDemuxer::finish() {
    if (!stats_.end_code) throw std::runtime_error("PSS: no program end code");
}

// ----------------------------------------------------------------- audio
void deinterleave_blocks(std::span<const std::uint8_t> blocks, int channels, int interleave,
                         std::vector<std::int16_t>& out) {
    if (channels <= 0 || interleave <= 0 || interleave % 2) throw std::runtime_error("PSS audio: bad block layout");
    const std::size_t block = std::size_t(interleave) * channels;
    if (blocks.size() % block) throw std::runtime_error("PSS audio: partial block");
    const int per = interleave / 2;
    out.reserve(out.size() + blocks.size() / 2);
    for (std::size_t base = 0; base < blocks.size(); base += block)
        for (int s = 0; s < per; ++s)
            for (int c = 0; c < channels; ++c) {
                const std::uint8_t* q = blocks.data() + base + std::size_t(c) * interleave + std::size_t(s) * 2;
                out.push_back(static_cast<std::int16_t>(std::uint16_t(q[0]) | std::uint16_t(q[1]) << 8));
            }
}

const PssAudioHeader& PssAudio::header() const {
    if (!header_) throw std::runtime_error("PSS audio: no SShd header yet");
    return *header_;
}

void PssAudio::push(std::span<const std::uint8_t> bytes) {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    if (!header_) {
        if (pending_.size() < 40) return;
        const std::uint8_t* p = pending_.data();
        if (std::memcmp(p, "SShd", 4) != 0) throw std::runtime_error("PSS audio: missing SShd");
        PssAudioHeader h;
        h.size = le32(p + 4);
        h.type = le32(p + 8);
        h.rate = le32(p + 12);
        h.channels = le32(p + 16);
        h.interleave = le32(p + 20);
        h.loop_start = le32(p + 24);
        h.loop_end = le32(p + 28);
        if (h.size != 0x18 || h.type != 1 || h.channels != 2)
            throw std::runtime_error("PSS audio: unsupported SShd (size/type/channels)");
        if (h.interleave <= 0 || h.interleave % 2) throw std::runtime_error("PSS audio: bad interleave");
        if (std::memcmp(p + 32, "SSbd", 4) != 0) throw std::runtime_error("PSS audio: missing SSbd");
        h.body_size = static_cast<std::uint32_t>(le32(p + 36));
        header_ = h;
        pending_.erase(pending_.begin(), pending_.begin() + 40);
        body_bytes_ = pending_.size();
    } else {
        body_bytes_ += bytes.size();
    }
    const std::size_t block = std::size_t(header_->interleave) * header_->channels;
    const std::size_t whole = pending_.size() / block * block;
    if (!whole) return;
    deinterleave_blocks(std::span(pending_).first(whole), header_->channels, header_->interleave, samples_);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(whole));
}

std::vector<std::uint8_t> wav_header(int rate, int channels, std::uint32_t data_bytes) {
    std::vector<std::uint8_t> h;
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put32(h, 36 + data_bytes);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, 16);
    put16(h, 1);
    put16(h, static_cast<std::uint16_t>(channels));
    put32(h, static_cast<std::uint32_t>(rate));
    put32(h, static_cast<std::uint32_t>(rate * channels * 2));
    put16(h, static_cast<std::uint16_t>(channels * 2));
    put16(h, 16);
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put32(h, data_bytes);
    return h;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const auto b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t frame_crc32(const mpeg2::Frame& frame) {
    std::uint32_t c = crc32(frame.y);
    c = crc32(frame.u, c);
    return crc32(frame.v, c);
}

std::vector<std::uint8_t> frame_to_rgb(const mpeg2::Frame& frame) {
    const int w = frame.width, h = frame.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (frame.y.size() != std::size_t(w) * h || frame.u.size() != std::size_t(cw) * ch ||
        frame.v.size() != frame.u.size())
        throw std::runtime_error("frame_to_rgb: plane sizes");
    std::vector<std::uint8_t> rgb(std::size_t(w) * h * 3);
    std::vector<int> row_u(cw), row_v(cw);
    const auto clamp = [](int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int y = 0; y < h; ++y) {
        // Vertical: chroma rows sit between luma rows 2r and 2r+1 (weights 3/4, 1/4), x4 scale.
        const int r = y / 2;
        const int other = (y & 1) ? std::min(r + 1, ch - 1) : std::max(r - 1, 0);
        for (int c = 0; c < cw; ++c) {
            row_u[c] = 3 * frame.u[std::size_t(r) * cw + c] + frame.u[std::size_t(other) * cw + c];
            row_v[c] = 3 * frame.v[std::size_t(r) * cw + c] + frame.v[std::size_t(other) * cw + c];
        }
        for (int x = 0; x < w; ++x) {
            // Horizontal: co-sited on even luma columns, x2 scale; total x8.
            const int c = x / 2, c1 = (x & 1) ? std::min(c + 1, cw - 1) : c;
            const int u = (row_u[c] + row_u[c1] + 4) / 8 - 128;
            const int v = (row_v[c] + row_v[c1] + 4) / 8 - 128;
            const int l = 76309 * (frame.y[std::size_t(y) * w + x] - 16);
            std::uint8_t* out = rgb.data() + (std::size_t(y) * w + x) * 3;
            out[0] = clamp((l + 104597 * v + 32768) >> 16);
            out[1] = clamp((l - 25675 * u - 53279 * v + 32768) >> 16);
            out[2] = clamp((l + 132201 * u + 32768) >> 16);
        }
    }
    return rgb;
}

void frame_to_rgba(const mpeg2::Frame& frame, std::vector<std::uint8_t>& out) {
    out.resize(std::size_t(std::max(frame.width, 0)) * std::max(frame.height, 0) * 4);
    frame_to_rgba_rows(frame, out.data(), 0, frame.height);
}

void frame_to_rgba_rows(const mpeg2::Frame& frame, std::uint8_t* out_pixels, int y0, int y1) {
    const int w = frame.width, h = frame.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (frame.y.size() != std::size_t(w) * h || frame.u.size() != std::size_t(cw) * ch ||
        frame.v.size() != frame.u.size())
        throw std::runtime_error("frame_to_rgba: plane sizes");
    if (y0 < 0 || y1 > h || y0 > y1) throw std::runtime_error("frame_to_rgba: rows");
    // frame_to_rgb's arithmetic with the chroma interpolated once per row into per-pixel arrays.
    // (Room past the end for the NEON passes' whole vectors.)
    std::vector<std::int16_t> px_u(std::size_t(w) + 32), px_v(std::size_t(w) + 32);
#if defined(__ARM_NEON)
    std::vector<std::uint16_t> row16_(std::size_t(cw) + 16);
#else
    std::vector<int> row_u(cw), row_v(cw);
#endif
    const auto clamp = [](int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int y = y0; y < y1; ++y) {
        const int r = y / 2;
        const int other = (y & 1) ? std::min(r + 1, ch - 1) : std::max(r - 1, 0);
#if defined(__ARM_NEON)
        // Port-only (132vt2a): the chroma interpolation on NEON as well, the same integers: the vertical 3:1 sum per
        // chroma column (row16, the last column repeated once past the end: c1's clamp), then even pixels
        // (2 * a + 4) >> 3 and odd ones (a + a_next + 4) >> 3, less 128, interleaved (all sums are non-negative).
        for (int plane = 0; plane < 2; ++plane) {
            const std::uint8_t* src = (plane ? frame.v : frame.u).data();
            const std::uint8_t* near_row = src + std::size_t(r) * cw;
            const std::uint8_t* far_row = src + std::size_t(other) * cw;
            std::uint16_t* row16 = row16_.data();
            int c = 0;
            for (; c + 8 <= cw; c += 8) {
                const uint16x8_t n = vmovl_u8(vld1_u8(near_row + c));
                vst1q_u16(row16 + c, vaddq_u16(vaddq_u16(n, vshlq_n_u16(n, 1)), vmovl_u8(vld1_u8(far_row + c))));
            }
            for (; c < cw; ++c) row16[c] = static_cast<std::uint16_t>(3 * near_row[c] + far_row[c]);
            for (int k = cw; k < cw + 8; ++k) row16[k] = row16[cw - 1];
            std::int16_t* px = (plane ? px_v : px_u).data();
            const int16x8_t bias = vdupq_n_s16(128);
            const uint16x8_t four = vdupq_n_u16(4);
            for (c = 0; c < cw; c += 8) {
                const uint16x8_t a = vld1q_u16(row16 + c), b = vld1q_u16(row16 + c + 1);
                const int16x8_t even = vsubq_s16(vreinterpretq_s16_u16(vshrq_n_u16(vaddq_u16(vshlq_n_u16(a, 1), four), 3)), bias);
                const int16x8_t odd = vsubq_s16(vreinterpretq_s16_u16(vshrq_n_u16(vaddq_u16(vaddq_u16(a, b), four), 3)), bias);
                const int16x8x2_t both = vzipq_s16(even, odd);
                vst1q_s16(px + 2 * c, both.val[0]);
                vst1q_s16(px + 2 * c + 8, both.val[1]);
            }
        }
#else
        for (int c = 0; c < cw; ++c) {
            row_u[c] = 3 * frame.u[std::size_t(r) * cw + c] + frame.u[std::size_t(other) * cw + c];
            row_v[c] = 3 * frame.v[std::size_t(r) * cw + c] + frame.v[std::size_t(other) * cw + c];
        }
        for (int x = 0; x < w; ++x) {
            const int c = x / 2, c1 = (x & 1) ? std::min(c + 1, cw - 1) : c;
            px_u[x] = static_cast<std::int16_t>((row_u[c] + row_u[c1] + 4) / 8 - 128);
            px_v[x] = static_cast<std::int16_t>((row_v[c] + row_v[c1] + 4) / 8 - 128);
        }
#endif
        const std::uint8_t* luma = frame.y.data() + std::size_t(y) * w;
        std::uint8_t* dst = out_pixels + std::size_t(y) * w * 4;
        int x = 0;
#if defined(__ARM_NEON)
        // (x + 0x8000) >> 16 clamped to 0..255: the add, the arithmetic shift, then two saturating narrows (plain
        // VADD / VSHR / VQMOVUN rather than VQRSHRUN, which Vita3K's CPU recompiler aborts on).
        const int16x8_t sixteen = vdupq_n_s16(16);
        const uint8x8_t opaque = vdup_n_u8(255);
        const int32x4_t half = vdupq_n_s32(0x8000);
        for (; x + 8 <= w; x += 8) {
            const int16x8_t l16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(luma + x))), sixteen);
            const int16x8_t u16 = vld1q_s16(px_u.data() + x), v16 = vld1q_s16(px_v.data() + x);
            uint8x8x4_t px;
            const int32x4_t l_lo = vmulq_n_s32(vmovl_s16(vget_low_s16(l16)), 76309);
            const int32x4_t l_hi = vmulq_n_s32(vmovl_s16(vget_high_s16(l16)), 76309);
            const int32x4_t u_lo = vmovl_s16(vget_low_s16(u16)), u_hi = vmovl_s16(vget_high_s16(u16));
            const int32x4_t v_lo = vmovl_s16(vget_low_s16(v16)), v_hi = vmovl_s16(vget_high_s16(v16));
            const auto narrow = [half](int32x4_t lo, int32x4_t hi) {
                return vqmovn_u16(vcombine_u16(vqmovun_s32(vshrq_n_s32(vaddq_s32(lo, half), 16)),
                                               vqmovun_s32(vshrq_n_s32(vaddq_s32(hi, half), 16))));
            };
            px.val[0] = narrow(vmlaq_n_s32(l_lo, v_lo, 104597), vmlaq_n_s32(l_hi, v_hi, 104597));
            px.val[1] = narrow(vmlaq_n_s32(vmlaq_n_s32(l_lo, u_lo, -25675), v_lo, -53279),
                               vmlaq_n_s32(vmlaq_n_s32(l_hi, u_hi, -25675), v_hi, -53279));
            px.val[2] = narrow(vmlaq_n_s32(l_lo, u_lo, 132201), vmlaq_n_s32(l_hi, u_hi, 132201));
            px.val[3] = opaque;
            vst4_u8(dst + 4 * x, px);
        }
#endif
        for (; x < w; ++x) {
            const int u = px_u[x], v = px_v[x];
            const int l = 76309 * (luma[x] - 16);
            std::uint8_t* o = dst + 4 * x;
            o[0] = clamp((l + 104597 * v + 32768) >> 16);
            o[1] = clamp((l - 25675 * u - 53279 * v + 32768) >> 16);
            o[2] = clamp((l + 132201 * u + 32768) >> 16);
            o[3] = 255;
        }
    }
}

std::optional<std::string> locate_game_iso(const std::string& dir) {
    std::string env;
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "OMEGA_ISO") == 0 && value) {
        env = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv("OMEGA_ISO")) env = value;
#endif
    if (!env.empty()) {
        if (std::filesystem::is_regular_file(env)) return env;
        return std::nullopt;
    }
    std::error_code ec;
    for (const auto& sub : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!sub.is_directory(ec2)) continue;
        for (const auto& file : std::filesystem::directory_iterator(sub.path(), ec2)) {
            auto ext = file.path().extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext == ".iso" && file.is_regular_file(ec2)) return file.path().string();
        }
    }
    return std::nullopt;
}

PssMovie::Source stream_source(std::istream& stream, std::uint64_t offset, std::uint64_t size) {
    return [&stream, offset, size, done = std::uint64_t{0}](std::span<std::uint8_t> out) mutable -> std::size_t {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size - done));
        if (!n) return 0;
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset + done));
        stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(stream.gcount()) != n) throw std::runtime_error("PSS: short read");
        done += n;
        return n;
    };
}

PssMovie::Source memory_source(std::span<const std::uint8_t> bytes) {
    return [bytes, done = std::size_t{0}](std::span<std::uint8_t> out) mutable -> std::size_t {
        const std::size_t n = std::min(out.size(), bytes.size() - done);
        std::memcpy(out.data(), bytes.data() + done, n);
        done += n;
        return n;
    };
}

// ----------------------------------------------------------------- movie
PssMovie::PssMovie(Source source, std::size_t chunk) : source_(std::move(source)), chunk_(chunk) {
    if (!chunk) throw std::runtime_error("PSS: zero read chunk");
}

bool PssMovie::pump() {
    if (source_done_) return false;
    const std::size_t got = source_(std::span(chunk_));
    if (got) demuxer_.push(std::span(chunk_).first(got));
    if (decode_video_ && video_slice_) {  // port-only (132win5): pushed by next_frame, a slice at a time
        if (video_pushed_ >= video_pending_.size()) {
            video_pending_.clear();
            video_pushed_ = 0;
        } else if (video_pushed_ >= (std::size_t{1} << 20)) {  // read ahead (132vt2a2): drop the pushed bytes
            video_pending_.erase(video_pending_.begin(), video_pending_.begin() + std::ptrdiff_t(video_pushed_));
            video_pushed_ = 0;
        }
        video_pending_.insert(video_pending_.end(), demuxer_.video().begin(), demuxer_.video().end());
    } else if (decode_video_) {
        decoder_.push(demuxer_.video());
    }
    demuxer_.video().clear();
    audio_.push(demuxer_.audio());
    demuxer_.audio().clear();
    if (!got || demuxer_.ended()) {
        // Read (and check) the zero tail after the end code, then close.
        while (got && demuxer_.ended()) {
            const std::size_t more = source_(std::span(chunk_));
            if (!more) break;
            demuxer_.push(std::span(chunk_).first(more));
        }
        demuxer_.finish();
        if (decode_video_ && video_slice_) finish_pending_ = true;
        else if (decode_video_) decoder_.finish();
        source_done_ = true;
    }
    return true;
}

bool PssMovie::next_frame(mpeg2::Frame& frame) {
    while (!decoder_.pop(frame)) {
        if (video_slice_ && video_pushed_ < video_pending_.size()) {
            const std::size_t n = std::min(video_slice_, video_pending_.size() - video_pushed_);
            decoder_.push(std::span(video_pending_).subspan(video_pushed_, n));
            video_pushed_ += n;
            continue;
        }
        if (finish_pending_) {
            finish_pending_ = false;
            decoder_.finish();
            continue;
        }
        if (source_done_) return false;
        pump();
    }
    return true;
}

std::uint64_t PssMovie::pictures_decoded() const {
    const auto& s = decoder_.stats();
    return s.pictures_i + s.pictures_p + s.pictures_b;
}

bool PssMovie::audio_format(int& rate, int& channels) {
    if (!audio_.has_header()) return false;
    rate = audio_.header().rate;
    channels = audio_.header().channels;
    return true;
}

void PssMovie::take_audio(std::vector<std::int16_t>& out) {
    auto& s = audio_.samples();
    out.insert(out.end(), s.begin(), s.end());
    s.clear();
}

bool PssMovie::pump_audio_only() {
    decode_video_ = false;
    return pump();
}

bool PssMovie::read_ahead() {
    if (source_done_ || !decode_video_ || !video_slice_) return false;
    if (video_pending_.size() - video_pushed_ >= read_ahead_video_limit) return false;
    return pump();
}

void PssMovie::drain_audio() {
    decode_video_ = false;
    while (!source_done_) pump();
}

}  // namespace omega::movie
