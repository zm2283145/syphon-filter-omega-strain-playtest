#include "spu2_voices.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega::audio {
namespace {

[[noreturn]] void unmodelled(const std::string& what) {
    throw std::runtime_error("SPU2 host model: " + what + " is not modelled");
}

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
}

// One envelope step size and period for a 7-bit rate (shift << 2 | step).
struct Rate {
    std::int32_t step{}, cycles{};
};
Rate rate(std::int32_t r, bool decreasing, bool exponential, std::int32_t level) {
    const std::int32_t shift = r >> 2, s = r & 3;
    Rate out;
    out.cycles = 1 << std::max(0, shift - 11);
    out.step = (decreasing ? -8 + s : 7 - s) * (1 << std::max(0, 11 - shift));
    if (exponential && !decreasing && level > 0x6000) out.cycles *= 4;
    if (exponential && decreasing) out.step = (out.step * level) >> 15;
    return out;
}

// A fixed SPU2 voice volume: bit 15 clear, bits 14..0 a signed level / 2.
std::int32_t volume(std::uint16_t v) {
    if (v & 0x8000) unmodelled("a sweep voice volume (" + hex(v) + ")");
    const std::int32_t level = (v & 0x4000) ? std::int32_t(v) - 0x8000 : std::int32_t(v);
    return level * 2;
}

}  // namespace

Spu2Voices::Spu2Voices() : ram_(ram_size) {}

void Spu2Voices::write_ram(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
    if (std::uint64_t(addr) + bytes.size() > ram_size) throw std::runtime_error("SPU2 host model: write outside sound RAM");
    std::copy(bytes.begin(), bytes.end(), ram_.begin() + addr);
}

void Spu2Voices::set_overlay(std::uint32_t addr, std::shared_ptr<const std::vector<std::uint8_t>> blocks) {
    if (addr >= ram_size || (addr & 0xf)) throw std::runtime_error("SPU2 host model: an overlay at 0x" + hex(addr));
    if (!blocks || blocks->empty() || blocks->size() % 16)
        throw std::runtime_error("SPU2 host model: an overlay that is not whole ADPCM blocks");
    overlays_[addr] = std::move(blocks);
}

std::size_t Spu2Voices::clear_overlays(std::uint32_t lo, std::uint32_t hi) {
    std::size_t n = 0;
    for (auto it = overlays_.lower_bound(lo); it != overlays_.end() && it->first < hi; ++n) it = overlays_.erase(it);
    return n;
}

void Spu2Voices::set_param(std::uint16_t entry, std::uint16_t value) {
    const std::uint32_t core = entry & 1, v = (entry >> 1) & 0x1f, param = entry >> 8;
    if (v >= 24 || param > 4) unmodelled("libsd parameter entry " + hex(entry));
    auto& x = voices_[core][v];
    switch (param) {
    case 0: x.voll = value; break;
    case 1: x.volr = value; break;
    case 2: x.pitch = value; break;
    case 3: x.adsr1 = value; break;
    case 4: x.adsr2 = value; break;
    }
}

std::uint16_t Spu2Voices::get_param(std::uint16_t entry) {
    const std::uint32_t core = entry & 1, v = (entry >> 1) & 0x1f, param = entry >> 8;
    if (v >= 24 || param != 5) unmodelled("libsd parameter read " + hex(entry));
    return static_cast<std::uint16_t>(voices_[core][v].level);
}

void Spu2Voices::set_switch(std::uint16_t entry, std::uint32_t value) {
    const std::uint32_t core = entry & 1, reg = entry >> 8;
    if ((entry & 0xfe) != 0) unmodelled("libsd switch entry " + hex(entry));
    value &= 0xffffff;
    switch (reg) {
    case 0x14: case 0x18: case 0x19: case 0x1a: case 0x1b:
        switches_[reg][core] = value;
        return;
    case 0x15:
        for (std::uint32_t v = 0; v < 24; ++v)
            if (value & (1u << v)) key_on(core, v);
        return;
    case 0x16:
        for (std::uint32_t v = 0; v < 24; ++v)
            if (value & (1u << v)) key_off(core, v);
        return;
    default:
        unmodelled("libsd switch entry " + hex(entry));
    }
}

std::uint32_t Spu2Voices::get_switch(std::uint16_t entry) {
    const std::uint32_t core = entry & 1, reg = entry >> 8;
    if ((entry & 0xfe) != 0) unmodelled("libsd switch read " + hex(entry));
    switch (reg) {
    case 0x14: case 0x18: case 0x19: case 0x1a: case 0x1b:
        return switches_[reg][core];
    default:
        unmodelled("libsd switch read " + hex(entry));
    }
}

void Spu2Voices::set_addr(std::uint16_t entry, std::uint32_t value) {
    const std::uint32_t core = entry & 1, v = (entry >> 1) & 0x1f;
    if (v >= 24 || (entry & 0xffc0) != 0x2040) unmodelled("libsd address entry " + hex(entry));
    if (value >= ram_size || (value & 0xf)) unmodelled("a voice start address " + hex(value));
    voices_[core][v].ssa = value;
}

