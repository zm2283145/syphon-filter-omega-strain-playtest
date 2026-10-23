#include "snd_bank.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace omega::snd989 {

namespace {

// .bss / .data globals (IRX vaddrs; the code reaches them as lui 2 + offset).
constexpr std::uint32_t lfo_list = 0x1d7f0;        // -0x2810 (29ac / 2428)
constexpr std::uint32_t lfo_count = 0x1d7f4;       // -0x280c
constexpr std::uint32_t registers = 0x1d810;       // -0x27f0: the global registers (index -code-6 / ~code)
constexpr std::uint32_t grain_table = 0x1d830;     // -0x27d0: the grain handlers by type
constexpr std::uint32_t bank_list = 0x1d990;       // -0x2670
constexpr std::uint32_t quiet = 0x1d90c;           // -0x26f4: 0 prints the 20e8 / lookup messages
constexpr std::uint32_t random_index = 0x1d9f0;    // -0x2610 (s16)
constexpr std::uint32_t random_table = 0x1d9f4;    // -0x260c (249 u16)
constexpr std::uint32_t countdown_a = 0x1e1bc;     // -0x1e44 (d598)
constexpr std::uint32_t countdown_b = 0x1e1c0;     // -0x1e40
constexpr std::uint32_t active_head = 0x1e1f0;     // -0x1e10
constexpr std::uint32_t active_tail = 0x1e1f4;     // -0x1e0c
constexpr std::uint32_t timestamp = 0x1e218;       // -0x1de8 (fc4c)
constexpr std::uint32_t stream_pool = 0x1e2ec;     // -0x1d14 (type-4 handles)
constexpr std::uint32_t sema = 0x1e3a4;            // -0x1c5c
constexpr std::uint32_t pending_ticks = 0x1e3a8;   // -0x1c58
constexpr std::uint32_t init_1c54 = 0x1e3ac;       // -0x1c54
constexpr std::uint32_t lock_held = 0x1e3b0;       // -0x1c50
constexpr std::uint32_t lock_owner = 0x1e3b4;      // -0x1c4c
constexpr std::uint32_t voice_mode = 0x1e3b8;      // -0x1c48
constexpr std::uint32_t voice_groups = 0x1e3bc;    // -0x1c44: 16 x {s16 lo, s16 hi}
constexpr std::uint32_t playing_list = 0x1e3fc;    // -0x1c04
constexpr std::uint32_t reserved_prio = 0x1e400;   // -0x1c00 (per core)
constexpr std::uint32_t reserved_voice = 0x1e408;  // -0x1bf8 (per core)
constexpr std::uint32_t reserved_flag = 0x1e410;   // -0x1bf0 (per core)
constexpr std::uint32_t mask_1abc = 0x1e544;       // -0x1abc
constexpr std::uint32_t reverb_mask = 0x1e54c;     // -0x1ab4 (VMIXEL / VMIXER)
constexpr std::uint32_t pending_kon = 0x1e554;     // -0x1aac
constexpr std::uint32_t pending_koff = 0x1e55c;    // -0x1aa4
constexpr std::uint32_t keyed_on = 0x1e564;        // -0x1a9c
constexpr std::uint32_t keyed_off = 0x1e56c;       // -0x1a94
constexpr std::uint32_t init_1e3c = 0x1e1c4;       // -0x1e3c
constexpr std::uint32_t counters = 0x24dec;        // voice counter bytes
constexpr std::uint32_t busy = 0x1e2a8;            // -0x1d58 (f884 re-entry)
constexpr std::uint32_t watchdog = 0x1d92c;        // -0x26d4 (.data 0)
// 5db0's nested command structs live on the IOP stack; the host keeps them
// here (0x40 a level), outside every IRX structure and bank head.
constexpr std::uint32_t cmd_stack = 0x2e000;
constexpr std::int32_t cmd_stack_levels = 0x7c;

constexpr std::uint32_t handler_size = 0x124;
constexpr std::uint32_t voice_size = 0x38;
constexpr std::int32_t voice_callback_6ad4 = 0x6ad4;
constexpr std::uint32_t sblk = 0x6b6c4253;  // 'SBlk'

[[noreturn]] void unhosted(const char* where, const std::string& what) {
    throw std::runtime_error(std::string("989snd ") + where + " not hosted: " + what);
}

std::int32_t sx16(std::uint32_t v) { return static_cast<std::int16_t>(v & 0xffff); }
std::int32_t sx8(std::uint32_t v) { return static_cast<std::int8_t>(v & 0xff); }

// div by a constant through the IRX's multiply-high sequences (signed truncation).
std::int32_t sdiv(std::int32_t a, std::int32_t b) { return b == 0 ? 0 : a / b; }

constexpr std::array<std::uint16_t, 12> semitone_table = {  // LIBSD 0x5170
    32768, 34716, 36780, 38967, 41285, 43740, 46340, 49096, 52015, 55108, 58385, 61857};
constexpr std::array<std::uint16_t, 128> fine_table = {  // LIBSD 0x5188
    32768, 32782, 32797, 32812, 32827, 32842, 32856, 32871, 32886, 32901, 32916, 32931, 32945, 32960, 32975, 32990,
    33005, 33020, 33035, 33050, 33065, 33080, 33094, 33109, 33124, 33139, 33154, 33169, 33184, 33199, 33214, 33229,
    33244, 33259, 33274, 33289, 33304, 33319, 33334, 33349, 33364, 33379, 33394, 33410, 33425, 33440, 33455, 33470,
    33485, 33500, 33515, 33530, 33546, 33561, 33576, 33591, 33606, 33621, 33636, 33652, 33667, 33682, 33697, 33712,
    33728, 33743, 33758, 33773, 33789, 33804, 33819, 33834, 33850, 33865, 33880, 33896, 33911, 33926, 33941, 33957,
    33972, 33987, 34003, 34018, 34033, 34049, 34064, 34080, 34095, 34110, 34126, 34141, 34157, 34172, 34187, 34203,
    34218, 34234, 34249, 34265, 34280, 34296, 34311, 34327, 34342, 34358, 34373, 34389, 34404, 34420, 34435, 34451,
    34466, 34482, 34497, 34513, 34528, 34544, 34560, 34575, 34591, 34606, 34622, 34638, 34653, 34669, 34685, 34700};

}  // namespace

std::uint16_t note_to_pitch(std::uint16_t center_note, std::uint16_t center_fine, std::uint16_t note,
                            std::int16_t fine) {
    std::int32_t a3 = std::int32_t(fine) + std::int32_t(center_fine);
    const std::int32_t whole = (a3 < 0 ? a3 + 0x7f : a3) >> 7;
    const std::int32_t offset = sx16(std::uint32_t(note) + std::uint32_t(whole) - center_note);
    a3 -= whole * 128;
    const std::int32_t octave = offset / 12;
    std::int32_t shift = octave - 2;
    std::int32_t semi = offset - octave * 12;
    bool adjust = false;
    if (sx16(std::uint32_t(semi)) < 0) {
        semi += 12;
        shift = octave - 3;
        adjust = a3 < 0;
    } else if (sx16(std::uint32_t(semi)) == 0) {
        if (a3 < 0) {
            semi += 12;
            shift = octave - 3;
            adjust = true;
        }
    } else {
        adjust = a3 < 0;
    }
    if (adjust) {
        semi = semi - 1 + whole;
        a3 += (whole + 1) * 128;
    }
    const std::int32_t i1 = sx16(std::uint32_t(semi));
    const std::uint32_t t1 = semitone_table.at(std::size_t(i1));
    const std::uint32_t t2 = fine_table.at(std::size_t(a3));
    std::int32_t p = static_cast<std::int32_t>(t1 * t2) >> 16;
    const std::int32_t s = sx16(std::uint32_t(shift));
    if (s < 0) {
        const std::int32_t n = -s;
        p = static_cast<std::int32_t>((std::uint32_t(p) + (1u << (n - 1))) >> n);
    }
    return static_cast<std::uint16_t>(p);
}

IrxSection irx_section(std::span<const std::uint8_t> irx, const std::string& name) {
    const auto u16 = [&](std::size_t o) {
        if (o + 2 > irx.size()) throw std::runtime_error("989snd: IRX header outside the file");
        return std::uint32_t(irx[o] | (irx[o + 1] << 8));
    };
    const auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (irx.size() < 0x34 || irx[0] != 0x7f || irx[1] != 'E' || irx[2] != 'L' || irx[3] != 'F')
        throw std::runtime_error("989snd: not an IRX (ELF) file");
    const std::size_t shoff = u32(0x20), shnum = u16(0x30), shstr = u16(0x32);
    const auto header = [&](std::size_t i) { return shoff + i * 40; };
    if (shstr >= shnum) throw std::runtime_error("989snd: IRX without a section name table");
    const std::size_t names = u32(header(shstr) + 0x10);
    for (std::size_t i = 0; i < shnum; ++i) {
        const auto h = header(i);
        std::size_t p = names + u32(h);
        std::string n;
        while (p < irx.size() && irx[p]) n += char(irx[p++]);
        if (n != name) continue;
        const std::size_t off = u32(h + 0x10), size = u32(h + 0x14);
        if (off + size > irx.size()) throw std::runtime_error("989snd: IRX section " + name + " outside the file");
        return {u32(h + 0xc), irx.subspan(off, size)};
    }
    throw std::runtime_error("989snd: IRX section " + name + " missing");
}

BankEngine::BankEngine(std::uint32_t rodata_addr, std::span<const std::uint8_t> rodata, std::uint32_t data_addr,
                       std::span<const std::uint8_t> data, SpuInterface& spu, VolumeState& volume)
    : mem_(memory_size), spu_(spu), volume_(volume) {
    if (rodata_addr + rodata.size() > memory_size || data_addr + data.size() > memory_size)
        throw std::runtime_error("989snd: IRX sections outside the bank engine memory");
    std::copy(rodata.begin(), rodata.end(), mem_.begin() + rodata_addr);
    std::copy(data.begin(), data.end(), mem_.begin() + data_addr);
}

std::uint16_t BankEngine::r16(std::uint32_t a) const {
    return static_cast<std::uint16_t>(mem_[a & mask] | (mem_[(a + 1) & mask] << 8));
}
std::uint32_t BankEngine::r32(std::uint32_t a) const {
    return std::uint32_t(r16(a)) | (std::uint32_t(r16(a + 2)) << 16);
}
void BankEngine::w16(std::uint32_t a, std::uint32_t v) {
    mem_[a & mask] = static_cast<std::uint8_t>(v);
    mem_[(a + 1) & mask] = static_cast<std::uint8_t>(v >> 8);
}
void BankEngine::w32(std::uint32_t a, std::uint32_t v) {
    w16(a, v);
    w16(a + 2, v >> 16);
}

// 182cc: the voice reset at init (the sema 1b948 is the host's: id 1).
void BankEngine::init_voices() {
    w32(init_1c54, 0);
    w32(pending_ticks, 0);
    for (std::int32_t v = 0; v < 48; ++v) {
        voice_reset(v);
        w32(voices + std::uint32_t(v) * voice_size + 4, std::uint32_t(v));
        w8(counters + std::uint32_t(v), 0);
    }
    for (const auto m : {pending_kon, pending_koff, keyed_on, keyed_off, reverb_mask, mask_1abc}) {
        w32(m, 0);
        w32(m + 4, 0);
    }
    w32(voice_mode, 0);
    w32(mask_1abc, 0);
    w32(init_1e3c, 0);
    w32(playing_list, 0);
    for (std::uint32_t g = 0; g < 16; ++g) {
        w16(voice_groups + g * 4, 0);
        w16(voice_groups + g * 4 + 2, 0x2f);
    }
    w32(sema, 1);
    sema_count_ = 1;
}

// 20e8 with a null error callback: the code is what the IRX reports.
void BankEngine::error(std::int32_t code) { errors_.push_back(code); }

// ba74: the additive lagged-Fibonacci generator (249 entries, lag 103).
std::uint32_t BankEngine::random() {
    const std::int32_t i = s16(random_index);
    const std::int32_t j = i < 0x93 ? i + 0x67 : i - 0x93;
    const std::uint32_t ai = random_table + std::uint32_t(i) * 2;
    const std::uint32_t v = r16(ai) ^ r16(random_table + std::uint32_t(j) * 2);
    w16(ai, v);
    w16(random_index, i < 0xf9 ? i + 1 : 0);
    return v & 0xffff;
}

