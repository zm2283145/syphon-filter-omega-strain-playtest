#pragma once
// Port-only (checkpoint 132md4, research/MODDING.md "Sounds and music"): the replacement sound files a mod's sounds\
// folder holds, decoded for the game's stream path (sound_iop_host.cpp). Not game code: the PS2 game plays only its
// disc's VAG / VPK streams. WAV (PCM 8 / 16 / 24 / 32 bit, float 32 / 64, WAVE_FORMAT_EXTENSIBLE) and Ogg Vorbis
// (Vorbis I, floor 1; the port's own decoder, no third-party code) are read; the result is fitted to the original
// stream's rate, channels and (for a stream that does not loop) length.

#include "audio_formats.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::mods {

struct DecodedAudio {
    std::uint32_t rate{};
    std::uint32_t channels{};
    std::vector<float> samples;   // interleaved, frames * channels, nominally -1..1
    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// Each throws std::runtime_error naming the problem (not that format, truncated, unsupported, corrupt).
DecodedAudio decode_wav(std::span<const std::uint8_t> bytes);
DecodedAudio decode_ogg_vorbis(std::span<const std::uint8_t> bytes);
// By content ("RIFF....WAVE" or "OggS").
DecodedAudio decode_audio_file(std::span<const std::uint8_t> bytes);

// The audio converted to `channels` (1: the channels' mean; more: each channel from the source's channel of that
// number, the last one repeated), resampled (linear) to `rate`, and, with `frames`, cut or padded with silence to
// exactly that many frames; 16-bit samples rounded and clamped. Checkpoint 132md13: `gain` scales each sample before
// the rounding (a mod's audio_volume=; 1 leaves every sample as it was).
audio::PcmClip fit_audio(const DecodedAudio& audio, std::uint32_t rate, std::uint32_t channels,
                         std::optional<std::size_t> frames = std::nullopt, float gain = 1.0F);

class AudioSource;

// fit_audio(decode_audio_file(file), rate, channels, frames)'s samples into a clip sized up front, decoded a piece
// at a time as the mix reaches them (the game's music streams are minutes long: a whole Ogg Vorbis file decoded at
// the stream start would stall that frame; the disc's VPK music is read the same way, audio::VpkProgressive). The
// constructor reads the headers and the length (Ogg: every page and its CRC) and throws on what decode_audio_file
// would refuse there; frames [0, decoded_frames()) hold fit_audio's samples, the rest are 0 until decoded.
class FittedAudio {
public:
    FittedAudio(std::vector<std::uint8_t> file, std::uint32_t rate, std::uint32_t channels,
                std::optional<std::size_t> frames = std::nullopt, float gain = 1.0F);
    ~FittedAudio();
    FittedAudio(const FittedAudio&) = delete;
    FittedAudio& operator=(const FittedAudio&) = delete;
    const std::shared_ptr<audio::PcmClip>& clip() const { return clip_; }
    std::uint32_t source_rate() const;
    std::uint32_t source_channels() const;
    std::size_t source_frames() const;
    std::size_t decoded_frames() const { return done_; }
    bool done() const { return done_ >= limit_; }
    // Decodes until frame frames - 1 holds its samples (or every frame does); throws on a corrupt packet.
    void decode_to(std::size_t frames);

private:
    std::vector<std::uint8_t> file_;
    std::unique_ptr<AudioSource> source_;
    std::shared_ptr<audio::PcmClip> clip_;
    std::uint32_t rate_ = 0, channels_ = 0;
    float gain_ = 1;
    std::size_t limit_ = 0, done_ = 0, mixed_base_ = 0;
    double step_ = 1;
    std::vector<float> raw_, mixed_;   // mixed_: source frames [mixed_base_, ...) in the clip's channels
};

// A 16-bit PCM WAV file of a clip (omega_mod_tool export-sounds / decode-audio).
std::vector<std::uint8_t> encode_wav(const audio::PcmClip& clip);

// Checkpoint 132md13 (omega_mod_tool pack --platform vita): a .wav / .ogg file as a 16-bit WAV of fit_audio(decoded,
// rate, channels) at its own length (no gain). 0 for `rate` / `channels`: the file's own, at most `max_rate` /
// `max_channels`. Throws as decode_audio_file.
std::vector<std::uint8_t> convert_audio_file(std::span<const std::uint8_t> file, std::uint32_t rate, std::uint32_t channels,
                                             std::uint32_t max_rate = 48000, std::uint32_t max_channels = 2);

}  // namespace omega::mods
