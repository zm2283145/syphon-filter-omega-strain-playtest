#pragma once
// ZMOVIES.HOG .PSS movies (research/FRONTEND.md, "Checkpoint 114 native movie
// decoder"): an MPEG-2 program stream in 16 KiB packs carrying one MPEG-2
// video stream (0xE0) and one private-stream-1 (0xBD) audio stream with the
// "ff a0 00 00" sub-header. The audio payload is an SShd/SSbd header followed
// by 48 kHz signed 16-bit PCM interleaved in 512-byte left/right blocks.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mpeg2_decoder.hpp"

namespace omega::movie {

constexpr std::size_t pss_pack_size = 16384;

struct PssDemuxStats {
    std::uint64_t packs{};
    std::uint64_t system_headers{};
    std::uint64_t video_pes{};
    std::uint64_t audio_pes{};
    std::uint64_t padding_pes{};
    std::uint64_t padding_bytes{};
    std::uint64_t video_bytes{};
    std::uint64_t audio_bytes{};
    std::uint64_t video_pts{};   // video PES packets carrying a PTS
    std::uint64_t audio_pts{};
    std::optional<std::uint64_t> first_scr;   // 90 kHz base of the first pack
    std::uint64_t last_scr{};
    std::uint64_t mux_rate{};   // bytes/s of the last pack header (mux_rate * 50)
    std::optional<std::uint64_t> first_video_pts, first_audio_pts;
    bool end_code{};
    std::uint64_t trailing_bytes{};   // zero bytes after the program end code
};

// Strict streaming demuxer mirroring local/frontend_movies/pss_demux.py: any
// structure outside the measured format throws.
class PssDemuxer {
public:
    // Feed container bytes in any chunking; payloads are appended to video()/audio().
    void push(std::span<const std::uint8_t> bytes);
    // Throws when the stream is incomplete (no program end code).
    void finish();
    std::vector<std::uint8_t>& video() { return video_; }
    std::vector<std::uint8_t>& audio() { return audio_; }
    const PssDemuxStats& stats() const { return stats_; }
    bool ended() const { return stats_.end_code; }

private:
    bool parse_one();
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_{};
    std::uint64_t consumed_{};
    std::vector<std::uint8_t> video_, audio_;
    PssDemuxStats stats_{};
};

struct PssAudioHeader {
    std::int32_t size{};        // 0x18
    std::int32_t type{};        // 1 = signed 16-bit little-endian PCM
    std::int32_t rate{};        // 48000
    std::int32_t channels{};    // 2
    std::int32_t interleave{};  // 512 bytes per channel block
    std::int32_t loop_start{}, loop_end{};   // -1 / -1
    std::uint32_t body_size{};  // SSbd size field
};

// SShd/SSbd parser and block de-interleaver: produces interleaved stereo
// samples (L0 R0 L1 R1 ...). A partial final block is held back.
class PssAudio {
public:
    void push(std::span<const std::uint8_t> bytes);
    bool has_header() const { return header_.has_value(); }
    const PssAudioHeader& header() const;
    std::vector<std::int16_t>& samples() { return samples_; }
    std::uint64_t body_bytes() const { return body_bytes_; }
    std::size_t pending_bytes() const { return pending_.size(); }   // partial block (or header) bytes

private:
    std::optional<PssAudioHeader> header_;
    std::vector<std::uint8_t> pending_;
    std::vector<std::int16_t> samples_;
    std::uint64_t body_bytes_{};
};

// De-interleaves whole blocks of `interleave` bytes per channel (exposed for tests).
void deinterleave_blocks(std::span<const std::uint8_t> blocks, int channels, int interleave,
                         std::vector<std::int16_t>& out);

// RIFF/WAVE header (PCM 16-bit) for `data_bytes` of sample data.
std::vector<std::uint8_t> wav_header(int rate, int channels, std::uint32_t data_bytes);

// CRC-32 (IEEE 802.3, the zlib polynomial) used for frame checksums.
std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);
std::uint32_t frame_crc32(const mpeg2::Frame& frame);   // over Y, then U, then V

// Packed RGB24 of a frame: BT.601 limited-range matrix, 4:2:0 chroma upsampled
// bilinearly (horizontally co-sited, vertically centred), no deinterlacing.
std::vector<std::uint8_t> frame_to_rgb(const mpeg2::Frame& frame);
// The same conversion straight to RGBA8 (alpha 255) for a texture upload, written into out (resized; reused across
// frames so a movie allocates once). Bit-identical to frame_to_rgb; ARM builds run the pixel loop on NEON
// (port-only, research/VITA.md "Movies").
void frame_to_rgba(const mpeg2::Frame& frame, std::vector<std::uint8_t>& out);
// Port-only (132vt2a2): frame_to_rgba's rows y0..y1 (exclusive) into out_pixels, the whole picture's RGBA (width x
// height x 4 bytes): the same bytes as frame_to_rgba's rows, so disjoint bands can be converted in parallel.
void frame_to_rgba_rows(const mpeg2::Frame& frame, std::uint8_t* out_pixels, int y0, int y1);