// 18538: a0 != 0 WaitSema (the IOP has one thread here: a held lock would
// never be released), a0 == 0 PollSema (-0x1a3 when held: returns 0); 1860c.
std::int32_t BankEngine::voice_lock(std::int32_t wait, std::int32_t owner) {
    if (sema_count_ == 0) {
        if (wait) unhosted("00018578", "WaitSema on the held voice semaphore (owner " + std::to_string(s32(lock_owner)) + ")");
        return 0;
    }
    sema_count_ = 0;
    w32(lock_owner, std::uint32_t(owner));
    w32(lock_held, 1);
    return 1;
}
void BankEngine::voice_unlock() {
    w32(lock_held, 0);
    w32(lock_owner, 0);
    sema_count_ = 1;
}

std::uint32_t BankEngine::voice_addr(std::int32_t v) const { return voices + std::uint32_t(v) * voice_size; }

// 1a10c: the voice joins the playing list (state 1).
void BankEngine::voice_play(std::int32_t v) {
    const auto V = voice_addr(v);
    w32(V + 8, 1);
    w32(V, 0);
    std::uint32_t p = r32(playing_list);
    if (p == 0) {
        w32(playing_list, V);
        return;
    }
    if (p == V) {
        error(0x52);
        return;
    }
    std::uint32_t n = r32(p);
    while (n != 0 && n != V) {
        p = n;
        n = r32(p);
    }
    if (n == V) {
        error(0x52);
        return;
    }
    w32(p, V);
}

// 1a1c8: the voice leaves the playing list (state 0) and its core's reservation.
void BankEngine::voice_reset(std::int32_t v) {
    const auto V = voice_addr(v);
    w32(V + 8, 0);
    w32(V + 0xc, 0);
    const std::uint32_t core = std::uint32_t(v / 24);
    if (r32(playing_list) == V) {
        w32(playing_list, r32(V));
    } else {
        std::uint32_t p = r32(playing_list);
        while (p != 0 && r32(p) != V) p = r32(p);
        if (p != 0) w32(p, r32(V));
    }
    if (s32(reserved_voice + core * 4) == v) {
        w32(reserved_voice + core * 4, 0xffffffff);
        w32(reserved_prio + core * 4, 0xffffffff);
        w32(reserved_flag + core * 4, 0);
    }
}

// 18760: volumes to 0 and the voice's NON bit (a1: cleared).
void BankEngine::voice_list(std::int32_t v, std::int32_t clear) {
    const std::uint32_t core = std::uint32_t(v / 24), vi = std::uint32_t(v % 24);
    const auto entry = static_cast<std::uint16_t>(core | vi << 1);
    spu_.set_param(entry, 0);
    spu_.set_param(static_cast<std::uint16_t>(entry | 0x100), 0);
    const auto non = static_cast<std::uint16_t>(core | 0x1400);
    const auto bits = spu_.get_switch(non);
    spu_.set_switch(non, clear ? bits & ~(1u << vi) : bits | (1u << vi));
}

// 18a58: a free voice of the group's range, else the lowest priority, oldest voice.
std::int32_t BankEngine::alloc_voice(std::int32_t group, std::int32_t prio, std::int32_t flag) {
    if (!(group < 0x10)) group = 0xf;
    const std::int32_t lo = s16(voice_groups + std::uint32_t(group) * 4);
    const std::int32_t end = s16(voice_groups + std::uint32_t(group) * 4 + 2) + 1;
    const auto take = [&](std::int32_t v) {
        const auto V = voice_addr(v);
        w32(V + 8, 2);
        w32(V + 0x1c, r32(timestamp));
        w8(V + 0x1a, std::uint32_t(prio));
        w8(V + 0x1b, std::uint32_t(group));
        w32(V + 0x28, flag ? 1 : 0);
    };
    for (std::int32_t v = lo; v < end; ++v) {
        if (r32(voice_addr(v) + 8) != 0) continue;
        take(v);
        w8(counters + std::uint32_t(v), 4);
        voice_list(v, 1);
        return v;
    }
    std::int32_t best = lo;
    for (std::int32_t v = lo + 1; v < end; ++v) {
        const auto V = voice_addr(v), B = voice_addr(best);
        if (r32(V + 8) == 3 || (r32(V + 0x28) & 1)) continue;
        const auto p = r8(V + 0x1a), bp = r8(B + 0x1a);
        if (p < bp || (p == bp && r32(V + 0x1c) < r32(B + 0x1c))) best = v;
    }
    const auto B = voice_addr(best);
    if (r32(B + 0x28) & 1) return -1;
    if (prio < std::int32_t(r8(B + 0x1a))) return -1;
    if (r8(B + 0x1a) >= 0x7e) return -1;
    if (r32(B + 8) == 3) return -1;
    if (r32(B + 0x10)) voice_callback(best, 1);
    voice_reset(best);
    w32(B + 8, 2);
    w32(B + 0x1c, r32(timestamp));
    w8(B + 0x1a, std::uint32_t(prio));
    w32(B + 0x28, flag ? 1 : 0);
    w8(B + 0x1b, std::uint32_t(group));
    w32(B + 0x10, 0);
    w8(counters + std::uint32_t(best), 4);
    voice_list(best, 1);
    return best;
}

// The voice's +10 callback with (voice, +c, code): only 6ad4 is installed by the bank player.
void BankEngine::voice_callback(std::int32_t v, std::int32_t) {
    const auto V = voice_addr(v);
    const auto fn = r32(V + 0x10);
    if (fn != std::uint32_t(voice_callback_6ad4)) unhosted("voice callback", "a voice callback other than 00006ad4");
    voice_done(v, r32(V + 0xc));
}

// 1a070: the note offset of a voice in 1/128 semitones, split into note and fine.
void BankEngine::note_pitch_offset(std::uint32_t tone, std::int32_t bend, std::int32_t b, std::int32_t c,
                                   std::int32_t d, std::int32_t& out1, std::int32_t& out2) const {
    std::int32_t x = c * 128 + d + b;
    if (bend < 0) {
        std::int32_t v = s8(tone + 8) * (bend * 128);
        if (v < 0) v += 0x7fff;
        x += v >> 15;
    } else {
        x += sdiv(s8(tone + 9) * (bend * 128), 0x7fff);
    }
    out1 = (x < 0 ? x + 0x7f : x) >> 7;
    out2 = x - out1 * 128;
}

// 19fd0: sceSdNote2Pitch on the tone's centre; a non-negative centre note is a 44.1 kHz sample.
std::uint16_t BankEngine::tone_pitch(std::int32_t center_note, std::int32_t center_fine, std::uint16_t note,
                                     std::int16_t fine) const {
    std::int32_t n = sx8(std::uint32_t(center_note));
    const bool scale = n >= 0;
    if (!scale) n = sx8(std::uint32_t(-n));
    std::uint32_t p = note_to_pitch(static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(sx8(std::uint32_t(center_fine))),
                                    note, fine);
    if (scale) {
        const std::uint32_t v = p * 44100u;
        p = static_cast<std::uint32_t>((std::uint64_t(v) * 0x057619f1u) >> 32) >> 10;
    }
    return static_cast<std::uint16_t>(p);
}

// 18d60: volumes through AdjustVolToGroup, the dry mix, pitch, start address, ADSR, reverb, then KON pending.
void BankEngine::key_on(std::int32_t v, std::int32_t noise) {
    const std::uint32_t core = std::uint32_t(v / 24), vi = std::uint32_t(v % 24), bit = 1u << vi;
    const auto V = voice_addr(v);
    w32(V + 0x1c, r32(timestamp));
    const auto entry = static_cast<std::uint16_t>(core | vi << 1);
    spu_.set_param(entry, static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x20), s8(V + 0x1b))) >> 1));
    spu_.set_param(static_cast<std::uint16_t>(entry | 0x100),
                   static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x22), s8(V + 0x1b))) >> 1));
    const auto tone = r32(V + 0x14);
    for (const std::uint16_t sw : {std::uint16_t(core | 0x1800), std::uint16_t(core | 0x1a00)}) {
        const auto bits = spu_.get_switch(sw);
        spu_.set_switch(sw, (r16(tone + 0xe) & 0x10) ? bits & ~bit : bits | bit);
    }
    if (noise) unhosted("00018fa0", "a noise voice");
    const auto non = static_cast<std::uint16_t>(core | 0x1400);
    spu_.set_switch(non, spu_.get_switch(non) & ~bit);
    std::int32_t o1 = 0, o2 = 0;
    note_pitch_offset(tone, s16(V + 0x24), s16(V + 0x26), s8(V + 0x18), s8(V + 0x19), o1, o2);
    spu_.set_param(static_cast<std::uint16_t>(entry | 0x200),
                   tone_pitch(s8(tone + 2), s8(tone + 3), static_cast<std::uint16_t>(o1), static_cast<std::int16_t>(o2)));
    spu_.set_addr(static_cast<std::uint16_t>(entry | 0x2040), r32(tone + 0x10));
    spu_.set_param(static_cast<std::uint16_t>(entry | 0x300), r16(tone + 0xa));
    spu_.set_param(static_cast<std::uint16_t>(entry | 0x400), r16(tone + 0xc));
    const auto rv = reverb_mask + core * 4;
    w32(rv, (r16(tone + 0xe) & 1) ? r32(rv) | bit : r32(rv) & ~bit);
    w32(pending_kon + core * 4, r32(pending_kon + core * 4) | bit);
    voice_play(v);
}

// 19264: the reverb masks and KON of the pending voices, then keyed-on.
void BankEngine::flush_key_on() {
    if (r32(pending_kon) == 0 && r32(pending_kon + 4) == 0) return;
    spu_.set_switch(0x1900, r32(reverb_mask));
    spu_.set_switch(0x1b00, r32(reverb_mask));
    spu_.set_switch(0x1901, r32(reverb_mask + 4));
    spu_.set_switch(0x1b01, r32(reverb_mask + 4));
    spu_.set_switch(0x1500, r32(pending_kon));
    spu_.set_switch(0x1501, r32(pending_kon + 4));
    const auto a = r32(pending_kon), b = r32(pending_kon + 4);
    w32(pending_kon + 4, 0);
    w32(keyed_off, r32(keyed_off) & ~a);
    w32(pending_kon, 0);
    w32(keyed_off + 4, r32(keyed_off + 4) & ~b);
    w32(keyed_on, r32(keyed_on) | a);
    w32(keyed_on + 4, r32(keyed_on + 4) | b);
}

// 1937c: KOFF of the pending voices, then keyed-off.
void BankEngine::flush_key_off() {
    if (r32(pending_koff) == 0 && r32(pending_koff + 4) == 0) return;
    spu_.set_switch(0x1600, r32(pending_koff));
    spu_.set_switch(0x1601, r32(pending_koff + 4));
    const auto a = r32(pending_koff), b = r32(pending_koff + 4);
    w32(pending_koff + 4, 0);
    w32(keyed_on, r32(keyed_on) & ~a);
    w32(pending_koff, 0);
    w32(keyed_on + 4, r32(keyed_on + 4) & ~b);
    w32(keyed_off, r32(keyed_off) | a);
    w32(keyed_off + 4, r32(keyed_off + 4) | b);
}

// 1948c: a voice whose envelope reached 0 after its key off.
void BankEngine::voice_ended(std::int32_t v) {
    const auto V = voice_addr(v);
    if (r8(V + 0x1a) != 0x7f) voice_reset(v);
    if (r32(V + 0x10)) voice_callback(v, 2);
    w32(V + 0x10, 0);
    const std::uint32_t core = std::uint32_t(v / 24);
    w32(keyed_off + core * 4, r32(keyed_off + core * 4) & ~(1u << (v % 24)));
}

// 19620: a playing voice silent for its four counter ticks: key it off.
void BankEngine::voice_silent(std::int32_t v) {
    const auto V = voice_addr(v);
    if (r32(V + 8) == 1 && r32(V + 0x10)) voice_callback(v, 3);
    w32(V + 0x10, 0);
    const std::uint32_t core = std::uint32_t(v / 24), bit = 1u << (v % 24);
    const auto kon = pending_kon + core * 4;
    if (r32(kon) & bit) w32(kon, r32(kon) & ~bit);
    w32(pending_koff + core * 4, r32(pending_koff + core * 4) | bit);
}

