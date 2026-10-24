#include "audio_formats.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace omega::audio {
namespace {

constexpr std::array<std::int32_t, 5> filter0{0, 60, 115, 98, 122};
constexpr std::array<std::int32_t, 5> filter1{0, 0, -52, -55, -60};

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("Audio header outside the file");
    return std::uint32_t(b[at]) << 24 | std::uint32_t(b[at + 1]) << 16 | std::uint32_t(b[at + 2]) << 8 | b[at + 3];
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("Audio header outside the file");
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 |
           std::uint32_t(b[at + 3]) << 24;
}

bool padding_frame(std::span<const std::uint8_t> f) {
    return f[0] == 0xc0 && std::all_of(f.begin() + 1, f.end(), [](std::uint8_t v) { return v == 0; });
}

}

std::uint8_t decode_adpcm_frame(std::span<const std::uint8_t> frame, AdpcmState& state, std::int16_t* out) {
    if (frame.size() != 16) throw std::runtime_error("PS-ADPCM frame is not 16 bytes");
    std::uint32_t filter = frame[0] >> 4;
    const std::uint32_t shift = frame[0] & 15;
    if (padding_frame(frame)) filter = 0;
    if (filter > 4 || shift > 12) {
        char text[64];
        std::snprintf(text, sizeof text, "PS-ADPCM frame with filter %u shift %u", filter, shift);
        throw std::runtime_error(text);
    }
    for (int i = 0; i < 28; ++i) {
        const std::uint8_t byte = frame[std::size_t(2 + i / 2)];
        const std::int32_t nibble = (i & 1) ? byte >> 4 : byte & 15;
        const std::int32_t d = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
        std::int32_t s = d + ((state.s1 * filter0[filter] + state.s2 * filter1[filter] + 32) >> 6);
        s = std::clamp(s, -32768, 32767);
        state.s2 = state.s1;
        state.s1 = s;
        out[i] = static_cast<std::int16_t>(s);
    }
    return frame[1];
}

bool is_vag(std::span<const std::uint8_t> b) {
    return b.size() >= 4 && b[0] == 'V' && b[1] == 'A' && b[2] == 'G' && b[3] == 'p';
}

bool is_vpk(std::span<const std::uint8_t> b) {
    return b.size() >= 4 && b[0] == ' ' && b[1] == 'K' && b[2] == 'P' && b[3] == 'V';
}

namespace {

VagHeader vag_header(std::span<const std::uint8_t> b, std::uint64_t file_size) {
    if (!is_vag(b) || b.size() < 0x30) throw std::runtime_error("Not a VAGp file");
    VagHeader h;
    h.version = be32(b, 4);
    h.data_size = be32(b, 0xc);
    h.rate = be32(b, 0x10);
    for (std::size_t i = 0x20; i < 0x30 && b[i]; ++i) h.name.push_back(static_cast<char>(b[i]));
    if (h.data_size % 16 || std::uint64_t(h.data_size) > file_size - 0x30)
        throw std::runtime_error("VAG data outside the file");
    if (!h.rate) throw std::runtime_error("VAG without a rate");
    return h;
}

}  // namespace

VagHeader parse_vag(std::span<const std::uint8_t> b) { return vag_header(b, b.size()); }

VagHeader parse_vag_header(std::span<const std::uint8_t> b) { return vag_header(b, ~std::uint64_t(0)); }

namespace {

VpkHeader vpk_header(std::span<const std::uint8_t> b, std::uint64_t file_size) {
    if (!is_vpk(b) || b.size() < 0x20) throw std::runtime_error("Not a VPK file");
    VpkHeader h{le32(b, 4), le32(b, 8), le32(b, 0xc), le32(b, 0x10), le32(b, 0x14)};
    if (!h.channels || h.channels > 8 || !h.rate || !h.chunk_bytes || h.chunk_bytes % (16 * h.channels) ||
        h.channel_bytes % 16)
        throw std::runtime_error("Unsupported VPK header");
    const std::uint64_t per = h.chunk_bytes / h.channels;
    const std::uint64_t chunks = (std::uint64_t(h.channel_bytes) + per - 1) / per;
    if (h.data_offset + chunks * h.chunk_bytes > file_size) throw std::runtime_error("VPK data outside the file");
    return h;
}

// decode_vpk's chunk chunk (the bytes of every channel from ase) into clip frames from done / 16 * 28.
void decode_vpk_chunk(const VpkHeader& h, std::span<const std::uint8_t> b, std::size_t base, std::size_t done,
                      std::vector<AdpcmState>& states, PcmClip& clip) {
    const std::size_t per = h.chunk_bytes / h.channels;
    const std::size_t n = std::min<std::size_t>(per, h.channel_bytes - done);
    std::int16_t pcm[28];
    for (std::size_t c = 0; c < h.channels; ++c) {
        for (std::size_t q = 0; q < n; q += 16) {
            if (decode_adpcm_frame(b.subspan(base + c * per + q, 16), states[c], pcm) != 0)
                throw std::runtime_error("VPK frame with flags");
            const std::size_t first = (done + q) / 16 * 28;
            for (std::size_t i = 0; i < 28; ++i) clip.slot(first + i, c) = pcm[i];
        }
    }
}

}  // namespace

VpkHeader parse_vpk(std::span<const std::uint8_t> b) { return vpk_header(b, b.size()); }

