#include "bank_sounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>

namespace omega::mods {
namespace {

constexpr std::uint32_t sblk = 0x6b6c4253;
constexpr std::size_t head_at = 0x18;
constexpr std::array<std::int32_t, 5> filter0{0, 60, 115, 98, 122};
constexpr std::array<std::int32_t, 5> filter1{0, 0, -52, -55, -60};

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::uint32_t u32(std::size_t at) const {
        if (at + 4 > bytes.size() || at + 4 < at) throw std::runtime_error("bank truncated at byte " + std::to_string(at));
        return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at + 1]) << 8 | std::uint32_t(bytes[at + 2]) << 16 |
               std::uint32_t(bytes[at + 3]) << 24;
    }
    std::int32_t s16(std::size_t at) const {
        if (at + 2 > bytes.size()) throw std::runtime_error("bank truncated at byte " + std::to_string(at));
        return static_cast<std::int16_t>(std::uint16_t(bytes[at] | bytes[at + 1] << 8));
    }
    std::int32_t s8(std::size_t at) const {
        if (at >= bytes.size()) throw std::runtime_error("bank truncated at byte " + std::to_string(at));
        return static_cast<std::int8_t>(bytes[at]);
    }
    std::uint8_t u8(std::size_t at) const { return static_cast<std::uint8_t>(s8(at)); }
};

struct Layout {
    std::size_t head = head_at, head_size = 0, body = 0, body_size = 0;
};

Layout layout(const Reader& r) {
    if (!is_sound_bank(r.bytes)) throw std::runtime_error("not a 989snd SBlk bank");
    Layout l;
    l.head_size = r.u32(12);
    l.body = r.u32(16);
    l.body_size = r.u32(20);
    if (l.head + l.head_size > r.bytes.size()) throw std::runtime_error("the bank head runs past the file");
    if (std::uint64_t(l.body) + l.body_size > r.bytes.size()) throw std::runtime_error("the bank body runs past the file");
    return l;
}

// The sound names (name table, head flag 0x100) by sound index; best effort: an inconsistent table names nothing.
std::map<std::int32_t, std::string> sound_names(const Reader& r, const Layout& l) {
    std::map<std::int32_t, std::string> names;
    try {
        if (!(r.u32(l.head + 8) & 0x100)) return names;
        const std::size_t nb = l.head + r.u32(l.head + 0x38);
        const std::size_t table = nb + r.u32(nb + 8);
        for (std::size_t k = 0; k < 32; ++k) {
            const auto first = r.s16(nb + 0x18 + k * 2);
            if (first < 0) continue;
            for (std::size_t e = table + std::size_t(first) * 20, guard = 0; r.u32(e) != 0 && guard < 0x10000; e += 20, ++guard) {
                std::string name;
                for (std::size_t i = 0; i < 16 && r.u8(e + i); ++i) name += static_cast<char>(r.u8(e + i));
                names.emplace(r.s16(e + 0x10), name);
            }
        }
    } catch (const std::exception&) {
        names.clear();
    }
    return names;
}

}  // namespace

bool is_sound_bank(std::span<const std::uint8_t> file) {
    if (file.size() < head_at + 4) return false;
    const Reader r{file};
    return r.u32(0) == 3 && r.u32(4) == 2 && r.u32(8) == head_at && r.u32(head_at) == sblk;
}

std::uint32_t bank_sample_rate(std::int32_t centre_note, std::int32_t centre_fine) {
    const bool scale = centre_note >= 0;
    const double base = scale ? 44100.0 : 48000.0;
    const std::int32_t note = scale ? centre_note : -centre_note;
    const double rate = base * std::pow(2.0, (60.0 - note + centre_fine / 128.0) / 12.0);
    return static_cast<std::uint32_t>(std::clamp(static_cast<double>(std::llround(rate)), 1.0, 48000.0 * 0x3fff / 0x1000));
}