// 1a334: the voice's KON dropped, KOFF pending.
void BankEngine::key_off_pending(std::int32_t core, std::int32_t vi) {
    const std::uint32_t bit = 1u << vi;
    const auto kon = pending_kon + std::uint32_t(core) * 4, koff = pending_koff + std::uint32_t(core) * 4;
    w32(kon, r32(kon) & ~bit);
    w32(koff, r32(koff) | bit);
}

// 19a94: VOLL / VOLR / PITCH zero for the masks' voices.
void BankEngine::silence(std::uint32_t masks) {
    for (std::int32_t v = 0; v < 48; ++v) {
        const std::uint32_t core = std::uint32_t(v / 24), vi = std::uint32_t(v % 24);
        if (!(r32(masks + core * 4) & (1u << vi))) continue;
        const auto entry = static_cast<std::uint16_t>(core | vi << 1);
        spu_.set_param(entry, 0);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x100), 0);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x200), 0);
    }
}

// e6a8: the next voice >= from in a handler's masks.
std::int32_t BankEngine::next_voice(std::uint32_t h, std::int32_t from) const {
    for (std::int32_t v = from; v < 0x30; ++v)
        if (r32(h + 0x18 + std::uint32_t(v / 24) * 4) & (1u << (v % 24))) return v;
    return -1;
}

// 6cbc: the tone's voice priority, lowered with distance (volume) for flags 2 / 4.
std::int32_t BankEngine::priority(std::int32_t vol, std::uint32_t tone) const {
    std::int32_t p = s8(tone);
    const auto flags = r16(tone + 0xe);
    if (!(flags & 6)) return p;
    const auto mode = (flags >> 1) & 3;
    std::int32_t a = p;
    if (mode == 1) a = p / 5;
    else if (mode == 2) a = p / 2;
    // slti 0x3b7, then a multiply by 0x44fc3a35 >> 40: a truncating divide by 950, not 951.
    if (a != 0 && vol < 0x3b7) p -= a - (a * vol) / 950;
    return sx8(std::uint32_t(p));
}

// 6c28: the pan pulled toward the front with distance.
std::int32_t BankEngine::distance_pan(std::int32_t pan, std::int32_t vol) const {
    if (vol >= 0x356) return pan;
    if (!(0xaa < vol)) return 0;
    if (pan < 0xb5) return (pan * (vol - 0xaa)) / 0x2ac;
    return 0x168 - ((0x168 - pan) * (vol - 0xaa)) / 0x2ac;
}

// 6ad4: a voice of the handler ended; a finished handler (and its finished parent) are freed.
void BankEngine::voice_done(std::int32_t v, std::uint32_t h) {
    const auto m = h + 0x18 + std::uint32_t(v / 24) * 4;
    w32(m, r32(m) & ~(1u << (v % 24)));
    if (r32(h + 0x18) || r32(h + 0x1c) || s16(h + 0x4a) != -1) return;
    if (r32(h + 0x28) == 0 && r32(h + 0x2c) != 0) return;
    const auto parent = r32(h + 0x28);
    unlink_lfos(h);
    free_handler(h, 0);
    if (parent && r32(parent + 0x2c) == 0 && r32(parent + 0x18) == 0 && r32(parent + 0x1c) == 0 &&
        s16(parent + 0x4a) == -1) {
        unlink_lfos(parent);
        free_handler(parent, 0);
    }
}

// 6764: a pitch bend on a handle and its children.
void BankEngine::set_bend(std::uint32_t handle, std::int32_t bend) {
    bend = sx16(std::uint32_t(bend));
    const auto c = lookup(handle);
    if (!c) return;
    for (auto k = r32(c + 0x2c); k; k = r32(k + 0x30)) set_bend(r32(k), bend);
    w16(c + 0x42, std::uint32_t(bend));
    apply_bend(c);
}

// 685c: the handler's bend into its voices' pitch.
void BankEngine::apply_bend(std::uint32_t h) {
    w16(h + 0x14, r16(h + 0x46) + r16(h + 0x44));
    const std::int32_t b = s16(h + 0x42) + s16(h + 0x40);
    w16(h + 0x36, std::uint32_t(std::clamp(b, -0x8000, 0x7fff)));
    const auto locked = voice_lock(1, 0x1c);
    for (auto v = next_voice(h, 0); v != -1; v = next_voice(h, v + 1)) {
        const auto V = voice_addr(v);
        w16(V + 0x24, r16(h + 0x36));
        w16(V + 0x26, r16(h + 0x14));
        if (r8(h + 0x16) & 2) continue;
        const auto tone = r32(V + 0x14);
        if (r16(tone + 0xe) & 8) unhosted("000069d8", "a noise voice bend");
        std::int32_t o1 = 0, o2 = 0;
        note_pitch_offset(tone, s16(h + 0x36), s16(h + 0x14), s8(h + 0x34), s8(h + 0x35), o1, o2);
        const auto p = tone_pitch(s8(tone + 2), s8(tone + 3), static_cast<std::uint16_t>(o1), static_cast<std::int16_t>(o2));
        spu_.set_param(static_cast<std::uint16_t>(std::uint32_t(v / 24) | std::uint32_t(v % 24) << 1 | 0x200), p);
    }
    if (locked) voice_unlock();
}

// 19740: the masks' playing voices (state 1) to the pending KOFF; their callback (code 3) when asked.
void BankEngine::key_off(std::uint32_t masks, std::int32_t callback) {
    for (std::int32_t v = 0; v < 48; ++v) {
        const std::uint32_t core = std::uint32_t(v / 24), bit = 1u << (v % 24);
        if (!(r32(masks + core * 4) & bit)) continue;
        const auto V = voice_addr(v);
        if (r32(V + 8) != 1) continue;
        w32(pending_koff + core * 4, r32(pending_koff + core * 4) | bit);
        if (callback && r32(V + 0x10)) voice_callback(v, 3);
        w32(V + 0x10, 0);
        const auto kon = pending_kon + core * 4;
        if (r32(kon) & bit) w32(kon, r32(kon) & ~bit);
    }
}

// 198f8: the masks' voices silenced at once (volumes 0) and keyed off.
void BankEngine::key_off_now(std::uint32_t masks, std::int32_t callback) {
    for (std::int32_t v = 0; v < 48; ++v) {
        const std::uint32_t core = std::uint32_t(v / 24), vi = std::uint32_t(v % 24);
        if (!(r32(masks + core * 4) & (1u << vi))) continue;
        const auto V = voice_addr(v);
        if (callback && r32(V + 8) == 1 && r32(V + 0x10)) voice_callback(v, 3);
        w32(V + 0x10, 0);
        if (r8(V + 0x1a) != 0x7f) voice_reset(v);
        const auto entry = static_cast<std::uint16_t>(core | vi << 1);
        spu_.set_param(entry, 0);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x100), 0);
        key_off_pending(std::int32_t(core), std::int32_t(vi));
    }
}

// 23e8: the handler's four LFO slots (+5c + i * 0x30) off and out of the LFO list (2428).
void BankEngine::unlink_lfos(std::uint32_t h) {
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto lfo = h + 0x5c + i * 0x30;
        w8(lfo + 5, r8(lfo + 5) & 0xfe);
        auto p = r32(lfo_list);
        if (p == lfo) {
            w32(lfo_list, r32(lfo + 0x2c));
            w32(lfo_count, r32(lfo_count) - 1);
            continue;
        }
        while (p && r32(p + 0x2c) != lfo) p = r32(p + 0x2c);
        if (p) {
            w32(p + 0x2c, r32(r32(p + 0x2c) + 0x2c));
            w32(lfo_count, r32(lfo_count) - 1);
        }
    }
}

// d750: the first free entry (+4 == 0) of a pool, its handle word advanced (serial | index << 16 | type << 24).
std::uint32_t BankEngine::pool_alloc(std::uint32_t pool, std::int32_t count, std::int32_t type, std::uint32_t stride) {
    for (std::int32_t i = 0; i < count; ++i) {
        const auto e = pool + std::uint32_t(i) * stride;
        if (r32(e + 4) != 0) continue;
        for (const std::uint32_t o : {0x24u, 0x20u, 0x28u, 0x2cu, 0x30u}) w32(e + o, 0);
        w8(e + 0x16, 0);
        w32(e, ((r16(e) + 1u) & 0xffff) | std::uint32_t(i) << 16 | std::uint32_t(type) << 24);
        return e;
    }
    return 0;
}

// d82c: the sound's instance limit (s8 +5) over the active handlers; `out` the one to steal.
std::int32_t BankEngine::instance_limit(std::uint32_t sound, std::int32_t vol, std::int32_t inst, std::uint32_t& out) {
    const std::int32_t limit = s8(sound + 5);
    if (limit == 0) return 0;
    out = 0;
    std::int32_t count = 0;
    std::uint32_t cand = 0;
    const auto flags = r16(sound + 6);
    for (auto t = r32(active_head); t; t = r32(t + 0x24)) {
        if (((r32(t) >> 24) & 0x1f) != 5) continue;
        const bool match = inst ? (r32(t + 0x120) == sound || r32(t + 4) == sound) : r32(t + 4) == sound;
        if (!match) continue;
        ++count;
        if (!cand) {
            cand = t;
        } else if (((flags & 0x10) && s16(t + 0x3a) < s16(cand + 0x3a)) ||
                   ((flags & 0x20) && r32(t + 0x11c) < r32(cand + 0x11c))) {
            cand = t;
        }
    }
    if (count < limit) return 1;
    if (!cand) return 0;
    if (((flags & 0x10) && s16(cand + 0x3a) < vol) || (flags & 0x20)) {
        out = cand;
        return 1;
    }
    return 0;
}

// d968: a handler for the sound (after its instance limit, stealing the chosen one).
std::uint32_t BankEngine::alloc_handler(std::uint32_t sound, std::int32_t vol, std::int32_t inst) {
    if (r16(sound + 6) & 8) {
        std::uint32_t out = 0;
        if (!instance_limit(sound, vol, inst, out)) return 0;
        if (out) stop_handler(out, 1, 0, 1);
    }
    return pool_alloc(handlers, 0x40, 5, handler_size);
}

// d9dc: active (bit 31) and appended to the active list.
void BankEngine::activate(std::uint32_t h) {
    w32(h + 0x20, 0);
    w32(h + 0x24, 0);
    w32(h, r32(h) | 0x80000000u);
    const auto tail = r32(active_tail);
    if (tail == 0) {
        w32(active_head, h);
    } else {
        if (tail == h) unhosted("0000da3c", "a handler activated twice");
        w32(tail + 0x24, h);
        w32(h + 0x20, tail);
    }
    w32(active_tail, h);
}

// da80: a handler (and with `force` its children) freed; its children move to its parent.
void BankEngine::free_handler(std::uint32_t h, std::int32_t force) {
    if (!force && r32(h + 0x28) == 0 && r32(h + 0x2c) != 0) {
        error(0x78);
        force = 1;
    }
    if (force)
        while (r32(h + 0x2c)) free_handler(r32(h + 0x2c), force);
    if (r32(h + 8)) {
        effect_free_chain(r32(h + 8));
        w32(h + 8, 0);
    }
    const auto prev = r32(h + 0x20), next = r32(h + 0x24);
    if (prev) {
        if (r32(prev + 0x24) != h) unhosted("0000db50", "a broken active list (eb3c)");
        w32(prev + 0x24, next);
    } else {
        w32(active_head, next);
    }
    if (next) {
        if (r32(next + 0x20) != h) unhosted("0000dba0", "a broken active list (eb3c)");
        w32(next + 0x20, r32(h + 0x20));
    } else {
        w32(active_tail, r32(h + 0x20));
    }
    const auto parent = r32(h + 0x28);
    if (parent) {
        if (r32(parent + 0x2c) == h) {
            w32(parent + 0x2c, r32(h + 0x30));
        } else {
            auto p = r32(parent + 0x2c);
            while (p && r32(p + 0x30) != h) p = r32(p + 0x30);
            if (!p) {
                error(0x74);
                return;
            }
            w32(p + 0x30, r32(h + 0x30));
        }
        for (auto k = r32(h + 0x2c); k; k = r32(k + 0x30)) w32(k + 0x28, r32(h + 0x28));
        if (r32(parent + 0x2c) == 0) {
            w32(parent + 0x2c, r32(h + 0x2c));
        } else {
            auto p = r32(parent + 0x2c);
            while (r32(p + 0x30)) {
                p = r32(p + 0x30);
                if (!p) break;
            }
            w32(p + 0x30, r32(h + 0x2c));
        }
    }
    w32(h + 4, 0);
    w32(h, r32(h) & 0x7fffffffu);
}