VpkHeader parse_vpk_header(std::span<const std::uint8_t> b) { return vpk_header(b, ~std::uint64_t(0)); }

PcmClip decode_vag(std::span<const std::uint8_t> b) {
    const auto h = parse_vag(b);
    PcmClip clip{h.rate, 1, {}, false};
    AdpcmState state;
    std::int16_t pcm[28];
    for (std::size_t p = 0x30; p < 0x30 + std::size_t(h.data_size); p += 16) {
        const auto flags = decode_adpcm_frame(b.subspan(p, 16), state, pcm);
        clip.samples.insert(clip.samples.end(), pcm, pcm + 28);
        if (flags & 1) {
            clip.repeat = (flags & 2) != 0;
            return clip;
        }
    }
    throw std::runtime_error("VAG without an end frame");
}

PcmClip decode_vpk(std::span<const std::uint8_t> b) {
    const auto h = parse_vpk(b);
    const std::size_t per = h.chunk_bytes / h.channels;
    const std::size_t frames = std::size_t(h.channel_bytes) / 16 * 28;
    PcmClip clip{h.rate, h.channels, std::vector<std::int16_t>(frames * h.channels), false};
    std::vector<AdpcmState> states(h.channels);
    for (std::size_t done = 0, chunk = 0; done < h.channel_bytes; done += per, ++chunk)
        decode_vpk_chunk(h, b, h.data_offset + chunk * h.chunk_bytes, done, states, clip);
    return clip;
}

VpkProgressive::VpkProgressive(std::span<const std::uint8_t> header, Reader read, bool stream)
    : header_(vpk_header(header, stream_bytes(header))), read_(std::move(read)), states_(header_.channels) {
    const std::size_t frames = std::size_t(header_.channel_bytes) / 16 * 28;
    const std::size_t chunk_frames = header_.chunk_bytes / header_.channels / 16 * 28;
    if (stream && chunk_frames && frames > 6 * chunk_frames) {
        clip_ = std::make_shared<PcmClip>(PcmClip{header_.rate, header_.channels, {}, false});
        clip_->ring = 4 * chunk_frames;
        clip_->total = frames;
        clip_->head.assign(chunk_frames * header_.channels, 0);
        clip_->samples.assign(clip_->ring * header_.channels, 0);
        return;
    }
    clip_ = std::make_shared<PcmClip>(
        PcmClip{header_.rate, header_.channels, std::vector<std::int16_t>(frames * header_.channels), false});
}

std::size_t VpkProgressive::decoded_frames() const {
    return std::min<std::size_t>(done_bytes_, header_.channel_bytes) / 16 * 28;
}

void VpkProgressive::decode_to(std::size_t frames) {
    const std::size_t per = header_.chunk_bytes / header_.channels;
    // Stream mode: decoding runs at most a chunk past the furthest reader, so a reader more than two chunks
    // behind the decoded frames went back (a loop, a replay): the ring no longer holds its frames.
    if (stream() && frames && frames + clip_->ring / 2 < decoded_frames()) {
        chunk_ = done_bytes_ = 0;
        std::fill(states_.begin(), states_.end(), AdpcmState{});
    }
    while (!done() && decoded_frames() < frames) {
        const auto bytes = read_(header_.data_offset + std::uint64_t(chunk_) * header_.chunk_bytes, header_.chunk_bytes);
        if (bytes.size() != header_.chunk_bytes) throw std::runtime_error("VPK chunk read short");
        decode_vpk_chunk(header_, bytes, 0, done_bytes_, states_, *clip_);
        done_bytes_ += per;
        ++chunk_;
    }
}

PcmClip decode_stream(std::span<const std::uint8_t> b) {
    if (is_vag(b)) return decode_vag(b);
    if (is_vpk(b)) return decode_vpk(b);
    throw std::runtime_error("Stream is neither VAGp nor VPK");
}

std::size_t stream_bytes(std::span<const std::uint8_t> b) {
    if (is_vag(b)) {
        if (b.size() < 0x30) throw std::runtime_error("VAG header outside the file");
        return 0x30 + std::size_t(be32(b, 0xc));
    }
    if (is_vpk(b)) {
        if (b.size() < 0x20) throw std::runtime_error("VPK header outside the file");
        const VpkHeader h{le32(b, 4), le32(b, 8), le32(b, 0xc), le32(b, 0x10), le32(b, 0x14)};
        if (!h.channels || !h.chunk_bytes || h.chunk_bytes % h.channels) throw std::runtime_error("Unsupported VPK header");
        const std::uint64_t per = h.chunk_bytes / h.channels;
        return std::size_t(h.data_offset + (std::uint64_t(h.channel_bytes) + per - 1) / per * h.chunk_bytes);
    }
    throw std::runtime_error("Stream is neither VAGp nor VPK");
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t c = 0xffffffffU;
    for (const auto v : bytes) {
        c ^= v;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

std::uint32_t pcm_crc32(const PcmClip& clip) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(clip.samples.size() * 2);
    for (const auto s : clip.samples) {
        const auto u = static_cast<std::uint16_t>(s);
        bytes.push_back(static_cast<std::uint8_t>(u));
        bytes.push_back(static_cast<std::uint8_t>(u >> 8));
    }
    return crc32(bytes);
}

}
