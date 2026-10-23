#pragma once
// Port-only (checkpoint 132md5, research/MODDING.md "Sound effects in the banks"): the samples of a 989snd sound bank
// (.BNK: WEPS / ENVIRON / VOX / SHELL) as replaceable units, and a PS-ADPCM encoder for their replacements. Not game
// code: the PS2 game only plays its disc's banks. A bank file is the header [3, 2, 0x18, head size, body offset, body
// size] then the SBlk head (src/lib/989snd/snd_bank.cpp, load_bank / relocate_bank) and the ADPCM body. The head's
// tone grains (+0x20, type 1) point into its tone table (+0x34), whose tones hold a body offset (+0x10), the centre
// note (+2) and fine (+3); sound n (+0x1c, 12 bytes) runs its grains from +8; the name table (+0x38, head flag 0x100)
// names sounds. A "sample" here is one distinct tone body offset; samples are numbered in ascending body order, so
// bank sample n is the key <bank member key>/<n> (mods::ModSet "sounds").

#include "audio_formats.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega::mods {

struct BankSample {
    std::size_t index = 0;             // the n of <bank key>/<n>
    std::uint32_t offset = 0;          // its first block in the body
    std::uint32_t blocks = 0;          // 16-byte blocks through the one whose flags have bit 0 (end / jump)
    bool loops = false;                // the end block has bit 1 (repeat): the voice loops
    std::uint32_t loop_block = 0;      // with loops: the last block at or before the end with flags bit 2
    std::int32_t centre_note = 0;      // of the first tone using it (s8 tone +2; negative: a 48 kHz sample)
    std::int32_t centre_fine = 0;      // s8 tone +3, 1/128 semitone
    std::uint32_t rate = 0;            // the rate it plays at by default (note 60, no bend): see bank_sample_rate
    std::vector<std::string> sounds;   // the sounds whose tone grains play it: their names, or #<sound index>
    std::size_t frames() const { return std::size_t(blocks) * 28; }
};

// "\3\0\0\0\2\0\0\0\x18\0\0\0" and an SBlk head.
bool is_sound_bank(std::span<const std::uint8_t> file);
// The bank's samples; throws std::runtime_error naming the problem (truncated, a table outside the head, a sample
// outside the body or without an end block).
std::vector<BankSample> bank_samples(std::span<const std::uint8_t> file);
// The rate in Hz the bank engine plays a tone at when the sound plays its default note 60 with no bend:
// 989snd's tone_pitch (sceSdNote2Pitch on the centre, x 44100 / 48000 for a non-negative centre note) as a real
// number: (centre >= 0 ? 44100 : 48000) * 2^((60 - |centre| + fine / 128) / 12), rounded.
std::uint32_t bank_sample_rate(std::int32_t centre_note, std::int32_t centre_fine);
// The sample decoded from the bank's body (mono, 16 bit, at sample.rate; repeat = loops).
audio::PcmClip decode_bank_sample(std::span<const std::uint8_t> file, const BankSample& sample);

// PS-ADPCM blocks of mono 16-bit samples (28 a block, the last padded with 0): each block's filter (0..4) and shift
// (0..12) are the pair with the least squared error through audio::decode_adpcm_frame's arithmetic (ties: the lower
// filter, then shift), so the encoding is deterministic. Flags: the first block 4 (loop start); the last 1 (end: the
// voice stops) or, with `loop`, 3 (end + repeat: the voice jumps back to the first block); the others 0, or 2 with
// `loop`. An empty input is one silent block.
std::vector<std::uint8_t> encode_adpcm(std::span<const std::int16_t> samples, bool loop);

}  // namespace omega::mods