// dd44: the handle becomes the last child of `parent`.
void BankEngine::add_child(std::uint32_t parent, std::uint32_t handle) {
    const auto c = lookup(handle);
    if (!c) return;
    if (c == parent || r32(c + 0x28) != 0) {
        error(0x75);
        return;
    }
    w32(c + 0x28, parent);
    auto p = r32(parent + 0x2c);
    if (!p) {
        w32(parent + 0x2c, c);
        return;
    }
    while (r32(p + 0x30)) p = r32(p + 0x30);
    w32(p + 0x30, c);
}

// e3e4: stop a handler (its children first when asked): its grains released, its voices keyed off.
void BankEngine::stop_handler(std::uint32_t h, std::int32_t children, std::int32_t now, std::int32_t a3) {
    if (s32(h) >= 0) return;
    std::int32_t done = 1, stopping = 0;
    const auto f = r8(h + 0x16);
    if (f & 4) {
        stopping = 1;
        children = 1;
    }
    w8(h + 0x16, f | 4);
    if (children) {
        for (auto k = r32(h + 0x2c); k;) {
            stop_handler(k, children, now, a3);
            k = r32(k + 0x30);
        }
    }
    const auto type = (r32(h) >> 24) & 0x1f;
    if (type == 4) unhosted("000119f4", "a stream handle stop in the bank engine");
    if (type == 2) unhosted("0000e514", "a type-2 handle stop");
    if (type == 5) {
        if (stopping) release(h, 1, a3);
        else if (release(h, now, a3) != 0) done = 0;
    }
    if (done) {
        voice_lock(1, 0x4a);
        if (now || (r8(h + 0x16) & 2)) key_off_now(h + 0x18, 1);
        else key_off(h + 0x18, 1);
        voice_unlock();
        if (s32(h) < 0) free_handler(h, 0);
    }
}

// df4c: e3e4 on a handle (type 5: the handler pool; type 4 streams are the host's).
void BankEngine::stop_handle(std::uint32_t handle, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    const auto type = (handle >> 24) & 0x1f;
    if (type == 4) unhosted("0000df98", "a stream handle in the bank engine");
    if (type != 5) return;
    stop_handler(handlers + ((handle >> 16) & 0xff) * handler_size, a1, a2, a3);
}

// e5d4: every active handler of the sound stopped.
void BankEngine::stop_sound(std::uint32_t sound, std::int32_t a1, std::int32_t a2) {
    for (auto h = r32(active_head); h; h = r32(h + 0x24))
        if (r32(h + 4) == sound) stop_handle(r32(h), 1, a1, a2);
}

// 4990: the index of the sound's release grain (type 0x2b), -1 if none or last.
std::int32_t BankEngine::release_grain(std::uint32_t h) const {
    const auto sound = r32(h + 4);
    const std::int32_t n = s8(sound + 4);
    const auto grains = r32(sound + 8);
    for (std::int32_t i = 0; i < n; ++i) {
        if (r8(grains + std::uint32_t(i) * 8 + 3) != 0x2b) continue;
        return i == n - 1 ? -1 : i;
    }
    return -1;
}

// 4838: the handler's release (its grains after the release grain), or its end.
std::int32_t BankEngine::release(std::uint32_t h, std::int32_t force, std::int32_t a2) {
    const auto i = release_grain(h);
    if (i == -1 || force) {
        unlink_lfos(h);
        if (r32(h + 0x2c) == 0) return 0;
        key_off(h + 0x18, 0);
        w32(h + 0x1c, 0);
        w32(h + 0x18, 0);
        w16(h + 0x4a, 0xffff);
        return 1;
    }
    key_off(h + 0x18, 0);
    w32(h + 0x1c, 0);
    w32(h + 0x18, 0);
    w16(h + 0x4a, std::uint32_t(i + 1));
    if (a2) w8(h + 0x16, r8(h + 0x16) | 0x20);
    while (s16(h + 0x4a) != -1)
        if (step(h) != 0) break;
    if (r32(h + 0x18) || r32(h + 0x1c) || r32(h + 0x2c)) return 1;
    unlink_lfos(h);
    return 0;
}

// The grain table 0x1d830 (.data): the handler of each grain type.
std::int32_t BankEngine::grain(std::uint32_t h, std::uint32_t sound, std::uint32_t g) {
    const std::uint32_t type = r8(g + 3);
    const auto fn = r32(grain_table + type * 4);
    switch (fn) {
    case 0x4a00:  // type 0: nothing
    case 0x5038:  // 20: nothing
    case 0x5040:  // 21: the loop start (5048 looks for it)
        return 0;
    case 0x4a08:  // 1 (and 9): a tone on a voice
        return tone_grain(h, sound, g);
    case 0x5db0:  // 5: a child sound
        return child_grain(h, g);
    case 0x5048: {  // 22: back to the grain after the last loop start
        for (std::int32_t i = s16(h + 0x4a) - 1; i >= 0; --i) {
            if (r8(r32(sound + 8) + std::uint32_t(i) * 8 + 3) == 21) {
                w16(h + 0x4a, std::uint32_t(i - 1));
                return 0;
            }
        }
        error(8);
        return -1;
    }
    case 0x51c0: {  // 25: a random one of g[0] runs of g[1] grains, not the last one played
        if (s8(h + 0x50)) {
            error(0xa);
            return -1;
        }
        std::int32_t r = std::int32_t(random() % std::uint32_t(s8(g)));
        if (r == s8(g + 2)) ++r;
        if (r == s8(g)) r = 0;
        w8(g + 2, std::uint32_t(r));
        w16(h + 0x4a, std::uint32_t(s16(h + 0x4a) + r * s8(g + 1)));
        w16(h + 0x52, std::uint32_t(s8(g + 1) + 1));
        w16(h + 0x54, std::uint32_t((s8(g) - 1 - r) * s8(g + 1)));
        w8(h + 0x50, 1);
        return 0;
    }
    case 0x52d0:  // 26: a random extra delay
        return std::int32_t(random() % ((r32(g) & 0xffffff) + 1));
    case 0x531c: {  // 27: a random pitch bend of +-g[0] percent
        std::int32_t v = std::int32_t(random() % 0x7fff);
        v = (v * 0xffff) / 0x7fff - 0x8000;
        v = (v * s8(g)) / 100;
        set_bend(r32(h), sx16(std::uint32_t(v)));
        return 0;
    }
    case 0x551c: {  // 31: a random register value in [g[1], g[2]]
        const std::int32_t val = std::int32_t(random() % std::uint32_t(s8(g + 2) - s8(g + 1) + 1)) + s8(g + 1);
        const std::int32_t idx = s8(g);
        if (idx >= 0) w8(h + 0x4c + std::uint32_t(idx), std::uint32_t(val));
        else w8(registers + std::uint32_t(~idx), std::uint32_t(val));
        return 0;
    }
    case 0x593c: {  // 39: the next of g[0] runs of g[1] grains in turn
        if (s8(h + 0x50)) {
            error(0xd);
            return -1;
        }
        const std::int32_t old = s8(g + 2);
        w8(g + 2, old + 1 >= s8(g) ? 0 : std::uint32_t(old + 1));
        w16(h + 0x4a, std::uint32_t(s16(h + 0x4a) + old * s8(g + 1)));
        w16(h + 0x52, std::uint32_t(s8(g + 1) + 1));
        w16(h + 0x54, std::uint32_t((s8(g) - 1 - old) * s8(g + 1)));
        w8(h + 0x50, 1);
        return 0;
    }
    case 0x5a6c:  // 41: the handler's voices keyed off
        key_off(h + 0x18, 0);
        w32(h + 0x1c, 0);
        w32(h + 0x18, 0);
        return 0;
    case 0x5adc:  // 43: the release point: the sequence ends here
        w16(h + 0x4a, std::uint32_t(s8(sound + 4) - 1));
        return 0;
    default: {
        char a[16];
        std::snprintf(a, sizeof a, "%08x", fn);
        unhosted(a, "grain type " + std::to_string(type));
    }
    }
}

// 6214: run the current grain, advance; the next grain's delay into +48.
std::int32_t BankEngine::step(std::uint32_t h) {
    const auto sound = r32(h + 4);
    const auto g = r32(sound + 8) + std::uint32_t(s16(h + 0x4a)) * 8;
    const auto r = grain(h, sound, g);
    if (r < 0) return 1;
    if (s8(h + 0x50)) {
        w16(h + 0x52, r16(h + 0x52) - 1);
        if (r16(h + 0x52) == 0) {
            w8(h + 0x50, 0);
            w16(h + 0x4a, r16(h + 0x4a) + r16(h + 0x54));
        }
    }
    w16(h + 0x4a, r16(h + 0x4a) + 1);
    const std::int32_t idx = s16(h + 0x4a);
    if (s8(sound + 4) - 1 < idx) {
        w16(h + 0x4a, 0xffff);
        return (r32(h + 0x18) || r32(h + 0x1c) || r32(h + 0x2c)) ? 0 : 1;
    }
    w16(h + 0x48, r16(r32(sound + 8) + std::uint32_t(idx) * 8 + 4) + std::uint32_t(r));
    return 0;
}

// 4774: one tick of the handler's grain sequence; nonzero: the handler is done.
std::int32_t BankEngine::sound_tick(std::uint32_t h) {
    if (s16(h + 0x4a) == -1) {
        if (r32(h + 0x2c) || r32(h + 0x18)) return 0;
        return r32(h + 0x1c) == 0 ? 1 : 0;
    }
    w16(h + 0x48, r16(h + 0x48) - 1);
    if (s16(h + 0x48) > 0) return 0;
    for (;;) {
        const auto r = step(h);
        if (r) return r;
        if (s16(h + 0x4a) == -1) return 0;
        if (s16(h + 0x48) > 0) return 0;
    }
}

// The volume / pan code of a tone or child grain: >= 0 itself, -1..-4 the
// handler's registers +4c.., -5 random, <= -6 the global registers.
namespace {
enum class Code { value, handler, random, global };
Code code_kind(std::int32_t c) {
    if (c >= 0) return Code::value;
    if (c >= -4) return Code::handler;
    if (c == -5) return Code::random;
    return Code::global;
}
}  // namespace