void Spu2Voices::key_on(std::uint32_t core, std::uint32_t v) {
    if (switches_[0x14][core] & (1u << v)) unmodelled("a noise voice (NON) key on");
    auto& x = voices_[core][v];
    x.overlay.reset();
    if (!overlays_.empty())
        if (const auto it = overlays_.find(x.ssa); it != overlays_.end()) x.overlay = it->second;
    x.nax = x.overlay ? 0 : x.ssa;
    x.lsa = x.nax;
    x.adpcm = {};
    x.index = 28;
    x.flags = 0;
    x.frac = 0;
    x.phase = Phase::attack;
    x.level = 0;
    x.counter = 0;
    x.prev = 0;
    x.cur = 0;
    x.cur = next_sample(x);
}

void Spu2Voices::key_off(std::uint32_t core, std::uint32_t v) {
    auto& x = voices_[core][v];
    if (x.phase == Phase::off) return;
    x.phase = Phase::release;
    x.counter = 0;
}

std::int32_t Spu2Voices::next_sample(Voice& x) {
    if (x.phase == Phase::off) return 0;
    if (x.index >= 28) {
        if (x.flags & 1) {  // the block just played ends: jump to the loop address
            x.nax = x.lsa;
            if (!(x.flags & 2)) {
                x.phase = Phase::off;
                x.level = 0;
                x.flags = 0;
                return 0;
            }
        }
        const std::uint8_t* frame = nullptr;
        if (x.overlay) {
            if (x.nax + 16 > x.overlay->size()) {  // set_overlay's blocks end with flags bit 0: not reached
                x.phase = Phase::off;
                x.level = 0;
                x.flags = 0;
                return 0;
            }
            frame = x.overlay->data() + x.nax;
        } else {
            x.nax &= ram_size - 1;
            frame = ram_.data() + x.nax;
        }
        x.flags = decode_adpcm_frame(std::span<const std::uint8_t>(frame, 16), x.adpcm, x.block.data());
        if (x.flags & 4) x.lsa = x.nax;
        x.nax += 16;
        x.index = 0;
    }
    return x.block[x.index++];
}

void Spu2Voices::envelope(Voice& x) {
    if (x.phase == Phase::off) return;
    if (--x.counter > 0) return;
    Rate r;
    switch (x.phase) {
    case Phase::attack: r = rate((x.adsr1 >> 8) & 0x7f, false, (x.adsr1 & 0x8000) != 0, x.level); break;
    case Phase::decay: r = rate(((x.adsr1 >> 4) & 0xf) << 2, true, true, x.level); break;
    case Phase::sustain: r = rate((x.adsr2 >> 6) & 0x7f, (x.adsr2 & 0x4000) != 0, (x.adsr2 & 0x8000) != 0, x.level); break;
    case Phase::release: r = rate((x.adsr2 & 0x1f) << 2, true, (x.adsr2 & 0x20) != 0, x.level); break;
    case Phase::off: return;
    }
    x.counter = r.cycles;
    x.level = std::clamp(x.level + r.step, 0, 0x7fff);
    switch (x.phase) {
    case Phase::attack:
        if (x.level >= 0x7fff) x.phase = Phase::decay;
        break;
    case Phase::decay:
        if (x.level <= std::min(((x.adsr1 & 0xf) + 1) << 11, 0x7fff)) x.phase = Phase::sustain;
        break;
    case Phase::release:
        if (x.level == 0) x.phase = Phase::off;
        break;
    default:
        break;
    }
}

void Spu2Voices::render_add(std::size_t frames, std::vector<std::int16_t>& out) {
    if (out.size() < frames * 2) throw std::runtime_error("SPU2 host model: render past the start of the buffer");
    auto* dst = out.data() + (out.size() - frames * 2);
    // Voice by voice into integer sums, then one clamp per frame: the voices are independent within a render
    // (key on / off and the switches change only between renders) and integer sums do not depend on their order,
    // so this is the frame-by-frame mix's exact output with the off voices skipped once rather than per frame.
    mix_.assign(frames * 2, 0);
    if (frames == 0) return;
    std::int32_t* sum = mix_.data();
    for (std::uint32_t core = 0; core < 2; ++core) {
        for (std::uint32_t v = 0; v < 24; ++v) {
            auto& x = voices_[core][v];
            if (x.phase == Phase::off) continue;
            const bool to_left = (switches_[0x18][core] & (1u << v)) != 0;
            const bool to_right = (switches_[0x1a][core] & (1u << v)) != 0;
            const std::int32_t vl = to_left ? volume(x.voll) : 0, vr = to_right ? volume(x.volr) : 0;
            const std::uint32_t step = std::min<std::uint32_t>(x.pitch, 0x3fff);
            for (std::size_t f = 0; f < frames && x.phase != Phase::off; ++f) {
                const std::int32_t s = x.prev + (((x.cur - x.prev) * std::int32_t(x.frac)) >> 12);
                const std::int32_t e = (s * x.level) >> 15;
                if (to_left) sum[f * 2] += (e * vl) >> 15;
                if (to_right) sum[f * 2 + 1] += (e * vr) >> 15;
                envelope(x);
                x.frac += step;
                while (x.frac >= 0x1000 && x.phase != Phase::off) {
                    x.frac -= 0x1000;
                    x.prev = x.cur;
                    x.cur = next_sample(x);
                }
            }
        }
    }
    for (std::size_t f = 0; f < frames * 2; ++f)
        dst[f] = static_cast<std::int16_t>(std::clamp(dst[f] + sum[f], -0x8000, 0x7fff));
}

std::size_t Spu2Voices::active_voices() const {
    std::size_t n = 0;
    for (const auto& core : voices_)
        for (const auto& x : core) n += x.phase != Phase::off;
    return n;
}

}  // namespace omega::audio