std::vector<BankSample> bank_samples(std::span<const std::uint8_t> file) {
    const Reader r{file};
    const auto l = layout(r);
    const std::size_t sounds = l.head + r.u32(l.head + 0x1c), grains = l.head + r.u32(l.head + 0x20);
    const std::size_t tones = l.head + r.u32(l.head + 0x34);
    const auto sound_count = r.s16(l.head + 0x16), grain_count = r.s16(l.head + 0x18);
    const auto in_head = [&](std::size_t at, std::size_t size, const char* what) {
        if (at < l.head || at + size > l.head + l.head_size)
            throw std::runtime_error(std::string(what) + " outside the bank head");
    };
    in_head(sounds, std::size_t(std::max(0, sound_count)) * 12, "the sound table");
    in_head(grains, std::size_t(std::max(0, grain_count)) * 8, "the grain table");
    // The tone each type-1 grain plays: its body offset and centre (first use wins).
    std::map<std::uint32_t, BankSample> by_offset;
    std::map<std::size_t, std::uint32_t> grain_sample;
    for (std::int32_t i = 0; i < grain_count; ++i) {
        const auto g = grains + std::size_t(i) * 8;
        if (r.u8(g + 3) != 1) continue;
        const auto tone = tones + (r.u32(g) & 0xffffff);
        in_head(tone, 0x14, "a tone");
        const auto offset = r.u32(tone + 0x10);
        grain_sample[std::size_t(i)] = offset;
        if (by_offset.contains(offset)) continue;
        BankSample s;
        s.offset = offset;
        s.centre_note = r.s8(tone + 2);
        s.centre_fine = r.s8(tone + 3);
        s.rate = bank_sample_rate(s.centre_note, s.centre_fine);
        by_offset.emplace(offset, s);
    }
    const auto names = sound_names(r, l);
    for (std::int32_t i = 0; i < sound_count; ++i) {
        const auto s = sounds + std::size_t(i) * 12;
        const auto count = r.s8(s + 4);
        const auto first = r.u32(s + 8);
        if (first % 8) continue;
        for (std::int32_t k = 0; k < count; ++k) {
            const auto it = grain_sample.find(first / 8 + std::size_t(k));
            if (it == grain_sample.end()) continue;
            const auto name = names.contains(i) ? names.at(i) : "#" + std::to_string(i);
            auto& users = by_offset.at(it->second).sounds;
            if (std::find(users.begin(), users.end(), name) == users.end()) users.push_back(name);
        }
    }
    std::vector<BankSample> out;
    for (auto& [offset, s] : by_offset) {
        if (offset % 16 || offset >= l.body_size)
            throw std::runtime_error("a tone sample at body offset " + hex(offset) + " outside the body");
        bool ended = false;
        for (std::uint32_t at = offset; at + 16 <= l.body_size; at += 16) {
            const auto flags = r.u8(l.body + at + 1);
            if (flags & 4) s.loop_block = (at - offset) / 16;
            ++s.blocks;
            if (flags & 1) {
                s.loops = (flags & 2) != 0;
                ended = true;
                break;
            }
        }
        if (!ended) throw std::runtime_error("the sample at body offset " + hex(offset) + " has no end block");
        if (!s.loops) s.loop_block = 0;
        s.index = out.size();
        out.push_back(std::move(s));
    }
    return out;
}

audio::PcmClip decode_bank_sample(std::span<const std::uint8_t> file, const BankSample& sample) {
    const Reader r{file};
    const auto l = layout(r);
    if (std::uint64_t(sample.offset) + std::uint64_t(sample.blocks) * 16 > l.body_size)
        throw std::runtime_error("the sample runs past the bank body");
    audio::PcmClip clip;
    clip.rate = sample.rate;
    clip.channels = 1;
    clip.repeat = sample.loops;
    clip.samples.resize(sample.frames());
    audio::AdpcmState state;
    for (std::uint32_t b = 0; b < sample.blocks; ++b)
        audio::decode_adpcm_frame(file.subspan(l.body + sample.offset + std::size_t(b) * 16, 16), state,
                                  clip.samples.data() + std::size_t(b) * 28);
    return clip;
}

std::vector<std::uint8_t> encode_adpcm(std::span<const std::int16_t> samples, bool loop) {
    const std::size_t blocks = std::max<std::size_t>(1, (samples.size() + 27) / 28);
    std::vector<std::uint8_t> out(blocks * 16, 0);
    std::int32_t s1 = 0, s2 = 0;
    std::array<std::int32_t, 28> x{};
    const auto run = [&](std::uint32_t f, std::uint32_t shift, std::int32_t& a, std::int32_t& b, std::uint8_t* nibbles) {
        const std::int32_t step = 1 << (12 - shift);
        std::int64_t err = 0;
        for (std::size_t i = 0; i < 28; ++i) {
            const std::int32_t pred = (a * filter0[f] + b * filter1[f] + 32) >> 6;
            const std::int32_t residual = x[i] - pred;
            std::int32_t q = residual >= 0 ? (residual + step / 2) / step : -((-residual + step / 2) / step);
            q = std::clamp(q, -8, 7);
            const std::int32_t s = std::clamp(pred + q * step, -32768, 32767);
            err += std::int64_t(x[i] - s) * (x[i] - s);
            b = a;
            a = s;
            if (nibbles) nibbles[2 + i / 2] |= std::uint8_t((q & 15) << ((i & 1) * 4));
        }
        return err;
    };
    for (std::size_t block = 0; block < blocks; ++block) {
        for (std::size_t i = 0; i < 28; ++i) {
            const auto at = block * 28 + i;
            x[i] = at < samples.size() ? samples[at] : 0;
        }
        std::int64_t best = std::numeric_limits<std::int64_t>::max();
        std::uint32_t best_f = 0, best_shift = 0;
        for (std::uint32_t f = 0; f < 5; ++f)
            for (std::uint32_t shift = 0; shift <= 12; ++shift) {
                std::int32_t a = s1, b = s2;
                const auto err = run(f, shift, a, b, nullptr);
                if (err < best) {
                    best = err;
                    best_f = f;
                    best_shift = shift;
                }
            }
        auto* frame = out.data() + block * 16;
        frame[0] = std::uint8_t(best_f << 4 | best_shift);
        run(best_f, best_shift, s1, s2, frame);
        std::uint8_t flags = loop ? 2 : 0;
        if (block == 0) flags |= 4;
        if (block + 1 == blocks) flags |= loop ? 3 : 1;
        frame[1] = flags;
    }
    return out;
}

}  // namespace omega::mods