// 4a08: a tone of the bank on a new voice (volume, pan, priority from the handler).
std::int32_t BankEngine::tone_grain(std::uint32_t h, std::uint32_t sound, std::uint32_t g) {
    if (r8(h + 0x16) & 0x20) return 0;
    const auto tone = r32(r32(h + 0x58) + 0x34) + (r32(g) & 0xffffff);
    w16(h + 0x10, r16(h + 0x38) + std::uint32_t((s16(h + 0x3a) * s16(h + 0xc)) >> 10));
    if (s16(h + 0x10) >= 0x80) w16(h + 0x10, 0x7f);
    if (s16(h + 0x10) < 0) w16(h + 0x10, 0);
    w16(h + 0x12, r16(h + 0x3e) + r16(h + 0x3c));
    while (s16(h + 0x12) >= 0x168) w16(h + 0x12, r16(h + 0x12) - 0x168);
    while (s16(h + 0x12) < 0) w16(h + 0x12, r16(h + 0x12) + 0x168);
    const auto prio = priority(s16(h + 0x3a), tone);
    voice_lock(1, 0x1a);
    const auto tflags = r16(tone + 0xe);
    if (tflags & 8) unhosted("0001886c", "a noise tone (reserved voice)");
    const auto v = alloc_voice(s8(sound + 1), prio, tflags & 0x20);
    if (v == -1) {
        voice_unlock();
        return 0;
    }
    const auto m = h + 0x18 + std::uint32_t(v / 24) * 4;
    w32(m, r32(m) | (1u << (v % 24)));
    const auto V = voice_addr(v);
    w32(V + 0xc, h);
    w32(V + 0x10, std::uint32_t(voice_callback_6ad4));
    w32(V + 0x14, tone);
    const std::int32_t vc = s8(tone + 1);
    switch (code_kind(vc)) {
    case Code::value: w8(V + 0x2c, r8(tone + 1)); break;
    case Code::handler: w8(V + 0x2c, r8(h + 0x4c + std::uint32_t(~vc))); break;
    case Code::random: w8(V + 0x2c, random() & 0x7f); break;
    case Code::global: w8(V + 0x2c, r8(registers + std::uint32_t(-vc - 6))); break;
    }
    if (s8(V + 0x2c) < 0) w8(V + 0x2c, 0);
    const std::int32_t pc = s16(tone + 4);
    switch (code_kind(pc)) {
    case Code::value: w16(V + 0x2e, r16(tone + 4)); break;
    case Code::handler: w16(V + 0x2e, std::uint32_t((s8(h + 0x4c + std::uint32_t(~pc)) * 360) / 127)); break;
    case Code::random: w16(V + 0x2e, random() % 360); break;
    case Code::global: w16(V + 0x2e, std::uint32_t((s8(registers + std::uint32_t(-pc - 6)) * 360) / 127)); break;
    }
    while (s16(V + 0x2e) < 0) w16(V + 0x2e, r16(V + 0x2e) + 0x168);
    while (s16(V + 0x2e) >= 0x168) w16(V + 0x2e, r16(V + 0x2e) - 0x168);
    w8(V + 0x18, r8(h + 0x34));
    w8(V + 0x19, r8(h + 0x35));
    w16(V + 0x24, r16(h + 0x36));
    w8(V + 0x1a, std::uint32_t(prio));
    w16(V + 0x26, r16(h + 0x14));
    w8(V + 0x1b, r8(sound + 1));
    const auto pan3 = (r16(r32(h + 4) + 6) & 4) ? distance_pan(s16(V + 0x2e), s16(h + 0x3a)) : s16(V + 0x2e);
    w16(V + 0x22, 0);
    w16(V + 0x20, 0);
    std::array<std::int16_t, 2> out{0, 0};
    make_volume(volume_, 0x7f, 0, s16(h + 0x10), s16(h + 0x12), s8(V + 0x2c), pan3, out);
    w16(V + 0x20, std::uint16_t(out[0]));
    w16(V + 0x22, std::uint16_t(out[1]));
    key_on(v, (tflags >> 3) & 1);
    voice_unlock();
    return 0;
}

// 5db0: a child sound (by index in this bank, or by name) played with the handler's volume, pan, bend and registers.
std::int32_t BankEngine::child_grain(std::uint32_t h, std::uint32_t g) {
    if (r8(h + 0x16) & 0x20) return 0;
    const auto rec = r32(r32(h + 0x58) + 0x34) + (r32(g) & 0xffffff);
    if (s16(h + 0xc) == 0) return 0;
    const auto clamp = [](std::int32_t a) {
        if (a < 0) a = -a;
        return a < 0x80 ? a : 0x7f;
    };
    std::int32_t vol = 0;
    const std::int32_t vc = s32(rec);
    switch (code_kind(vc)) {
    case Code::value: vol = vc; break;
    case Code::handler: vol = s8(h + 0x4c + std::uint32_t(~vc)); break;
    case Code::random: vol = std::int32_t(random() & 0x7f); break;
    case Code::global: vol = s8(registers + std::uint32_t(-vc - 6)); break;
    }
    vol = clamp(vol);
    std::int32_t pan = 0;
    const std::int32_t pc = s32(rec + 4);
    switch (code_kind(pc)) {
    case Code::value: pan = pc; break;
    case Code::random: pan = std::int32_t((random() % 360) & 0xffff); break;
    case Code::handler: pan = (clamp(s8(h + 0x4c + std::uint32_t(~pc))) * 360) / 127; break;
    case Code::global: pan = (clamp(s8(registers + std::uint32_t(-pc - 6))) * 360) / 127; break;
    }
    if (cmd_depth_ >= cmd_stack_levels) unhosted("00005db0", "child sounds nested deeper than the host's command stack");
    const auto slot = cmd_stack + std::uint32_t(cmd_depth_) * 0x40;
    std::int32_t index = s32(rec + 0xc);
    std::uint32_t bank = 0;
    if (index >= 0) {
        bank = r32(h + 0x58);
    } else {
        w32(slot, 0);
        index = lookup_sound(0, rec + 0x10, slot);
        bank = r32(slot);
        if (index < 0 || bank == 0) {
            index = lookup_sound(r32(h + 0x58), rec + 0x10, slot);
            if (index < 0) return 0;
            bank = r32(slot);
        }
    }
    const auto cmd = slot + 4;
    w32(cmd + 0x28, 0x8000007c);
    w32(cmd + 8, std::uint32_t((s16(h + 0x3a) * s16(h + 0xc)) / 127));
    w32(cmd + 0xc, std::uint32_t(s16(h + 0x3e)));
    w16(cmd + 0x20, r16(h + 0x46));
    w16(cmd + 0x22, r16(h + 0x42));
    for (std::uint32_t i = 0; i < 4; ++i) w8(cmd + 0x24 + i, r8(h + 0x4c + i));
    ++cmd_depth_;
    const auto c = play_sound(bank, index, sx16(std::uint32_t(vol)), sx16(std::uint32_t(pan)), cmd);
    --cmd_depth_;
    if (c) add_child(h, c);
    return 0;
}

// 3fa8: a sound of a bank started: its handler set up and its grains run up to the first delay.
std::uint32_t BankEngine::play_sound(std::uint32_t bank, std::int32_t index, std::int32_t vol, std::int32_t pan,
                                     std::uint32_t cmd) {
    if (index < 0 || index >= s16(bank + 0x16)) return 0;
    const auto sound = r32(bank + 0x1c) + std::uint32_t(index) * 12;
    w16(sound + 6, r16(sound + 6) | 0x4000);
    if (s8(sound + 4) == 0) return 0;
    if (vol == -1) vol = s8(sound);
    if (pan == -1) pan = s16(sound + 2);
    const auto f = r32(cmd + 0x28);
    std::int32_t s6 = (f & 4) ? s32(cmd + 8) : 0x400;
    std::int32_t s7 = (f & 8) ? s32(cmd + 0xc) : -1;
    const std::int32_t bend2 = (f & 0x10) ? s16(cmd + 0x20) : 0;
    const std::int32_t bend = (f & 0x20) ? s16(cmd + 0x22) : 0;
    if (s6 == 0x7fffffff) s6 = 0x400;
    std::int32_t s2 = (s6 * vol) >> 10;
    if (s2 >= 0x80) s2 = 0x7f;
    if (std::uint32_t(s7 + 2) < 2) s7 = pan;
    if (r16(sound + 6) & 2) stop_sound(sound, 0, 1);
    const auto h = alloc_handler(sound, s6, std::int32_t((f >> 31) ^ 1));
    if (!h) return 0;
    w32(h + 4, sound);
    w32(h + 0x120, sound);
    w32(h + 0x11c, r32(timestamp));
    unlink_lfos(h);
    w32(h + 8, 0);
    w16(h + 0xc, std::uint32_t(vol));
    w16(h + 0xe, std::uint32_t(pan));
    w16(h + 0x10, std::uint32_t(s2));
    w16(h + 0x12, std::uint32_t(s7));
    w8(h + 0x16, 0);
    w16(h + 0x14, std::uint32_t(bend2));
    w32(h + 0x18, 0);
    w32(h + 0x1c, 0);
    w8(h + 0x34, 0x3c);
    w8(h + 0x35, 0);
    w16(h + 0x36, std::uint32_t(bend));
    w16(h + 0x38, 0);
    w16(h + 0x3a, std::uint32_t(s6));
    w16(h + 0x3c, 0);
    w16(h + 0x3e, std::uint32_t(s7));
    w16(h + 0x40, 0);
    w16(h + 0x42, std::uint32_t(bend));
    w16(h + 0x44, 0);
    w16(h + 0x46, std::uint32_t(bend2));
    w8(h + 0x17, r8(sound + 1));
    w16(h + 0x4a, 0);
    w16(h + 0x48, r16(r32(sound + 8) + 4));
    for (std::uint32_t i = 0; i < 4; ++i) w8(h + 0x4c + i, (f & 0x40) ? r8(cmd + 0x24 + i) : 0);
    w8(h + 0x50, 0);
    w16(h + 0x52, 0);
    w16(h + 0x54, 0);
    w32(h + 0x58, bank);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto l = h + i * 0x30;
        w16(l + 0x5c, 0);
        w32(l + 0x68, 0);
        w8(l + 0x5e, 0);
        w8(l + 0x60, 0);
        w8(l + 0x61, 0);
        w32(l + 0x84, h);
    }
    std::int32_t s1 = 0;
    while (s1 == 0 && s16(h + 0x4a) != -1 && r16(h + 0x48) == 0) s1 = step(h);
    activate(h);
    if (s1 != 0 || (r16(h + 0x48) == 0 && r32(h + 0x18) == 0 && r32(h + 0x1c) == 0 && r32(h + 0x2c) == 0)) {
        stop_handler(h, 1, 0, 0);
        return 0;
    }
    return r32(h);
}

// ca98: a loaded bank by its 8-byte name (names block +0).
std::uint32_t BankEngine::lookup_bank_by_name(std::uint32_t name8) {
    for (auto b = r32(bank_list); b; b = r32(b + 0x30)) {
        if (!(r32(b + 8) & 0x100)) continue;
        const auto n = r32(b + 0x38);
        if (r32(n) == r32(name8) && r32(n + 4) == r32(name8 + 4)) return b;
    }
    return 0;
}

// cb0c: a sound index by its 16-byte name (hash cca4 into the names block's buckets); bank 0: every bank.
std::int32_t BankEngine::lookup_sound(std::uint32_t bank, std::uint32_t name16, std::uint32_t out) {
    if (bank == 0) {
        for (auto b = r32(bank_list); b; b = r32(b + 0x30)) {
            const auto v = lookup_sound(b, name16, out);
            if (v >= 0) {
                w32(out, b);
                return v;
            }
        }
        return -1;
    }
    if (r32(bank) != sblk || !(r32(bank + 8) & 0x100)) return -1;
    const auto nb = r32(bank + 0x38);
    std::int32_t k = s8(name16) + s8(name16 + 4) + s8(name16 + 8) + s8(name16 + 12);  // cca4
    if (k < 0) k = -k;
    k %= 32;
    for (auto e = r32(nb + 8) + std::uint32_t(s16(nb + 0x18 + std::uint32_t(k) * 2)) * 20; r32(e); e += 20) {
        if (r32(e) != r32(name16) || r32(e + 4) != r32(name16 + 4) || r32(e + 8) != r32(name16 + 8) ||
            r32(e + 12) != r32(name16 + 12))
            continue;
        if (out) w32(out, bank);
        return s16(e + 0x10);
    }
    return -1;
}

// be54 (cmd 0x5c): flags +28: 1 the bank handle at +0 (else its name), 2 the index at +10 (else the name).
std::uint32_t BankEngine::play(std::uint32_t cmd) {
    const auto f = r32(cmd + 0x28);
    std::uint32_t bank = 0;
    if (f & 1) {
        bank = r32(cmd);
    } else if (s8(cmd) != 0) {
        bank = lookup_bank_by_name(cmd);
        if (!bank) return 0;
    }
    std::int32_t index = 0;
    if (f & 2) {
        index = s16(cmd + 0x10);
    } else {
        const auto out = scratch + 0x40;
        w32(out, bank);
        index = lookup_sound(bank, cmd + 0x10, out);
        if (index < 0) return 0;
        bank = r32(out);
    }
    if (bank == 0) unhosted("0000bf64", "a play command without a bank");
    if (r32(bank) != sblk) unhosted("0000c004", "a MIDI bank play");
    return play_sound(bank, index, -1, -1, cmd);
}

