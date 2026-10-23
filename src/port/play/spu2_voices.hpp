#pragma once

// A host model of the PS2 SPU2 voices behind the libsd calls of the 989snd
// bank player (snd989::SpuInterface, checkpoint 119k). Nothing here is from
// the game binaries: the IOP's SPU2 is hardware, so the behaviour below is
// the documented SPU (PSX-SPX "SPU ADSR" / "SPU ADPCM" and the SPU2 register
// map that libsd's entries encode) and every simplification is a host
// assumption listed in research/GENERATOR_RUNTIME.md (checkpoint 119k):
//
//   - 2 cores x 24 voices, 2 MB of sound RAM; bank bodies are written at the
//     SPU address the host gave their bank (BankEngine::load_bank).
//   - libsd entries: voice parameters core | voice << 1 | param << 8 (VOLL 0,
//     VOLR 1, PITCH 2, ADSR1 3, ADSR2 4, ENVX 5), the start address
//     core | voice << 1 | 0x2040, switches core | reg << 8 (NON 0x14, KON
//     0x15, KOFF 0x16, VMIXL 0x18, VMIXEL 0x19, VMIXR 0x1a, VMIXER 0x1b).
//     Anything else the bank player does not use throws.
//   - PS-ADPCM blocks (audio::decode_adpcm_frame); flags bit 2 sets the loop
//     address at the block, bit 0 jumps to it after the block, without bit 1
//     the voice stops with its envelope at 0. Key on starts at the start
//     address with the loop address set to it and the decoder history clear.
//   - Pitch 0x1000 = 48 kHz (capped at 0x3fff), linear interpolation between
//     consecutive samples (the SPU2's Gaussian filter is not modelled).
//   - ADSR per sample at 48 kHz: rate = shift << 2 | step, cycles
//     1 << max(0, shift - 11), step (7 - s or -8 + s) << max(0, 11 - shift),
//     exponential increase 4x slower above 0x6000, exponential decrease scaled
//     by level / 0x8000; attack (ADSR1 15 / 14..8) to 0x7fff, decay (ADSR1
//     7..4 << 2, exponential decrease) to ((ADSR1 & 0xf) + 1) << 11, sustain
//     (ADSR2 15 / 14 / 12..6) until key off, release (ADSR2 5 / 4..0 << 2) to 0.
//   - Fixed voice volumes only (bit 15 clear: a 15-bit signed level, x 2 /
//     0x8000); a sweep volume throws. Dry output only (VMIXL / VMIXR); the
//     reverb sends VMIXEL / VMIXER are remembered but no reverb is rendered.
//     Core 0 and core 1 are summed at unity (the core and master volumes libsd
//     sets at init are not modelled).
//
// Port-only (checkpoint 132md5, research/MODDING.md "Sound effects in the banks"): a mod's replacement of one bank
// sample is an overlay, PS-ADPCM blocks in host memory registered at the sample's start address; a voice keyed on at
// that start address reads its blocks (and loop / end flags) from the overlay instead of sound RAM. Sound RAM, the
// bank engine and every other voice are untouched; with no overlay registered nothing changes.

#include "audio_formats.hpp"
#include "snd_bank.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace omega::audio {

class Spu2Voices final : public snd989::SpuInterface {
public:
    static constexpr std::uint32_t ram_size = 0x200000;

    Spu2Voices();
    void write_ram(std::uint32_t addr, std::span<const std::uint8_t> bytes);
    const std::vector<std::uint8_t>& ram() const { return ram_; }
    // Checkpoint 132md5: replacement blocks for voices keyed on at `addr` (16-byte aligned; whole blocks, the last
    // with flags bit 0); clear_overlays drops those whose address is in [lo, hi) (a bank's body when it unloads) and
    // returns how many. Voices already playing one keep it to their end.
    void set_overlay(std::uint32_t addr, std::shared_ptr<const std::vector<std::uint8_t>> blocks);
    std::size_t clear_overlays(std::uint32_t lo, std::uint32_t hi);
    std::size_t overlay_count() const { return overlays_.size(); }

    void set_param(std::uint16_t entry, std::uint16_t value) override;
    std::uint16_t get_param(std::uint16_t entry) override;
    void set_switch(std::uint16_t entry, std::uint32_t value) override;
    std::uint32_t get_switch(std::uint16_t entry) override;
    void set_addr(std::uint16_t entry, std::uint32_t value) override;

    // Renders `frames` 48 kHz stereo frames and adds them (saturating) to the
    // last `frames` frames of `out` (interleaved).
    void render_add(std::size_t frames, std::vector<std::int16_t>& out);
    // Voices whose envelope is running (not off).
    std::size_t active_voices() const;

    enum class Phase { off, attack, decay, sustain, release };
    struct Voice {
        std::uint16_t voll{}, volr{}, pitch{}, adsr1{}, adsr2{};
        std::uint32_t ssa{}, lsa{}, nax{};
        Phase phase = Phase::off;
        std::int32_t level{};    // ENVX
        std::int32_t counter{};  // samples until the next envelope step
        std::uint32_t frac{};    // 4.12 pitch counter
        std::int32_t prev{}, cur{};
        std::array<std::int16_t, 28> block{};
        std::uint8_t flags{};
        std::size_t index = 28;  // next sample of `block`
        AdpcmState adpcm;
        std::shared_ptr<const std::vector<std::uint8_t>> overlay;  // 132md5: nax is an offset into it
    };
    const Voice& voice(std::uint32_t core, std::uint32_t v) const { return voices_[core][v]; }

private:
    void key_on(std::uint32_t core, std::uint32_t v);
    void key_off(std::uint32_t core, std::uint32_t v);
    std::int32_t next_sample(Voice& v);
    void envelope(Voice& v);
    std::vector<std::uint8_t> ram_;
    std::array<std::array<Voice, 24>, 2> voices_{};
    std::array<std::array<std::uint32_t, 2>, 0x20> switches_{};  // [reg][core]
    std::map<std::uint32_t, std::shared_ptr<const std::vector<std::uint8_t>>> overlays_;
};

}
