#pragma once

// Disc audio formats streamed by the IOP's 989snd.irx (not ported; the EE
// side is src/main/gameSound/game_sound.*). Layouts were measured on the
// disc (tools/ps_adpcm_ref.py is the reference decoder used by the tests):
//
//   VAG  quips / voice (GAMEDATA\<level>\SNDVAG.HOG, ZMEDIA\SNDVAG*.HOG):
//        big-endian "VAGp", +4 version (0 or 4), +c data bytes, +10 rate
//        (22050), +20 name; mono PS-ADPCM frames from +30. Every file starts
//        with an all-zero frame and ends with a frame whose flags have bit 0
//        (1, or 7 = end + repeat + loop start; version 4 files end 1, 7).
//   VPK  music (ZMEDIA\MUSIC.HOG): little-endian " KPV", +4 bytes per
//        channel, +8 data offset (0x800), +c the interleave of one chunk of
//        all channels (0xe000), +10 rate (32000), +14 channels (2), +18
//        eight bytes 7f 00 0e 01 7f 00 5a 00 (not interpreted). Each chunk
//        holds +c / channels bytes of every channel in turn; the last chunk is
//        padded to full size and the file ends with it. The padding, and the
//        last two frames of each channel's byte count, are frames 0xc0
//        followed by 15 zero bytes; no frame carries flags.
//   Frame 16 bytes: byte 0 = filter << 4 | shift, byte 1 = flags, 14 bytes of
//        4-bit samples, low nibble first; s = (nibble << 12 >> shift) +
//        ((s1 * f0 + s2 * f1 + 32) >> 6), clamped to int16, with f0 / f1 the
//        SPU2 filter pairs 0..4.
//
// Host policy (documented in research/GENERATOR_RUNTIME.md, checkpoint 119i):
// filter 12 has no known SPU2 coefficient; the only such frames on the disc
// are the all-zero VPK padding frames, decoded as filter 0 (28 zero samples).
// Any other filter above 4 or shift above 12 throws.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace omega::audio {

struct AdpcmState {
    std::int32_t s1{}, s2{};
};

// Decodes one 16-byte frame into 28 samples; returns the flags byte.
std::uint8_t decode_adpcm_frame(std::span<const std::uint8_t> frame, AdpcmState& state, std::int16_t* out);

struct PcmClip {
    std::uint32_t rate{};
    std::uint32_t channels{};
    std::vector<std::int16_t> samples;  // interleaved, frames * channels
    bool repeat{};                      // VAG: the end frame's repeat bit (flags & 2)
    // Port-only (132vt2e, PS Vita, research/VITA.md "Memory"): ring > 0 marks a streamed VPK (VpkProgressive's
    // stream mode): `total` frames long, frames [0, head frames) kept in `head`, frame f past them at
    // samples[(f % ring) * channels]. 0: every frame is in `samples`.
    std::size_t ring{}, total{};
    std::vector<std::int16_t> head;
    std::size_t frames() const { return ring ? total : channels ? samples.size() / channels : 0; }
    std::int16_t at(std::size_t frame, std::size_t channel) const {
        if (!ring) return samples[frame * channels + channel];
        const std::size_t h = head.size() / channels;
        return frame < h ? head[frame * channels + channel] : samples[(frame % ring) * channels + channel];
    }
    std::int16_t& slot(std::size_t frame, std::size_t channel) {
        if (!ring) return samples[frame * channels + channel];
        const std::size_t h = head.size() / channels;
        return frame < h ? head[frame * channels + channel] : samples[(frame % ring) * channels + channel];
    }
};

struct VagHeader {
    std::uint32_t version{}, data_size{}, rate{};
    std::string name;
};

struct VpkHeader {
    std::uint32_t channel_bytes{}, data_offset{}, chunk_bytes{}, rate{}, channels{};
};

bool is_vag(std::span<const std::uint8_t> bytes);
bool is_vpk(std::span<const std::uint8_t> bytes);
VagHeader parse_vag(std::span<const std::uint8_t> bytes);
VpkHeader parse_vpk(std::span<const std::uint8_t> bytes);
// Port-only (checkpoint 132md13): the header alone (a stream's first 0x30 bytes): the same checks but for the data
// lying inside the bytes given.
VagHeader parse_vag_header(std::span<const std::uint8_t> header);
VpkHeader parse_vpk_header(std::span<const std::uint8_t> header);
PcmClip decode_vag(std::span<const std::uint8_t> bytes);
PcmClip decode_vpk(std::span<const std::uint8_t> bytes);
// VAG or VPK by magic; throws for anything else.
PcmClip decode_stream(std::span<const std::uint8_t> bytes);
// The file length a VAG / VPK header (at least its first 0x30 bytes) declares.
std::size_t stream_bytes(std::span<const std::uint8_t> header);

// Port-only (checkpoint 132win5): a VPK decoded a chunk at a time into a clip sized up front, so starting a
// music stream reads and decodes its first chunks only (the whole-file read and decode was ~80 ms in one
// front-end frame). Frames [0, decoded_frames()) hold decode_vpk's samples; the rest are 0 until decoded.
// read(offset, size) returns the file's bytes [offset, offset + size).
class VpkProgressive {
public:
    using Reader = std::function<std::vector<std::uint8_t>(std::uint64_t offset, std::size_t size)>;
    // Port-only (132vt2e, PS Vita): stream = true keeps the first chunk and a ring of four chunks (PcmClip::ring)
    // rather than the whole clip (TORONTO1's music was 11 MB, the front end's 30 MB); a reader far behind the
    // decoded frames (a loop or a replay from the start) restarts the decode at chunk 0.
    VpkProgressive(std::span<const std::uint8_t> header, Reader read, bool stream = false);
    const std::shared_ptr<PcmClip>& clip() const { return clip_; }
    std::size_t decoded_frames() const;
    bool done() const { return done_bytes_ >= header_.channel_bytes; }
    bool stream() const { return clip_->ring != 0; }
    // Decodes whole chunks until frame rames - 1 holds its samples (or the clip is complete).
    void decode_to(std::size_t frames);

private:
    VpkHeader header_;
    Reader read_;
    std::shared_ptr<PcmClip> clip_;
    std::vector<AdpcmState> states_;
    std::size_t chunk_ = 0, done_bytes_ = 0;
};

std::uint32_t crc32(std::span<const std::uint8_t> bytes);
std::uint32_t pcm_crc32(const PcmClip& clip);  // CRC-32 of the little-endian interleaved samples

}