// bb84 (cmd 0x56): the sound's user data (bank +3c, 16 bytes a sound).
std::int32_t BankEngine::user_data(std::uint32_t bank, std::uint32_t name8, std::int32_t index, std::uint32_t name16,
                                   std::uint32_t out) {
    if (bank == 0 && name8 != 0) {
        bank = lookup_bank_by_name(name8);
        if (!bank) return 0;
    }
    if (index >= 0 && bank == 0) return 0;
    if (index == -1) {
        const auto o = scratch + 0x40;
        w32(o, bank);
        index = lookup_sound(bank, name16, o);
        if (index < 0) return 0;
        bank = r32(o);
    }
    if (bank == 0) unhosted("0000bc9c", "user data of a negative index without a bank");
    if (r32(bank) != sblk) return 0;
    const auto src = r32(bank + 0x3c) + std::uint32_t(index) * 16;
    for (std::uint32_t i = 0; i < 16; i += 4) w32(out + i, r32(src + i));
    return 1;
}

// 98c4 (97e0's SBlk path): the head's tables made absolute, the tone SPU addresses offset, the bank listed.
void BankEngine::relocate_bank(std::uint32_t head) {
    if (r32(head) == 0x32764253) unhosted("000097e0", "an SBv2 bank head");
    w32(head + 0x34, r32(head + 0x34) + head);
    for (std::int32_t i = 0; i < s16(head + 0x18); ++i) {
        const auto g = r32(head + 0x20) + std::uint32_t(i) * 8;
        if (r8(g + 3) != 1) continue;
        const auto tone = r32(head + 0x34) + (r32(g) & 0xffffff);
        w32(tone + 0x10, r32(tone + 0x10) + r32(head + 0x24));
    }
    for (std::int32_t i = 0; i < s16(head + 0x16); ++i) {
        const auto s = r32(head + 0x1c) + std::uint32_t(i) * 12;
        w32(s + 8, r32(s + 8) + r32(head + 0x20));
        w16(s + 6, r16(s + 6) & 0xbfff);
    }
    if (r32(head + 8) & 0x100) {
        const auto n = r32(head + 0x38) + head;
        w32(head + 0x38, n);
        for (const std::uint32_t o : {8u, 0xcu, 0x10u, 0x14u}) w32(n + o, r32(n + o) + n);
    }
    if (r32(head + 8) & 0x200) w32(head + 0x3c, r32(head + 0x3c) + head);
    auto p = r32(bank_list);
    if (!p) {
        w32(bank_list, head);
        return;
    }
    while (r32(p + 0x30)) p = r32(p + 0x30);
    w32(p + 0x30, head);
}

// The file header [3, 2, 0x18, head size, body offset, body size] then the SBlk head; 93ac's pre-steps, 97e0.
std::uint32_t BankEngine::load_bank(std::span<const std::uint8_t> file, std::uint32_t addr, std::uint32_t spu_addr) {
    const auto word = [&](std::size_t o) {
        if (o + 4 > file.size()) throw std::runtime_error("989snd: bank file truncated");
        return std::uint32_t(file[o]) | std::uint32_t(file[o + 1]) << 8 | std::uint32_t(file[o + 2]) << 16 |
               std::uint32_t(file[o + 3]) << 24;
    };
    const auto at = word(8), size = word(12);
    if (word(0) != 3 || word(4) != 2 || at != 0x18) throw std::runtime_error("989snd: not a bank file");
    if (addr + size > memory_size || addr < heads) throw std::runtime_error("989snd: bank head outside the host area");
    std::copy(file.begin() + at, file.begin() + at + size, mem_.begin() + addr);
    if (r32(addr) != sblk) unhosted("000093ac", "a bank head other than SBlk");
    w32(addr + 0x24, spu_addr);
    w32(addr + 8, r32(addr + 8) & ~4u);
    if (!(r32(addr + 8) & 1)) {
        w32(addr + 8, r32(addr + 8) | 1);
        w32(addr + 0x1c, r32(addr + 0x1c) + addr);
        w32(addr + 0x20, r32(addr + 0x20) + addr);
    }
    relocate_bank(addr);
    return addr;
}

// 93ac with no SPU address supplied (the game's 3a8260 / 8c8c / 84f0 load): the
// head copied as above, then for an SBlk head with +8 bit 4 (the TORONTO1
// banks: 0x304) ed70(+2c) -> +24 (0: error 0x1a, 93ac returns -0xb and the
// load fails), else f0d8(+24, +2c) (0: error 0x1b); +8 keeps bit 4. Then the
// one-time offsets (+1c / +20) and 97e0. Returns the head, or 0 on failure.
std::uint32_t BankEngine::load_bank(std::span<const std::uint8_t> file, std::uint32_t addr) {
    const auto word = [&](std::size_t o) {
        if (o + 4 > file.size()) throw std::runtime_error("989snd: bank file truncated");
        return std::uint32_t(file[o]) | std::uint32_t(file[o + 1]) << 8 | std::uint32_t(file[o + 2]) << 16 |
               std::uint32_t(file[o + 3]) << 24;
    };
    const auto at = word(8), size = word(12);
    if (word(0) != 3 || word(4) != 2 || at != 0x18) throw std::runtime_error("989snd: not a bank file");
    if (addr + size > memory_size || addr < heads) throw std::runtime_error("989snd: bank head outside the host area");
    std::copy(file.begin() + at, file.begin() + at + size, mem_.begin() + addr);
    if (r32(addr) != sblk) unhosted("000093ac", "a bank head other than SBlk");
    const auto body = r32(addr + 0x2c);
    if (r32(addr + 8) & 4) {
        const auto spu = sram_alloc(body);
        if (!spu) {
            error(0x1a);
            return 0;
        }
        w32(addr + 0x24, spu);
    } else if (!sram_reserve(r32(addr + 0x24), body)) {
        error(0x1b);
        return 0;
    }
    if (!(r32(addr + 8) & 1)) {
        w32(addr + 8, r32(addr + 8) | 1);
        w32(addr + 0x1c, r32(addr + 0x1c) + addr);
        w32(addr + 0x20, r32(addr + 0x20) + addr);
    }
    relocate_bank(addr);
    return addr;
}

// The sound-RAM allocator: free blocks in a tree ordered by size (left:
// smaller), 0x81 nodes at 0x2416c {+0 address, +4 size, +8 in use, +c parent,
// +10 left, +14 right}, the root at 0x1e210 and a node count at 0x1e214.
namespace {
constexpr std::uint32_t sram_root = 0x1e210;   // -0x1df0
constexpr std::uint32_t sram_count = 0x1e214;  // -0x1dec
constexpr std::uint32_t sram_nodes = 0x2416c;
constexpr std::int32_t sram_node_count = 0x81;
constexpr std::uint32_t sram_node(std::int32_t i) { return sram_nodes + std::uint32_t(i) * 0x18; }
constexpr std::uint32_t sram_round(std::uint32_t size) { return (size & 0x3f) ? size + 0x40 - (size & 0x3f) : size; }
}  // namespace

// ed20.
void BankEngine::sram_init() {
    w32(sram_root, 0);
    for (std::int32_t i = sram_node_count - 1; i >= 0; --i) w32(sram_node(i) + 8, 0);
    sram_free(0x5040, 0x1fafbf);
}

// f414: from `node` down the right links, the first block of at least `size`.
std::uint32_t BankEngine::sram_find(std::uint32_t node, std::uint32_t size) const {
    while (node && r32(node + 4) < size) node = r32(node + 0x14);
    return node;
}

// f380: a node into the tree (its children kept).
void BankEngine::sram_insert(std::uint32_t n) {
    if (!n) return;
    w32(n + 8, 1);
    w32(sram_count, r32(sram_count) + 1);
    auto cur = r32(sram_root);
    if (!cur) {
        w32(sram_root, n);
        w32(n + 0xc, 0);
        return;
    }
    const auto size = r32(n + 4);
    for (;;) {
        const auto link = size < r32(cur + 4) ? 0x10u : 0x14u;
        if (const auto next = r32(cur + link)) {
            cur = next;
            continue;
        }
        w32(cur + link, n);
        w32(n + 0xc, cur);
        return;
    }
}

// f2c8: a node out of the tree (its right subtree takes its place, its left subtree is reinserted).
void BankEngine::sram_unlink(std::uint32_t n) {
    if (!n) return;
    if (n == r32(sram_root)) {
        w32(sram_root, r32(n + 0x14));
    } else {
        const auto parent = r32(n + 0xc);
        w32(parent + (r32(parent + 0x10) == n ? 0x10 : 0x14), r32(n + 0x14));
    }
    if (const auto right = r32(n + 0x14)) w32(right + 0xc, r32(n + 0xc));
    sram_insert(r32(n + 0x10));
    w32(n + 8, 0);
    w32(n + 0x10, 0);
    w32(n + 0x14, 0);
    w32(sram_count, r32(sram_count) - 1);
}

// ed70: the smallest-fitting block's low end (error 0x20 when none fits).
std::uint32_t BankEngine::sram_alloc(std::uint32_t size) {
    const auto root = r32(sram_root);
    if (!size) return 0;
    size = sram_round(size);
    auto n = sram_find(root, size);
    if (!n) {
        error(0x20);
        return 0;
    }
    while (const auto smaller = sram_find(r32(n + 0x10), size)) n = smaller;
    const auto addr = r32(n);
    sram_unlink(n);
    const auto left = r32(n + 4) - size;
    w32(n + 4, left);
    if (left) {
        w32(n, r32(n) + size);
        sram_insert(n);
    }
    return addr;
}

// ee48: `size` ending at a 128 KB boundary inside the smallest block that has
// room below one (f0d8 reserves it). None: error 0x58 unless the free total is short.
std::uint32_t BankEngine::sram_alloc_aligned(std::uint32_t size) {
    size = sram_round(size);
    std::uint32_t best = 0;
    for (std::int32_t i = 0; i < sram_node_count; ++i) {
        const auto n = sram_node(i);
        if (!r32(n + 8)) continue;
        const auto block = r32(n + 4);
        if (block < size) continue;
        const auto start = r32(n), end = start + block, floor = end - (end & 0x1ffff);
        if (!(start < floor) || floor - start < size) continue;
        if (best && !(block < r32(best + 4))) continue;
        best = n;
    }
    if (!best) {
        if (sram_free_total() < size) return 0;
        error(0x58);
        return 0;
    }
    return sram_reserve(((r32(best) + r32(best + 4)) & 0xfffe0000u) - size, size);
}

// ef68: a block back, merged with the free blocks it touches (error 0x5d for
// size 0, 0x21 with no free node).
void BankEngine::sram_free(std::uint32_t addr, std::uint32_t size) {
    if (!size) {
        error(0x5d);
        return;
    }
    size = sram_round(size);
    std::int32_t slot = sram_node_count;
    for (std::int32_t i = 0; i < sram_node_count; ++i) {
        const auto n = sram_node(i);
        if (!r32(n + 8)) {
            if (i < slot) slot = i;
            continue;
        }
        const auto start = r32(n);
        if (start + r32(n + 4) == addr) {
            sram_unlink(n);
            addr = r32(n);
            size += r32(n + 4);
            if (i < slot) slot = i;
        } else if (start == addr + size) {
            sram_unlink(n);
            size += r32(n + 4);
            if (i < slot) slot = i;
        }
    }
    if (slot >= sram_node_count) {
        error(0x21);
        return;
    }
    const auto n = sram_node(slot);
    w32(n + 4, size);
    w32(n, addr);
    w32(n + 0xc, 0);
    w32(n + 0x10, 0);
    w32(n + 0x14, 0);
    sram_insert(n);
}