// The game ISO for ISO-dependent tools and tests: $OMEGA_ISO, else the first
// <dir>/*/*.iso (the layout tools/run_play_regressions.ps1 uses).
std::optional<std::string> locate_game_iso(const std::string& dir = ".");

// The HOG holding the movies and subtitles.
inline const std::vector<std::string>& movies_hog() {
    static const std::vector<std::string> path{"ZMEDIA", "ZMOVIES.HOG"};
    return path;
}

// What the front end's movie player (FrontendSession, 003f8220) pulls a movie
// through: a picture per call (two fields, 30000/1001 a second) and the audio
// demuxed or decoded so far. PssMovie is the disc's; port-only (checkpoint
// 132md14) a mod's replacement (mods/mod_movie.hpp) is another.
class MovieStream {
public:
    virtual ~MovieStream() = default;
    // The next display-order frame; false at the end of the movie.
    virtual bool next_frame(mpeg2::Frame& frame) = 0;
    // The audio's format once known (rate in Hz, interleaved channels).
    virtual bool audio_format(int& rate, int& channels) = 0;
    // Appends the audio produced since the last call (interleaved samples).
    virtual void take_audio(std::vector<std::int16_t>& out) = 0;
    // Port-only (132md14): the picture's display aspect (width / height) when it
    // is not the disc movies' 4:3; 0: 4:3.
    virtual float picture_aspect() const { return 0.0F; }
    // Port-only (132vt2a2, research/VITA.md "Movies"): reads one more chunk of the
    // container ahead of the pictures, for its audio (take_audio then has more);
    // the video read with it waits for next_frame. False when nothing was read
    // (the end, or no read-ahead in this stream). The pictures, their order and
    // the audio's samples are the same; only when the audio comes moves.
    virtual bool read_ahead() { return false; }
    // Port-only (132vt2a2): whether the last next_frame (true) showed the picture
    // before it again (a player pacing pictures by the audio's clock held it).
    virtual bool repeated_picture() const { return false; }
};

// A whole movie: pulls container bytes from `source` (returns the byte count
// read, 0 at the end), demuxes, decodes video in display order and collects
// the de-interleaved audio.
class PssMovie final : public MovieStream {
public:
    using Source = std::function<std::size_t(std::span<std::uint8_t>)>;
    explicit PssMovie(Source source, std::size_t chunk = pss_pack_size);
    // The next display-order frame; false at the end of the movie.
    bool next_frame(mpeg2::Frame& frame) override;
    bool audio_format(int& rate, int& channels) override;
    void take_audio(std::vector<std::int16_t>& out) override;
    // Port-only (checkpoint 132md14): reads one more chunk for its audio alone (the
    // video is not decoded from then on); false at the end of the container. A
    // mod's replacement picture with the disc movie's sound pulls it this way.
    bool pump_audio_only();
    // MovieStream::read_ahead (132vt2a2): one more chunk while the video read but
    // not yet decoded stays under read_ahead_video_limit; only with a video slice
    // (set_video_slice), which keeps the read video until next_frame needs it.
    bool read_ahead() override;
    bool source_done() const { return source_done_; }  // read-ahead reached the container end, before video drains
    static constexpr std::size_t read_ahead_video_limit = 8u << 20;
    mpeg2::Decoder& decoder() { return decoder_; }
    const PssDemuxer& demuxer() const { return demuxer_; }
    PssAudio& audio() { return audio_; }
    // Reads the rest of the container (demux + audio only; video is not decoded).
    void drain_audio();
    // Port-only (checkpoint 132win5): the demuxed video goes to the decoder ytes at a time, only while no
    // frame is ready, instead of the whole read chunk at once (0, the default: at once). A large read chunk
    // then no longer decodes a dozen pictures in one call: the frames, their order and the audio each call
    // leaves behind are the same (a read still happens only when the video read so far is used up).
    void set_video_slice(std::size_t bytes) { video_slice_ = bytes; }
    // Pictures decoded so far (I + P + B).
    std::uint64_t pictures_decoded() const;

private:
    bool pump();
    Source source_;
    std::vector<std::uint8_t> chunk_;
    PssDemuxer demuxer_;
    PssAudio audio_;
    mpeg2::Decoder decoder_;
    bool source_done_{};
    bool decode_video_{true};
    std::size_t video_slice_{};
    std::vector<std::uint8_t> video_pending_;   // demuxed, not yet pushed (video_slice_)
    std::size_t video_pushed_{};
    bool finish_pending_{};                     // the source ended: finish once the pending video is pushed
};

// Source over `size` bytes at `offset` of a seekable stream (e.g. a HOG member
// extent of the ISO, from DiscImage::archive_member_extent).
PssMovie::Source stream_source(std::istream& stream, std::uint64_t offset, std::uint64_t size);
// Source over bytes in memory (the span must outlive the movie).
PssMovie::Source memory_source(std::span<const std::uint8_t> bytes);

}  // namespace omega::movie