// f0d8: [addr, addr + size) out of the free blocks holding it. Returns addr,
// or 0 (error 0x23) when nothing was free there / no node is left; a range
// past its block's end reports 0x22 and still returns addr.
std::uint32_t BankEngine::sram_reserve(std::uint32_t addr, std::uint32_t size) {
    size = sram_round(size);
    std::int32_t rest = 0;
    std::uint32_t rest_addr = 0;
    std::int32_t slot = sram_node_count;
    for (std::int32_t i = 0; i < sram_node_count; ++i) {
        const auto n = sram_node(i);
        if (!r32(n + 8)) {
            if (i < slot) slot = i;
            continue;
        }
        const auto start = r32(n);
        if (addr < start || !(addr < start + r32(n + 4))) continue;
        sram_unlink(n);
        rest_addr = r32(n);
        const auto end = addr + size;
        if (addr == rest_addr) {
            const auto left = r32(n + 4) - size;
            w32(n, addr + size);
            w32(n + 4, left);
            if (left) sram_insert(n);
            return addr;
        }
        const auto block = r32(n + 4);
        rest = std::int32_t(block);
        if (end == rest_addr + block) {
            const auto left = block - size;
            w32(n + 4, left);
            if (left) sram_insert(n);
            return addr;
        }
        w32(n + 4, addr - rest_addr);
        sram_insert(n);
        rest_addr = end;
        rest = rest - std::int32_t(r32(n + 4)) - std::int32_t(size);
    }
    if (rest == 0 || slot >= sram_node_count) {
        error(0x23);
        return 0;
    }
    if (rest < 0) {
        error(0x22);
        return addr;
    }
    const auto n = sram_node(slot);
    w32(n + 4, std::uint32_t(rest));
    w32(n, rest_addr);
    w32(n + 0xc, 0);
    w32(n + 0x10, 0);
    w32(n + 0x14, 0);
    sram_insert(n);
    return addr;
}

// f44c / f48c: the free total / the largest free block.
std::uint32_t BankEngine::sram_free_total() const {
    std::uint32_t total = 0;
    for (std::int32_t i = sram_node_count - 1; i >= 0; --i)
        if (r32(sram_node(i) + 8)) total += r32(sram_node(i) + 4);
    return total;
}
std::uint32_t BankEngine::sram_free_largest() const {
    std::uint32_t largest = 0;
    for (std::int32_t i = sram_node_count - 1; i >= 0; --i)
        if (r32(sram_node(i) + 8) && largest < r32(sram_node(i) + 4)) largest = r32(sram_node(i) + 4);
    return largest;
}

// d0e4(core, type, depth, delay)'s sound-RAM part on its first-time path
// (0x1e194[core] (the type) and 0x1e1a4[core] both <= 0): ee48(0x1e1c8[type])
// -> 0x1e19c[core] (0: error 0x56, 0x1e194[core] = -1, -0xb), then
// 0x1e194[core] = type. The rest of the path (3948, the ESA end address
// sceSdSetAddr(core | 0x1d00), sceSdSetEffectAttr 1ba48, the work-area clear
// a734 / 1ba50 and the countdown 0x1e1b4 / 0x1e1bc that d598 restores) sets the
// libsd reverb, which the voice model does not have.
std::int32_t BankEngine::reverb_area(std::int32_t core, std::int32_t type) {
    if (core < 0 || core > 1) unhosted("0000d0e4", "a reverb core other than 0 / 1");
    const auto cur = 0x1e194u + std::uint32_t(core) * 4, prev = 0x1e1a4u + std::uint32_t(core) * 4;
    // d140: the type already set (a second init 001c9bf0, the front end's resume 002c9510) only sets the effect
    // attributes (1ba48, libsd's sceSdSetEffectAttr), which the host's SPU2 model does not keep, as after an allocation.
    if (s32(cur) == type) return 0;
    if (s32(prev) > 0) unhosted("0000d18c", "a reverb with a pending previous type");
    if (s32(cur) > 0) unhosted("0000d1f8", "a reverb type change (its area freed by ef68)");
    if (type <= 0 || type > 9) unhosted("0000d2d8", "reverb type " + std::to_string(type));
    const auto area = sram_alloc_aligned(r32(0x1e1c8 + std::uint32_t(type) * 4));
    w32(0x1e19c + std::uint32_t(core) * 4, area);
    if (!area) {
        w32(cur, 0xffffffffu);
        error(0x56);
        return -0xb;
    }
    w32(cur, std::uint32_t(type));
    return 0;
}

// de2c: a handle's handler (type 5: the pool slot, if the handle word still matches).
std::uint32_t BankEngine::lookup(std::uint32_t handle) {
    if (handle == 0) return 0;
    if (handle == 0xffffffffu) {
        error(0x64);
        return 0;
    }
    const auto idx = (handle >> 16) & 0xff;
    const auto type = (handle >> 24) & 0x1f;
    if (type == 5) {
        if (idx >= 0x40) {
            error(0x65);
            return 0;
        }
        const auto p = handlers + idx * handler_size;
        return r32(p) == handle ? p : 0;
    }
    if (type == 4) unhosted("0000dee4", "a stream handle in the bank engine");
    return 0;
}

// c060 (cmd 0x15).
void BankEngine::stop(std::uint32_t handle) {
    if (const auto c = lookup(handle)) stop_handler(c, 1, 0, 0);
}

// c268 (cmd 0x21).
std::uint32_t BankEngine::restart(std::uint32_t handle, std::uint32_t flags, std::int32_t vol, std::int32_t pan,
                                  std::int32_t a4, std::int32_t a5) {
    if (!lookup(handle)) return 0;
    if ((flags & 6) == 6) flags &= ~2u;
    if (!(flags & 1)) vol = 0x7fffffff;
    const std::int32_t pan_arg = (flags & 2) ? pan : -2;
    if (flags & 3) set_vol_pan(handle, vol, pan_arg);
    if (flags & 4) glide(handle, pan, 0, 0x1e, 4);
    if (flags & 8) unhosted("0000c5e4", "a restart with flag 8 (" + std::to_string(sx16(std::uint32_t(a4))) + ")");
    if (flags & 0x10) unhosted("0000c580", "a restart with flag 0x10 (" + std::to_string(sx16(std::uint32_t(a5))) + ")");
    return handle;
}

// c1d4: type 5 -> 6358; streams (179e4) are the host's.
void BankEngine::set_vol_pan(std::uint32_t handle, std::int32_t vol, std::int32_t pan) {
    const auto type = (handle >> 24) & 0x1f;
    if (type == 4) unhosted("000179e4", "a stream volume in the bank engine");
    if (type == 5) handler_vol_pan(handle, vol, pan, 0);
}

// 6358: a handler's volume (+3a) and pan (+3e) into its children and voices.
void BankEngine::handler_vol_pan(std::uint32_t handle, std::int32_t vol, std::int32_t pan, std::int32_t a3) {
    const auto c = lookup(handle);
    if (!c) return;
    const auto sound = r32(c + 4);
    if (vol < 0) {
        w16(c + 0x3a, std::uint32_t(-vol));
    } else if (vol != 0x7fffffff) {
        effect_remove(c, 2);
        w16(c + 0x3a, std::uint32_t(vol));
    }
    if (pan == -1) w16(c + 0x3e, r16(sound + 2));
    else if (pan != -2) w16(c + 0x3e, std::uint32_t(pan));
    std::int32_t s5 = ((s16(c + 0x3a) * s16(c + 0xc)) >> 10) + s16(c + 0x38);
    s5 = std::clamp(s5, 0, 0x7f);
    std::int32_t s0 = s16(c + 0x3e) + s16(c + 0x3c);
    while (s0 >= 360) s0 -= 360;
    while (s0 < 0) s0 += 360;
    if (s5 == s16(c + 0x10) && s0 == s16(c + 0x12)) return;
    if (a3 == 0)
        for (auto k = r32(c + 0x2c); k; k = r32(k + 0x30))
            handler_vol_pan(r32(k), (s16(c + 0x3a) * s16(c + 0xc)) / 127, pan, 0);
    w16(c + 0x10, std::uint32_t(s5));
    w16(c + 0x12, std::uint32_t(s0));
    const auto locked = voice_lock(1, 0x1b);
    for (auto v = next_voice(c, 0); v != -1; v = next_voice(c, v + 1)) {
        const auto V = voice_addr(v);
        const auto pan3 = (r16(sound + 6) & 4) ? distance_pan(s16(V + 0x2e), s16(c + 0x3a)) : s16(V + 0x2e);
        std::array<std::int16_t, 2> out{s16(V + 0x20), s16(V + 0x22)};
        make_volume(volume_, 0x7f, 0, s16(c + 0x10), s16(c + 0x12), s8(V + 0x2c), pan3, out);
        w16(V + 0x20, std::uint16_t(out[0]));
        w16(V + 0x22, std::uint16_t(out[1]));
        if (!(r8(c + 0x16) & 2)) {
            const auto entry = static_cast<std::uint16_t>(std::uint32_t(v / 24) | std::uint32_t(v % 24) << 1);
            spu_.set_param(entry, static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x20), s8(c + 0x17))) >> 1));
            spu_.set_param(static_cast<std::uint16_t>(entry | 0x100),
                           static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x22), s8(c + 0x17))) >> 1));
        }
        w8(V + 0x1a, std::uint32_t(priority(s16(c + 0x3a), r32(V + 0x14))));
    }
    if (locked) voice_unlock();
}

// 2bbc: a pan glide to `target` (direction `dir`, `time` ticks; mode 4: time per 180 degrees, 3: rescaled).
void BankEngine::glide(std::uint32_t handle, std::int32_t target, std::int32_t dir, std::int32_t time, std::int32_t mode) {
    const auto c = lookup(handle);
    if (!c) return;
    std::int32_t s0 = target - s16(c + 0x12);
    bool shortest = false;
    if (dir == 0) {
        shortest = true;
        dir = 1;
    }
    if (dir > 0 && s0 < 0) s0 += 360;
    if (dir < 0 && s0 > 0) s0 -= 360;
    if (shortest && std::abs(s0) >= 0xb5) {
        dir = -dir;
        s0 -= 360;
    }
    if (mode == 4) time = (time * std::abs(s0)) / 180;
    if (time == 0) {
        effect_remove(c, 4);
        set_vol_pan(handle, 0x7fffffff, target);
        return;
    }
    if (s0 == 0) {
        effect_remove(c, 4);
        return;
    }
    bool fresh = false;
    auto e = effect_find(c, 4);
    if (e) {
        w32(e, r32(e) | 0x80000000u);
        if (mode == 3) {
            std::int32_t a = target - s32(e + 0x20);
            if (dir > 0 && a < 0) a += 360;
            if (dir < 0 && a > 0) a -= 360;
            if (a == 0) {
                effect_remove(c, 4);
                set_vol_pan(handle, 0x7fffffff, target);
                return;
            }
            time = std::abs((s0 * time) / a);
            if (time <= 0) time = 1;
        }
    } else {
        e = effect_alloc();
        if (!e) {
            set_vol_pan(handle, 0x7fffffff, target);
            return;
        }
        w32(e + 0x10, 0x2ef0);
        w32(e, r32(e) | 4);
        fresh = true;
    }
    if (std::abs(s0) < time) {
        const auto q = std::abs(time / s0);
        w32(e + 0x1c, std::uint32_t(q));
        w32(e + 4, std::uint32_t(q));
        w32(e + 8, 1);
        w32(e + 0x18, s0 < 0 ? 0xffffffffu : 1u);
    } else {
        w32(e + 0x18, std::uint32_t((s0 << 5) / time));
        w32(e + 8, 0);
        w32(e + 4, 0);
    }
    w32(e + 0x20, std::uint32_t(target));
    if (fresh) effect_append(c, e);
    else w32(e, r32(e) & 0x7fffffffu);
}

// 2ef0: one step of a pan glide.
std::int32_t BankEngine::glide_tick(std::uint32_t e, std::uint32_t h) {
    const std::int32_t step = s32(e + 0x18);
    const std::int32_t old = s16(h + 0x12);
    std::int32_t s0 = (std::abs(step) >= 2) ? sx16(std::uint32_t(((old << 5) + step) << 11) >> 16)
                                            : sx16(r16(h + 0x12) + std::uint32_t(step));
    const std::int32_t target = s32(e + 0x20);
    std::int32_t a0 = sx16(std::uint32_t(target));
    if (s0 >= 360 && target < old) a0 = sx16(std::uint32_t(target + 360));
    else if (s0 < 0 && old < target) a0 = sx16(std::uint32_t(target - 360));
    if ((step > 0 && a0 < s0 && old < a0) || (step < 0 && s0 < a0 && a0 < old)) {
        s0 = sx16(std::uint32_t(target));
    } else if (s0 < 0) {
        s0 = sx16(std::uint32_t(s0 + 360));
    } else if (s0 >= 360) {
        s0 = sx16(std::uint32_t(s0 - 360));
    }
    set_vol_pan(r32(h), 0x7fffffff, s0);
    if (s0 == target) effect_unlink(h, e);
    else if (r32(e + 8) == 1) w32(e + 4, r32(e + 0x1c));
    return 0;
}

// e8b0: a handler's effect chain ticked (the rest of the chain first).
std::int32_t BankEngine::effect_tick(std::uint32_t e, std::uint32_t h) {
    if (r32(e + 0xc) && effect_tick(r32(e + 0xc), h) == 1) return 1;
    if (s32(e) < 0) return 0;
    if (r32(e + 8) == 1) w32(e + 4, r32(e + 4) - 1);
    if (r32(e + 4) != 0) return 0;
    if (r32(e + 0x10) != 0x2ef0) unhosted("0000e938", "an effect callback other than the pan glide 00002ef0");
    return glide_tick(e, h);
}

void BankEngine::effect_free_chain(std::uint32_t e) {  // e954
    if (r32(e + 0xc)) effect_free_chain(r32(e + 0xc));
    w32(e, 0);
}

void BankEngine::effect_remove(std::uint32_t h, std::uint32_t m) {  // e990
    if (const auto e = effect_find(h, m)) effect_unlink(h, e);
}

std::uint32_t BankEngine::effect_find(std::uint32_t h, std::uint32_t m) const {  // e9c4
    auto e = r32(h + 8);
    while (e && !(r32(e) & m)) e = r32(e + 0xc);
    return e;
}

std::uint32_t BankEngine::effect_alloc() {  // e9fc
    for (std::uint32_t i = 0; i < 0x40; ++i) {
        const auto e = effects + i * 0x24;
        if (r32(e) & 1) continue;
        w32(e, 1);
        w32(e + 0xc, 0);
        return e;
    }
    return 0;
}

void BankEngine::effect_append(std::uint32_t h, std::uint32_t e) {  // ea44
    auto p = r32(h + 8);
    if (!p) {
        w32(h + 8, e);
        return;
    }
    while (r32(p + 0xc)) p = r32(p + 0xc);
    w32(p + 0xc, e);
}

void BankEngine::effect_unlink(std::uint32_t h, std::uint32_t e) {  // ea8c
    if (!e) return;
    w32(e, r32(e) & ~1u);
    if (r32(h + 8) == e) {
        w32(h + 8, r32(e + 0xc));
        return;
    }
    for (auto p = r32(h + 8); p; p = r32(p + 0xc)) {
        if (r32(p + 0xc) == e) {
            w32(p + 0xc, r32(e + 0xc));
            return;
        }
    }
}

// e7d0: every active handler's grains (4774) and effects (e8b0); a finished one is stopped.
void BankEngine::handlers_tick() {
    for (auto h = r32(active_head); h;) {
        std::int32_t done = 0;
        const auto type = (r32(h) >> 24) & 0x1f;
        if (type == 4) unhosted("00011efc", "a stream handler in the bank engine");
        if (type == 5 && !(r8(h + 0x16) & 2)) done = sound_tick(h);
        if (!done && r32(h + 8) && !(r8(h + 0x16) & 2)) done = effect_tick(r32(h + 8), h);
        if (done) stop_handler(h, 1, 0, 0);
        h = r32(h + 0x24);
    }
}

// f884 with one pending tick.
bool BankEngine::tick() {
    if (r32(busy)) return false;
    w32(busy, 1);
    if (r32(lock_held)) unhosted("0000f8e8", "a sound tick while the voice lock is held");
    w32(pending_ticks, r32(pending_ticks) + 1);
    for (auto V = r32(playing_list); V;) {
        const auto v = s32(V + 4);
        const std::uint32_t core = std::uint32_t(v / 24), vi = std::uint32_t(v % 24), bit = 1u << vi;
        if (!(r32(pending_kon + core * 4) & bit)) {
            if (spu_.get_param(static_cast<std::uint16_t>(core | vi << 1 | 0x500)) != 0) {
                w8(counters + std::uint32_t(v), 4);
            } else if (r32(keyed_off + core * 4) & bit) {
                voice_ended(v);
            } else if (r32(keyed_on + core * 4) & bit) {
                w8(counters + std::uint32_t(v), r8(counters + std::uint32_t(v)) - 1);
                const auto n = s8(counters + std::uint32_t(v));
                if (n == 0) voice_silent(v);
                else if (n < 0) w8(counters + std::uint32_t(v), 4);
            }
        }
        V = r32(V);
    }
    bool duck = false;
    while (s32(pending_ticks) > 0) {
        w32(timestamp, r32(timestamp) + 1);
        handlers_tick();
        flush_key_off();
        flush_key_on();
        if (r32(countdown_a) || r32(countdown_b)) unhosted("0000d598", "the reverb countdown");
        if (!(r32(timestamp) & 1) && r32(lfo_list)) unhosted("000029ac", "the LFO tick");
        if (!(r32(timestamp) & 3)) duck = true;
        w32(pending_ticks, r32(pending_ticks) - 1);
    }
    w32(pending_ticks, 0);
    if (r32(watchdog)) unhosted("0000fba4", "the 989snd tick watchdog");
    w32(busy, 0);
    return duck;
}

// 1af50.
void BankEngine::reapply_groups(std::uint32_t groups) {
    voice_lock(1, 0x8a);
    for (std::int32_t v = 0; v < 48; ++v) {
        const auto V = voice_addr(v);
        const auto h = r32(V + 0xc);
        if (h && (r8(h + 0x16) & 2)) continue;
        const auto state = r32(V + 8);
        if (state != 1 && state != 4) continue;
        if (!(groups & voice_group_bit(s8(V + 0x1b)))) continue;
        const auto entry = static_cast<std::uint16_t>(std::uint32_t(v / 24) | std::uint32_t(v % 24) << 1);
        spu_.set_param(entry, static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x20), s8(V + 0x1b))) >> 1));
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x100),
                       static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x22), s8(V + 0x1b))) >> 1));
    }
    voice_unlock();
}

std::uint32_t BankEngine::voice_groups_playing() const {
    std::uint32_t m = 0;
    for (std::int32_t v = 0; v < 48; ++v) {
        const auto V = voice_addr(v);
        const auto state = r32(V + 8);
        const auto h = r32(V + 0xc);
        if ((state == 1 || state == 4) && h != 0 && !(r8(h + 0x16) & 2)) m |= 1u << (std::uint32_t(s8(V + 0x1b)) & 31);
    }
    return m;
}

void BankEngine::stop_groups(std::uint32_t group_mask) {
    for (auto h = r32(active_head); h; h = r32(h + 0x24)) {
        if (!(group_mask & (1u << (std::uint32_t(s8(h + 0x17)) & 31)))) continue;
        const auto handle = r32(h);
        if (handle & 0x80000000u) stop(handle);
    }
}

// 19b80: VOLL, VOLR and PITCH 0 on every voice in use (+8) that the handler owns (+c).
void BankEngine::pause_voices(std::uint32_t h) {
    voice_lock(1, 0x7a);
    for (std::int32_t v = 0; v < 48; ++v) {
        const auto V = voice_addr(v);
        if (!r32(V + 8) || r32(V + 0xc) != h) continue;
        const auto entry = static_cast<std::uint16_t>(std::uint32_t(v / 24) | std::uint32_t(v % 24) << 1);
        spu_.set_param(entry, 0);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x100), 0);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x200), 0);
    }
    voice_unlock();
}

// 19e18: the same voices' VOLL / VOLR through AdjustVolToGroup and their pitch (1a070, 19fd0) again.
void BankEngine::resume_voices(std::uint32_t h) {
    voice_lock(1, 0x7b);
    for (std::int32_t v = 0; v < 48; ++v) {
        const auto V = voice_addr(v);
        if (!r32(V + 8) || r32(V + 0xc) != h) continue;
        const auto entry = static_cast<std::uint16_t>(std::uint32_t(v / 24) | std::uint32_t(v % 24) << 1);
        spu_.set_param(entry, static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x20), s8(V + 0x1b))) >> 1));
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x100),
                       static_cast<std::uint16_t>(std::uint16_t(adjust_vol_to_group(volume_, s16(V + 0x22), s8(V + 0x1b))) >> 1));
        const auto tone = r32(V + 0x14);
        std::int32_t o1 = 0, o2 = 0;
        note_pitch_offset(tone, s16(V + 0x24), s16(V + 0x26), s8(V + 0x18), s8(V + 0x19), o1, o2);
        spu_.set_param(static_cast<std::uint16_t>(entry | 0x200),
                       tone_pitch(s8(tone + 2), s8(tone + 3), static_cast<std::uint16_t>(o1), static_cast<std::int16_t>(o2)));
    }
    voice_unlock();
}

// dfd0(h, children): the children first (+2c, linked by +30, each with children 1), then +16 |= 2 and 19b80.
void BankEngine::pause_handler(std::uint32_t h, std::int32_t children) {
    if (children)
        for (auto c = r32(h + 0x2c); c; c = r32(c + 0x30)) pause_handler(c, 1);
    w8(h + 0x16, r8(h + 0x16) | 2);
    pause_voices(h);
}

// e03c(h, children): the children first, then 19e18 and +16 &= ~2.
void BankEngine::resume_handler(std::uint32_t h, std::int32_t children) {
    if (children)
        for (auto c = r32(h + 0x2c); c; c = r32(c + 0x30)) resume_handler(c, 1);
    resume_voices(h);
    w8(h + 0x16, r8(h + 0x16) & 0xfdu);
}

void BankEngine::pause_groups(std::uint32_t group_mask, bool pause) {
    for (auto h = r32(active_head); h; h = r32(h + 0x24)) {
        if (!(group_mask & (1u << (std::uint32_t(s8(h + 0x17)) & 31)))) continue;
        if (((r8(h + 0x16) & 2) == 0) != pause) continue;
        if (((r32(h) >> 24) & 0x1f) == 4) unhosted(pause ? "00011468" : "000116bc", "pausing / resuming a stream handle");
        if (pause) pause_handler(h, 1);
        else resume_handler(h, 1);
    }
}

void BankEngine::stop_all() {
    for (auto h = r32(active_head); h; h = r32(h + 0x24))
        if (r32(h + 0x28) == 0 && !(r8(h + 0x16) & 4)) stop_handler(h, 1, 1, 0);
    // 198f8 on a stack pair of -1 words (the host's scratch).
    constexpr auto masks = scratch + 0xf8;
    w32(masks, 0xffffffffu);
    w32(masks + 4, 0xffffffffu);
    key_off_now(masks, 1);
}

bool BankEngine::unload_bank(std::uint32_t head) {
    if (head == 0 || head == 0xffffffffu) {
        error(0x14);
        return false;
    }
    if (r32(head) != sblk) unhosted("00008f2c", "unloading a bank head other than SBlk");
    bool listed = false;
    for (auto p = r32(bank_list); p && !listed; p = r32(p + 0x30)) listed = p == head;
    if (!listed) {
        error(0x16);
        return false;
    }
    // 9b8c: c978(head, 1).
    for (std::int32_t i = 0; i < s16(head + 0x16); ++i) stop_sound(r32(head + 0x1c) + std::uint32_t(i) * 12, 1, 0);
    std::uint32_t prev = 0;
    for (auto p = r32(bank_list); p != head; p = r32(p + 0x30)) prev = p;
    if (prev) w32(prev + 0x30, r32(head + 0x30));
    else w32(bank_list, r32(head + 0x30));
    if (r32(head + 0x2c)) sram_free(r32(head + 0x24), r32(head + 0x2c));
    return true;
}

std::vector<std::uint32_t> BankEngine::banks() const {
    std::vector<std::uint32_t> out;
    for (auto p = r32(bank_list); p; p = r32(p + 0x30)) out.push_back(p);
    return out;
}

std::vector<std::uint32_t> BankEngine::active_handles() const {
    std::vector<std::uint32_t> out;
    for (auto h = r32(active_head); h; h = r32(h + 0x24)) out.push_back(r32(h));
    return out;
}

// 10324: slti on the group, so a negative group writes below the table as the IRX does.
void BankEngine::set_stream_group(std::int32_t group, std::int32_t lo, std::int32_t hi) {
    if (group >= 16) return;
    const auto a = 0x1e2acu + std::uint32_t(group) * 4u;
    w16(a, std::uint32_t(lo));
    w16(a + 2, std::uint32_t(hi));
}

}  // namespace omega::snd989
