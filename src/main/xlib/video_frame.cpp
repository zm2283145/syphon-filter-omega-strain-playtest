#include "video_frame.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr u32 dma = 0x537f80;            // DMA packet object (0037a9f0 passes 00537f80 to 00375650/00375c30)
constexpr u32 slot_manager = 0x539248;   // 003bafc0: lw a0, -6db8(0x540000)
constexpr u32 fade_slots = 0x4bc1b0;     // 0037a370: ld 4bc1b0 + slot*8
constexpr u32 timer = 0x10000800;        // T1_COUNT
constexpr u32 timer_mode = 0x10000810;   // T1_MODE
constexpr u32 tick_base = 0x533870;
constexpr u32 ticks = 0x533878;          // 64-bit accumulated ticks
constexpr u32 lock_flag = 0x533880;      // lbu: interrupts are masked around frees
constexpr u32 lock_depth = 0x533888;
constexpr u32 free_queue = 0x533890;     // 0036c960 queue entries
constexpr u32 free_count = 0x533c90;
constexpr u32 pools = 0x533cb0;          // pool k at 00533cb0 + k*818
constexpr u32 gs_csr = 0x12001000;
constexpr u32 frames_per_tick = 0x490ca000;   // 576000.0f
constexpr u32 tick_divisor = 0x40b851ec;      // 5.76f

std::string hex(u32 value) {
    char text[12];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

u32 r32(const LevelLoadMemory& m, u32 a) { return m.read(a, 4); }
void w32(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 4, v); }
u64 r64(const LevelLoadMemory& m, u32 a) { return m.read(a, 4) | static_cast<u64>(m.read(a + 4, 4)) << 32; }
void w64(LevelLoadMemory& m, u32 a, u64 v) {
    m.write(a, 4, static_cast<u32>(v));
    m.write(a + 4, 4, static_cast<u32>(v >> 32));
}
u64 sx(u32 v) { return static_cast<u64>(static_cast<std::int64_t>(static_cast<s32>(v))); }
u32 sra(u32 v, int n) { return static_cast<u32>(static_cast<s32>(v) >> n); }

float fbits(u32 v) { return std::bit_cast<float>(v); }
u32 bitsf(float v) { return std::bit_cast<u32>(v); }
// div.s: results flush to zero; a zero divisor gives +-FLT_MAX (sign of S*T, + when S is 0).
float div_s(float a, float b) {
    if (b == 0)
        return std::copysign(std::numeric_limits<float>::max(),
                             a != 0 && std::signbit(a) != std::signbit(b) ? -1.0F : 1.0F);
    const float r = a / b;
    return std::fpclassify(r) == FP_SUBNORMAL ? (std::signbit(r) ? -0.0F : 0.0F) : r;
}
float cvt_s_w(u32 v) { return static_cast<float>(static_cast<s32>(v)); }
// The compiler's unsigned-to-float: cvt.s.w when the sign bit is clear, else
// cvt.s.w((u >> 1) | (u & 1)) doubled with add.s.
float u2f(u32 u) {
    if (static_cast<s32>(u) >= 0) return cvt_s_w(u);
    const float half = cvt_s_w((u >> 1) | (u & 1));
    return half + half;
}
// cvt.w.s truncates and saturates.
s32 cvt_w_s(float f) {
    const double d = f;
    if (std::isnan(d)) throw std::runtime_error("video frame 0037a9f0: cvt.w.s of NaN");
    if (d >= 2147483647.0) return std::numeric_limits<s32>::max();
    if (d <= -2147483648.0) return std::numeric_limits<s32>::min();
    return static_cast<s32>(d);
}
// div: the EE leaves LO undefined for a zero divisor, so that is rejected.
s32 div_lo(u32 a, u32 b, const char* where) {
    const auto x = static_cast<s32>(a), y = static_cast<s32>(b);
    if (y == 0) throw std::runtime_error(std::string("video frame ") + where + ": division by zero");
    if (x == std::numeric_limits<s32>::min() && y == -1) return x;
    return x / y;
}

[[noreturn]] void forever(const char* where, u32 reg, const char* what) {
    throw std::runtime_error(std::string("video frame ") + where + ": " + what + " (" + hex(reg) +
                             ") stays busy in static memory; the original would wait forever");
}

// Lock pattern around every free: lbu 00533880 -> _DisableIntc(9), then the
// depth word 00533888 is incremented; the release mirrors it.
void lock(LevelLoadMemory& m, LevelLoadHost& host) {
    if (m.read(lock_flag, 1)) host.call(m, 0x10dc40, {9});
    w32(m, lock_depth, r32(m, lock_depth) + 1);
}
void unlock(LevelLoadMemory& m, LevelLoadHost& host) {
    if (m.read(lock_flag, 1)) host.call(m, 0x10dca8, {9});
    w32(m, lock_depth, r32(m, lock_depth) - 1);
}

// 00383560 / 0037fba0: list erase of node (+0 prev, +4 next) with its count,
// then 0036b130(0, node, file, 0x64).
void erase(LevelLoadMemory& m, LevelLoadHost& host, u32 count, u32 node, u32 file) {
    (void)r32(m, node + 4);   // *it = node->next (the caller's iterator; unused afterwards)
    {
        const u32 next = r32(m, node + 4), prev = r32(m, node);
        w32(m, prev + 4, next);
    }
    {
        const u32 prev = r32(m, node), next = r32(m, node + 4);
        w32(m, next, prev);
    }
    if (node) {
        lock(m, host);
        host.call(m, 0x36b130, {0, node, file, 0x64});
        unlock(m, host);
    }
    w32(m, count, r32(m, count) - 1);
}

// 00382c30(manager, index): release one texture slot.
void release_slot_00382c30(LevelLoadMemory& m, LevelLoadHost& host, u32 mgr, u32 index) {
    const u32 offset = index << 2;
    const u32 object = r32(m, r32(m, mgr + 8) + offset);
    if (m.read(object + 0x62, 1) == 0) return;
    m.write(object + 0x62, 1, 0);
    m.write(r32(m, r32(m, mgr + 8) + offset) + 0x60, 1, 0);
    m.write(r32(m, r32(m, mgr + 8) + offset) + 0x61, 1, 0);
    const u32 end = mgr + 0x420;
    for (u32 node = r32(m, mgr + 0x424); node != end; node = r32(m, node + 4)) {
        if (r32(m, node + 8) == index) {
            erase(m, host, mgr + 0x41c, node, 0x49c220);
            break;
        }
    }
    if (r32(m, mgr + 0x41c) == 0) w32(m, mgr + 4, 0x4000);
}

// 003bafc0(entry): release the slot pair {+0, +4}; +4 = -1 means none.
void release_pair_003bafc0(LevelLoadMemory& m, LevelLoadHost& host, u32 entry) {
    release_slot_00382c30(m, host, r32(m, slot_manager), r32(m, entry));
    const u32 second = r32(m, entry + 4);
    if (second != 0xffffffffU) release_slot_00382c30(m, host, r32(m, slot_manager), second);
}

// 00375c30(dma, 1, 1, qwc, 0, 1): reserve a tagged VIF1 packet of qwc
// quadwords; returns the tag address + 8.
u32 reserve_00375c30(LevelLoadMemory& m, u32 qwc) {
    constexpr u32 channel = 1;
    const u32 buffer = r32(m, dma + 4 * channel);
    if (!(r32(m, buffer + 0x10) + qwc * 16 < r32(m, buffer + 8))) {
        w32(m, dma + 0xd8, channel);
        w32(m, buffer + 0x10, r32(m, r32(m, dma + 0xcc + 4 * channel) + 0xc));
    }
    u32 at = r32(m, buffer + 0x10);
    w32(m, buffer + 0x10, at + 8);
    w64(m, at, sx(1U << 28) | qwc);
    at = r32(m, buffer + 0x10);
    w32(m, buffer + 0x10, at + 8);
    w64(m, at, 0);
    at = r32(m, buffer + 0x10);
    const u32 result = r32(m, buffer + 0x10);
    w32(m, buffer + 0x10, at + qwc * 16);
    return result - 8;
}

// 00375770(dma, 1): close the VIF1 packet and start channel 1 (chain mode).
void kick_00375770(LevelLoadMemory& m, LevelLoadHost& host) {
    if (static_cast<s32>(r32(m, dma + 0xd8)) >= 0) return;
    const u32 buffer = r32(m, dma + 4);
    const u32 cursor = r32(m, buffer + 0x10);
    if (cursor == r32(m, buffer + 0xc)) return;
    w32(m, buffer + 0x10, cursor + 8);
    w64(m, cursor, 0x70000000);
    const u32 next = r32(m, buffer + 0x10);
    w32(m, buffer + 0x10, next + 8);
    w64(m, next, 0);
    const u32 start = r32(m, buffer + 0xc);
    if (r32(m, 0x10009000) & 0x100) forever("00375770", 0x10009000, "VIF1 CHCR STR");
    w32(m, 0x10009020, 0);                                  // D1_QWC
    w32(m, 0x1000e020, r32(m, 0x1000e020) | 2);             // D_PCR |= CPC1
    w32(m, 0x1000e010, 2);                                  // D_STAT: clear CIS1
    if ((start & 0x70000000) == 0x70000000) {
        w32(m, 0x10009030, start | 0x80000000);             // D1_TADR, scratchpad
    } else {
        w32(m, 0x10009030, start & 0x0fffffff);
        host.call(m, 0x10d3e0, {0});                        // FlushCache(0)
    }
    w32(m, 0x10009000, 0x1c5);                              // D1_CHCR: chain, TTE, STR
}

// 00375650: flip one double-buffered channel (buffer +slot, size, previous,
// index, base of the 0x60-strided descriptors).
void flip(LevelLoadMemory& m, u32 slot, u32 size, u32 previous, u32 index, u32 base) {
    const u32 buffer = r32(m, dma + slot);
    const auto bytes = static_cast<s32>(r32(m, buffer + 0x10) - r32(m, buffer + 0xc));
    const s32 quads = bytes >= 0 ? bytes >> 3 : static_cast<s32>(static_cast<u32>(bytes) + 7) >> 3;
    w32(m, dma + size, static_cast<u32>(quads) << 3);
    w32(m, dma + previous, r32(m, dma + slot));
    w32(m, dma + index, r32(m, dma + index) ^ 1);
    w32(m, dma + slot, dma + r32(m, dma + index) * 0x60 + base);
    const u32 fresh = r32(m, dma + slot);
    w32(m, fresh + 0x10, r32(m, fresh + 0xc));
    w32(m, fresh + 0x14, 0);
    m.write(fresh + 0x1c, 1, 0);
}

void flip_00375650(LevelLoadMemory& m, LevelLoadHost& host) {
    kick_00375770(m, host);
    flip(m, 4, 0xec, 0xd0, 0xe0, 0x2c);
    flip(m, 8, 0xf0, 0xd4, 0xe4, 0x4c);
    deferred_free_pump_0036c960(m, host);
    if (static_cast<s32>(r32(m, dma + 0xd8)) >= 0) w32(m, dma + 0xd8, 0xffffffffU);
}

// 00382060 / 003820c0 on the +1058 object.
void close_00382060(LevelLoadMemory& m, u32 p) {
    const u32 target = r32(m, p + 0x484);
    if (target) {
        if (m.read(p + 0x489, 1) != 0 || r32(m, p + 0x480) == 1)
            w32(m, target + 8, 0x13000000);
        else
            w32(m, target, 0x13000000);
    }
    w32(m, 0x539328, 0);
    w32(m, 0x539330, r32(m, 0x539330) ^ 1);
}
void reset_003820c0(LevelLoadMemory& m, u32 p) {
    w32(m, p + 0x480, 0);
    w32(m, p + 0x484, 0);
    m.write(p + 0x488, 1, 0);
    m.write(p + 0x489, 1, 0);
}

// 0037b0c0(v, force): age the +1108 buffer list, freeing expired entries.
void release_buffers_0037b0c0(LevelLoadMemory& m, LevelLoadHost& host, u32 v, bool force) {
    if (r32(m, v + 0x1108) == 0) return;
    const u32 end = v + 0x110c;
    u32 node = r32(m, v + 0x1110);
    while (node != end) {
        w32(m, node + 0x10, r32(m, node + 0x10) - 1);
        if (r32(m, node + 0x10) != 0 && !force) {
            node = r32(m, node + 4);
            continue;
        }
        const u32 entry = node;
        const u32 pool = r32(m, entry + 0xc);
        node = r32(m, node + 4);
        const u32 block = r32(m, entry + 8);
        lock(m, host);
        if (pool == 0xffffffffU)
            host.call(m, 0x36b130, {0, block, 0x4bc200, 0x812});
        else
            host.call(m, 0x36b130, {pool, block, 0x4bc200, 0x814});
        unlock(m, host);
        erase(m, host, v + 0x1108, entry, 0x4bc2a8);
    }
}

// 0037b000(v): roll the per-frame statistics.
void roll_0037b000(LevelLoadMemory& m, u32 v) {
    for (const u32 k : {0u, 4u}) {
        const u32 now = r32(m, v + 0x102c + k);
        u32 peak = r32(m, v + 0x1050 + k);
        if (!(static_cast<s32>(now) < static_cast<s32>(peak))) peak = now;
        w32(m, v + 0x1050 + k, peak);
    }
    w32(m, v + 0x1020, r32(m, v + 0x1020) ^ 1);
    const u32 row = v + r32(m, v + 0x1020) * 0x38;
    for (u32 k = 0; k < 16; k += 4) w32(m, row + 0xfd8 + k, r32(m, row + 0xfc4 + k));
    w32(m, v + 0x1040, 0);
    w32(m, v + 0x1034, 0);
    w32(m, v + 0x102c, 0);
    w32(m, v + 0x1038, 0);
    w32(m, v + 0x1030, 0);
    if (0 < static_cast<s32>(r32(m, v + 0x1028))) {
        s32 i = 0;
        u32 offset = 0;
        do {
            const u32 table = r32(m, v + 0x103c);
            ++i;
            w32(m, table + offset, 0);
            offset += 4;
        } while (i < static_cast<s32>(r32(m, v + 0x1028)));
    }
    w32(m, v + 0x1060, 0xffffffffU);
}

// 0037a370(v, w, h, rgba, 0, slot): full-screen fade quad.
void fade_quad_0037a370(LevelLoadMemory& m, u32 v, u32 w, u32 h, u32 alpha, u32 slot) {
    const u32 p = reserve_00375c30(m, 0xc);
    const u32 hw = w >> 1, hh = h >> 1;
    w64(m, p, 0x5000000c11000000ULL);
    w64(m, p + 8, 0x100000000000800bULL);
    u32 q = p + 0x10;
    w64(m, q, 0xe);
    w64(m, q + 8, r64(m, v + 0x90));
    q += 0x10;
    w64(m, q, 0x4c);
    w64(m, q + 8, r64(m, v + 0xa0));
    q += 0x10;
    w64(m, q, 0x4e);
    w64(m, q + 8, r64(m, v + 0xa8));
    q += 0x10;
    w64(m, q, 0x18);
    w64(m, q + 8, static_cast<u64>(w - 1) << 16 | static_cast<u64>(h - 1) << 48);
    q += 0x10;
    w64(m, q, 0x40);
    w64(m, q + 8, 0x31001);
    q += 0x10;
    w64(m, q, 0x47);
    w64(m, q + 8, 1);
    q += 0x10;
    w64(m, q, 0x1a);
    w64(m, q + 8, 0x4e);
    q += 0x10;
    w64(m, q, 0);
    w64(m, q + 8, sx(0x80) | sx(0x80) << 8 | sx(0x80) << 16 | sx(alpha) << 24 | 0x3f800000ULL << 32);
    q += 0x10;
    w64(m, q, 1);
    w64(m, q + 8, r64(m, fade_slots + slot * 8));
    q += 0x10;
    w64(m, q, 0x42);
    w64(m, q + 8, static_cast<u64>((0x800 - hw) << 4) | static_cast<u64>((0x800 - hh) << 4) << 16);
    q += 0x10;
    w64(m, q, 5);
    w64(m, q + 8, static_cast<u64>((hw + 0x800) << 4) | static_cast<u64>((hh + 0x800) << 4) << 16);
    q += 0x10;
    w64(m, q, 5);
    m.write(v + 0xd0, 1, 1);
}

// 0037a5c0(v): frame clear packet.
void clear_0037a5c0(LevelLoadMemory& m, u32 v) {
    const u32 fb = r32(m, v + 0x12c + 4 * r32(m, v + 0x140));
    const u32 p = reserve_00375c30(m, 8);
    w64(m, p, 0x5000000811000000ULL);
    w64(m, p + 8, 0x1000000000008007ULL);
    u32 q = p + 0x10;
    w64(m, q, 0xe);
    w64(m, q + 8, 1);
    q += 0x10;
    w64(m, q, 0x1a);
    w64(m, q + 8, sx(r32(m, v + 0x2c)) << 24 | (static_cast<u64>(fb) | sx(sra(r32(m, v + 0x1c), 6)) << 16));
    q += 0x10;
    w64(m, q, 0x4c);
    w64(m, q + 8, 0x30000);
    q += 0x10;
    w64(m, q, 0x47);
    w64(m, q + 8, 6);
    q += 0x10;
    w64(m, q, 0);
    w64(m, q + 8, r64(m, v + 0xc8));
    q += 0x10;
    w64(m, q, 1);
    {
        const u32 h = r32(m, v + 0x20), w = r32(m, v + 0x1c);
        w64(m, q + 8, sx((0x800 - sra(w, 1)) << 4) | sx((0x800 - sra(h, 1)) << 4) << 16);
    }
    q += 0x10;
    w64(m, q, 5);
    {
        const u32 h = r32(m, v + 0x20), w = r32(m, v + 0x1c);
        w64(m, q + 8, sx((sra(w, 1) + 0x800) << 4) | sx((sra(h, 1) + 0x800) << 4) << 16);
    }
    q += 0x10;
    w64(m, q, 5);
    m.write(v + 0xd0, 1, 1);
}

// The GS register images +90/+98/+a0/+a8 (FRAME, FRAME with mask, ZBUF,
// XYOFFSET) shared by 0037a9f0 and 0037a750, then the +d0 and record flags.
void register_images(LevelLoadMemory& m, u32 v) {
    const u32 index = r32(m, v + 0x140);
    const u32 w = r32(m, v + 0x1c), h = r32(m, v + 0x20);
    const u32 interlace = m.read(v + 0x64, 1);
    const u32 zbuf_hi = r32(m, v + 0xfc), zbuf = r32(m, v + 0x134), zpsm = r32(m, v + 0x30);
    const u32 mask = r32(m, v + 0xf8), psm = r32(m, v + 0x2c);
    const u32 fb = r32(m, v + 0x12c + index * 4);
    const u32 xo = (0x800 - (w >> 1)) << 4;
    u32 yo = (0x800 - (h >> 1)) << 4;
    if (interlace) yo += static_cast<u32>(((r64(m, gs_csr) >> 13) & 1) == 0) << 3;
    const u64 frame = static_cast<u64>(fb) | static_cast<u64>(w >> 6) << 16 | static_cast<u64>(psm) << 24;
    w64(m, v + 0x90, static_cast<u64>(mask) << 32 | frame);
    w64(m, v + 0x98, frame | 0xffffffULL << 32);
    w64(m, v + 0xa0, static_cast<u64>(zbuf_hi) << 32 | (static_cast<u64>(zbuf) | static_cast<u64>(zpsm) << 24));
    w64(m, v + 0xa8, static_cast<u64>(xo) | static_cast<u64>(yo) << 32);
    m.write(v + 0xd0, 1, 1);
    m.write(v + r32(m, v + 0xa70) * 0x110 + 0x2f6, 1, 1);
}

// 0037b330(v): display registers when +1100 selects a texture as display 1.
void display_0037b330(LevelLoadMemory& m, u32 v) {
    const u32 field = r32(m, v + 0x140) == 0;
    const u32 flags = r32(m, v);
    const u32 object = r32(m, r32(m, r32(m, v + 0x1058) + 8) + r32(m, v + 0x1100) * 4);
    const u64 fb = sx(r32(m, v + 0x12c + field * 4));
    const u32 sub = r32(m, object + 0x44);
    const bool f4 = flags & 4, f2 = flags & 2;
    const u32 dx = f2 ? 0x290 : 0x27c;
    const u32 sub_w = m.read(sub + 4, 2), sub_h = m.read(sub + 6, 2), sub_psm = m.read(sub + 0xa, 2);
    const u32 tbp = static_cast<u32>(r64(m, object + 0x50) & 0x3fff) >> 5;
    w64(m, 0x12000000, sx(r32(m, v + 0x1104)) << 8 | 0x63);                       // PMODE
    const u64 dispfb1 = sx(sub_psm) << 15 | (sx(tbp) | sx(sra(sub_w, 6)) << 9);
    w64(m, 0x12000020, static_cast<u64>(!f4) | static_cast<u64>(m.read(v + 0x64, 1)) << 1);   // SMODE2
    w64(m, 0x12000070, sx(r32(m, v + 0x2c)) << 15 | (fb | sx(sra(r32(m, v + 0x1c), 6)) << 9));  // DISPFB2
    w64(m, 0x12000090, dispfb1);                                                  // DISPFB1
    {
        const u32 w = r32(m, v + 0x10), h = r32(m, v + 0x14);
        const u32 magh = static_cast<u32>(div_lo(w + 0x9ff, w, "0037b330")) - 1;
        const u32 dy = f2 ? (f4 ? 0x24 : 0x48) : (f4 ? 0x19 : 0x32);
        w64(m, 0x12000080, static_cast<u64>(dx) | static_cast<u64>(dy) << 12 | static_cast<u64>(magh) << 23 |
                               static_cast<u64>((w << 2) - 1) << 32 | static_cast<u64>(h - 1) << 44);  // DISPLAY2
    }
    const u32 w4 = r32(m, v + 0x10) << 2;
    s32 magh = div_lo(w4, sub_w, "0037b330") - 1;
    if (magh < 0) magh = 0;
    const u32 h = r32(m, v + 0x14);
    s32 magv = div_lo(h, sub_h, "0037b330") - 1;
    if (magv < 0) magv = 0;
    const u32 dy = f2 ? (f4 ? 0x24 : 0x48) : (f4 ? 0x19 : 0x32);
    w64(m, 0x120000a0, static_cast<u64>(dx) | static_cast<u64>(dy) << 12 | static_cast<u64>(magh) << 23 |
                           static_cast<u64>(magv) << 27 | static_cast<u64>(w4 - 1) << 32 |
                           static_cast<u64>(h - 1) << 44);                                    // DISPLAY1
}

// 0037b5c0(v): display registers for the frame just drawn.
void display_0037b5c0(LevelLoadMemory& m, u32 v) {
    if (r32(m, v + 0x1100) != 0xffffffffU) {
        display_0037b330(m, v);
        return;
    }
    const u32 field = r32(m, v + 0x140) == 0;
    const u32 flags = r32(m, v);
    const bool f4 = flags & 4, f2 = flags & 2;
    const u64 fb = sx(r32(m, v + 0x12c + field * 4));
    w64(m, 0x12000000, 0xff61);                                                                  // PMODE
    w64(m, 0x12000020, static_cast<u64>(!f4) | static_cast<u64>(m.read(v + 0x64, 1)) << 1);    // SMODE2
    w64(m, 0x12000070, sx(r32(m, v + 0x2c)) << 15 | (fb | sx(sra(r32(m, v + 0x1c), 6)) << 9));  // DISPFB2
    const u32 w = r32(m, v + 0x10);
    const auto negative = static_cast<s32>(0U - w);
    const u32 half = negative >= 0 ? static_cast<u32>(negative >> 1)
                                   : static_cast<u32>(static_cast<s32>(static_cast<u32>(negative) + 1) >> 1);
    const u32 shift_x = f4 ? half : 0;
    const u32 shift_y = !f4 && f2 ? 4 : 0;
    // The div runs on both paths; its quotient is only used without flag 4.
    const u32 magh = f4 ? 1 : static_cast<u32>(div_lo(w + 0x9ff, w, "0037b5c0")) - 1;
    const u32 h = r32(m, v + 0x14);
    const u32 width = w * (magh + 1) - 1;   // mult a0, t0, v1
    const u32 dx = (f2 ? 0x290 : 0x27c) + shift_x;
    const u32 dy = (f2 ? (f4 ? 0x24 : 0x48) : (f4 ? 0x19 : 0x32)) + shift_y;
    w64(m, 0x12000080, static_cast<u64>(dx) | static_cast<u64>(dy) << 12 | static_cast<u64>(magh) << 23 |
                           static_cast<u64>(width) << 32 | static_cast<u64>(h - 1) << 44);  // DISPLAY2
}

// 003d24b0(listeners, frame time): when +c is set, method +c on every
// listener (then +c = 0); always method +8 with $f12 = frame time.
void listeners_003d24b0(LevelLoadMemory& m, LevelLoadHost& host, u32 list, u32 time) {
    const u32 end = list + 4;
    if (m.read(list + 0xc, 1)) {
        for (u32 node = r32(m, list + 8); node != end; node = r32(m, node + 4)) {
            const u32 listener = r32(m, node + 8);
            host.call(m, r32(m, r32(m, listener + 0x10) + 0xc), {listener});
        }
        m.write(list + 0xc, 1, 0);
    }
    for (u32 node = r32(m, list + 8); node != end; node = r32(m, node + 4)) {
        const u32 listener = r32(m, node + 8);
        host.call_float(m, r32(m, r32(m, listener + 0x10) + 8), {listener}, {time});
    }
}

// Both entries poll the DMA/FIFO status before touching the packets.
void wait_idle(LevelLoadMemory& m, const char* where) {
    if (r32(m, 0x10009000) & 0x100) forever(where, 0x10009000, "VIF1 CHCR STR");
    if (r32(m, 0x10003c00) & 3) forever(where, 0x10003c00, "VIF1_STAT VPS");
    if (r32(m, 0x10003c00) & 0x1f000000) forever(where, 0x10003c00, "VIF1_STAT FQC");
    if (r32(m, 0x1000a000) & 0x100) forever(where, 0x1000a000, "GIF CHCR STR");
    if (r32(m, 0x10003020) & 0xc00) forever(where, 0x10003020, "GIF_STAT PATH");
}

// T1 + base, as unsigned float / 5.76, accumulated into 00533878; the timer
// restarts (T1_COUNT = 0, T1_MODE = 0x82) and the base is cleared.
void accumulate_ticks(LevelLoadMemory& m) {
    const u32 count = r32(m, timer) + r32(m, tick_base);
    const s32 step = cvt_w_s(div_s(u2f(count), fbits(tick_divisor)));
    w64(m, ticks, r64(m, ticks) + sx(static_cast<u32>(step)));
    w32(m, timer, 0);
    w32(m, timer_mode, 0x82);
    w32(m, tick_base, 0);
}

// GS CSR: sd 8 (VSINT), then poll until bit 3 reads back set.
void wait_vsync(LevelLoadMemory& m) {
    w64(m, gs_csr, 8);
    if (!(r64(m, gs_csr) & 8)) forever("0037a9f0", gs_csr, "GS CSR VSINT");
}

// 0037a750: the flag-8 frame (no fade, limiter or listeners).
void frame_flag8_0037a750(LevelLoadMemory& m, LevelLoadHost& host, u32 v) {
    wait_idle(m, "0037a750");
    close_00382060(m, r32(m, v + 0x1058));
    flip_00375650(m, host);
    release_buffers_0037b0c0(m, host, v, false);
    w32(m, v + 0x140, r32(m, v + 0x140) ^ 1);
    roll_0037b000(m, v);
    w32(m, v + 0x1d8, r32(m, v + 0x1d8) + 1);
    register_images(m, v);
    reset_003820c0(m, r32(m, v + 0x1058));
    accumulate_ticks(m);
}

// ---- 0037e780 (renderer init) and its pure callees.
constexpr u32 display_modes = 0x4bc1d0;   // 0037f740: six {w, h} float pairs
constexpr u32 alloc_file = 0x4bc200;      // 0036b180 file argument
constexpr u32 one = 0x3f800000;           // 1.0f

// mul.s: results flush to zero like div.s.
float mul_s(float a, float b) {
    const float r = a * b;
    return std::fpclassify(r) == FP_SUBNORMAL ? (std::signbit(r) ? -0.0F : 0.0F) : r;
}

// sq stores ignore the low four address bits.
void zero_quad(LevelLoadMemory& m, u32 a) {
    a &= ~15U;
    for (u32 k = 0; k < 16; k += 4) w32(m, a + k, 0);
}
void quad(LevelLoadMemory& m, u32 a, u32 x, u32 y, u32 z, u32 w) {
    a &= ~15U;
    w32(m, a, x);
    w32(m, a + 4, y);
    w32(m, a + 8, z);
    w32(m, a + 12, w);
}
// The vf0-derived identity of 0037e780: rows +30, +10, +20, +00 in store order.
void identity(LevelLoadMemory& m, u32 a) {
    quad(m, a + 0x30, 0, 0, 0, one);
    quad(m, a + 0x10, 0, one, 0, 0);
    quad(m, a + 0x20, 0, 0, one, 0);
    quad(m, a, one, 0, 0, 0);
}

// 001260f0(p, 0, n) (memset; every call here fills zero). Sizes are compared
// as sign-extended 64-bit values, so anything past EE RAM runs off its end.
void zero_fill_001260f0(LevelLoadMemory& m, u32 p, u32 n) {
    if (n > 0x2000000U)
        throw std::runtime_error("renderer init 0037e780: 001260f0 zero fill of " + hex(n) +
                                 " bytes runs past EE RAM");
    for (u32 k = 0; k < n; ++k) m.write(p + k, 1, 0);
}

// 0036b180(0, size, 004bc200, line) inside the 00533880/00533888 lock.
u32 locked_alloc(LevelLoadMemory& m, LevelLoadHost& host, u32 size, u32 line) {
    lock(m, host);
    const u32 p = host.call(m, 0x36b180, {0, size, alloc_file, line});
    unlock(m, host);
    return p;
}

// 003d28d0(p): listener list constructor (vtable 004e00a8, then 00139070's
// empty header at +4 and the +c notify byte).
u32 list_ctor_003d28d0(LevelLoadMemory& m, u32 p) {
    w32(m, p + 0x10, 0x4e00a8);
    w32(m, p, 0);
    w32(m, p + 8, p + 4);
    w32(m, p + 4, p + 4);
    m.write(p + 0xc, 1, 0);
    return p;
}

// 0037f740(v): display mode and the +3c..+60 / +f80..+f94 view constants.
void display_0037f740(LevelLoadMemory& m, u32 v) {
    const float w = cvt_s_w(r32(m, v + 0x10)), h = cvt_s_w(r32(m, v + 0x14));
    u32 tw = 0, th = 0;
    s32 mode = -1;
    for (u32 k = 0; k < 6; ++k) {
        if (w <= fbits(r32(m, display_modes + 8 * k))) tw = k;
        if (h <= fbits(r32(m, display_modes + 8 * k + 4))) th = k;
        if (tw != k || th != k) {
            mode = static_cast<s32>(k) - 1;
            break;
        }
    }
    if (mode < 0)
        throw std::runtime_error("renderer init 0037e780: 0037f740 finds no display mode for +10 = " +
                                 hex(r32(m, v + 0x10)) + ", +14 = " + hex(r32(m, v + 0x14)) +
                                 "; the original then indexes 004bc1d0 with a stale $t2");
    w32(m, v + 0xf80, 0);
    w32(m, v + 0xf84, 0);
    w32(m, v + 0xf88, r32(m, display_modes + 8 * static_cast<u32>(mode)));
    w32(m, v + 0xf8c, r32(m, display_modes + 8 * static_cast<u32>(mode) + 4));
    w32(m, v + 0x4c, bitsf(div_s(cvt_s_w(r32(m, v + 0x1c)), fbits(r32(m, v + 0xf88)))));
    w32(m, v + 0x50, bitsf(div_s(cvt_s_w(r32(m, v + 0x20)), fbits(r32(m, v + 0xf8c)))));
    w32(m, v + 0x54, 0);
    w32(m, v + 0x58, 0);
    w32(m, v + 0x5c, one);
    w32(m, v + 0x60, one);
    const u32 flags = r32(m, v);
    if (flags & 4) {
        const float ratio = div_s(cvt_s_w(r32(m, v + 0x1c)), fbits(0x44340000));   // 720.0f
        w32(m, v + 0x60, bitsf(mul_s(fbits(r32(m, v + 0x60)), ratio)));
    } else if (flags & 2) {
        w32(m, v + 0x60, bitsf(mul_s(fbits(r32(m, v + 0x60)), fbits(0x3f888889))));
    }
    w32(m, v + 0x44, one);
    w32(m, v + 0x48, one);
    w32(m, v + 0x3c, 0);
    w32(m, v + 0x40, 0);
    const float aspect = div_s(fbits(r32(m, v + 0xf88)), fbits(r32(m, v + 0xf8c)));
    w32(m, v + 0xf90, bitsf(aspect));
    w32(m, v + 0xf94, bitsf(aspect));
}

// 0037f670(v): GS memory layout in 2 KB pages (frame buffers +12c/+130,
// z buffer +134, texture base +138, the byte address +13c of the first free page).
void layout_0037f670(LevelLoadMemory& m, u32 v) {
    const u32 width_pages = sra(r32(m, v + 0x1c) + 0x3f, 6);
    const u32 color = r32(m, v + 0x24);
    const bool wide = color == 0x20 || color == 0x18;
    const u32 h = r32(m, v + 0x20);
    const u32 color_pages = width_pages * (wide ? sra(h + 0x1f, 5) : sra(h + 0x3f, 6));
    const u32 depth = r32(m, v + 0x28);
    const u32 depth_rows = depth == 0x10 ? sra(h + 0x3f, 6) : sra(h + 0x1f, 5);
    const u32 depth_pages = static_cast<s32>(depth) > 0 ? width_pages * depth_rows : 0;
    w32(m, v + 0x12c, 0);
    w32(m, v + 0x130, r32(m, v + 0x12c) + color_pages);
    w32(m, v + 0x138, r32(m, v + 0x130) + color_pages);
    w32(m, v + 0x134, r32(m, v + 0x138));
    w32(m, v + 0x13c, (r32(m, v + 0x134) + depth_pages) << 5);
    w32(m, v + 0x140, 0);
}

// The two +fb0 packet buffer sets of 0037e780 (stride 0x38) and their records.
void packet_buffers(LevelLoadMemory& m, LevelLoadHost& host, u32 v) {
    const u32 bytes = (r32(m, v + 0x1024) << 4) + 0x10;
    const u32 record_bytes = (r32(m, v + 0x1028) * 3 << 6) + 0x10;
    for (u32 i = 0; i < 2; ++i) {
        const u32 s = v + 0xfb0 + i * 0x38;
        w32(m, s, locked_alloc(m, host, bytes, 0x1a8));
        zero_fill_001260f0(m, r32(m, s), bytes);
        w32(m, s + 4, 0);
        w32(m, s + 8, locked_alloc(m, host, bytes, 0x1ae));
        zero_fill_001260f0(m, r32(m, s + 8), bytes);
        w32(m, s + 0xc, locked_alloc(m, host, bytes, 0x1af));
        zero_fill_001260f0(m, r32(m, s + 0xc), bytes);
        w32(m, s + 0x10, locked_alloc(m, host, record_bytes, 0x1b0));
        zero_fill_001260f0(m, r32(m, s + 0x10), record_bytes);
        const u32 first = r32(m, s);
        w32(m, s + 0x14, (first + 15) & ~15U);
        w32(m, s + 0x18, 0);
        w32(m, s + 0x1c, (r32(m, s + 8) + 15) & ~15U);
        w32(m, s + 0x20, (r32(m, s + 0xc) + 15) & ~15U);
        w32(m, s + 0x24, (r32(m, s + 0x10) + 15) & ~15U);
        for (s32 j = 0; j < static_cast<s32>(r32(m, v + 0x1028)); ++j) {
            const u32 at = static_cast<u32>(j) * 0xc0;
            const auto rec = [&] { return r32(m, s + 0x24) + at; };
            zero_quad(m, rec());
            zero_quad(m, rec() + 0x10);
            w64(m, rec() + 0x20, 0x1000000000000003ULL);
            w64(m, rec() + 0x28, 0xe);
            w64(m, rec() + 0x30, 0);
            w64(m, rec() + 0x38, 0x7f);
            w64(m, rec() + 0x40, 0);
            w64(m, rec() + 0x48, 0x7f);
            w64(m, rec() + 0x50, 0);
            w64(m, rec() + 0x58, 0x7f);
            zero_quad(m, rec() + 0x60);
            zero_quad(m, rec() + 0x70);
            for (u32 k = 0; k < 12; ++k) w32(m, rec() + 4 * k + 0x80, 0);
            w64(m, rec() + 0xb0, 0x1000000000000000ULL);
            w64(m, rec() + 0xb8, 0xe);
        }
    }
}
}  // namespace

void deferred_free_pump_0036c960(LevelLoadMemory& m, LevelLoadHost& host) {
    while (r32(m, free_count) != 0) {
        const u32 n = r32(m, free_count) - 1;
        const u32 masked = m.read(lock_flag, 1);
        w32(m, free_count, n);
        const u32 block = r32(m, free_queue + 4 * r32(m, free_count));
        if (masked) host.call(m, 0x10dc40, {9});
        w32(m, lock_depth, r32(m, lock_depth) + 1);
        if (block) {
            const u32 pool = static_cast<u32>(static_cast<std::int8_t>(m.read(block - 0xc, 1)) & 0xf);
            host.call(m, 0x36bc10, {pools + pool * 0x818, block, 0x4bbe50, 0x46});
        }
        unlock(m, host);
    }
}

std::uint32_t video_rate_1e8_0037ab70(std::uint32_t t1_count) {
    return bitsf(div_s(fbits(frames_per_tick), cvt_s_w(t1_count)));
}

void video_frame_begin_0037a9f0(LevelLoadMemory& m, LevelLoadHost& host, u32 v) {
    if (r32(m, v) & 8) {
        frame_flag8_0037a750(m, host, v);
        return;
    }
    for (u32 i = 0; i < 6; ++i) release_pair_003bafc0(m, host, v + 0x1068 + i * 0x14);
    if (const u32 alpha = r32(m, v + 0x1d0))
        fade_quad_0037a370(m, v, r32(m, v + 0x1c), r32(m, v + 0x20), alpha, 2);
    if (const u32 alpha = r32(m, v + 0x1d4))
        fade_quad_0037a370(m, v, r32(m, v + 0x1c), r32(m, v + 0x20), alpha, 3);
    wait_idle(m, "0037a9f0");
    if (static_cast<s32>(r32(m, v + 0x1d8)) < 2) {
        accumulate_ticks(m);
    } else {
        const float rate = fbits(frames_per_tick);
        {
            const u32 count = r32(m, timer);
            w32(m, v + 0x1e8, video_rate_1e8_0037ab70(count + r32(m, tick_base)));
        }
        bool waited = false;
        if (u32 limit = r32(m, v + 0x1e4)) {
            const u32 base = r32(m, tick_base);
            float fps = div_s(rate, cvt_s_w(r32(m, timer) + base));
            if (!(fps <= u2f(limit))) {
                waited = true;
                wait_vsync(m);
                limit = r32(m, v + 0x1e4);
                fps = div_s(rate, cvt_s_w(r32(m, timer) + base));
                // Nothing in static memory advances T1, so a second pass is identical.
                if (!(fps <= u2f(limit)))
                    throw std::runtime_error("video frame 0037a9f0: frame limiter +1e4 = " + hex(limit) +
                                             " waits on T1 (10000800), which static memory never advances");
            }
        }
        if ((r32(m, v + 0x1d0) != 0 || r32(m, v + 0x1d4) != 0) && !waited) wait_vsync(m);
        {
            const u32 count = r32(m, timer) + r32(m, tick_base);
            w32(m, v + 0x1e0, bitsf(div_s(cvt_s_w(count), rate)));
        }
        accumulate_ticks(m);
        display_0037b5c0(m, v);
    }
    close_00382060(m, r32(m, v + 0x1058));
    flip_00375650(m, host);
    release_buffers_0037b0c0(m, host, v, false);
    roll_0037b000(m, v);
    w32(m, v + 0x140, r32(m, v + 0x140) ^ 1);
    w32(m, v + 0x1d8, r32(m, v + 0x1d8) + 1);
    register_images(m, v);
    clear_0037a5c0(m, v);
    const u32 time = r32(m, v + 0x1e0);
    listeners_003d24b0(m, host, r32(m, v + 0x105c), time);
    reset_003820c0(m, r32(m, v + 0x1058));
}

void renderer_init_0037e780(LevelLoadMemory& m, LevelLoadHost& host, u32 v, u32 width, u32 height,
                            u32 color_bits, u32 depth_bits, u32 flags, u32 qwords, u32 records) {
    if (r32(m, v) != 0xffffffffU) host.call(m, 0x37e320, {v});
    w32(m, v, flags);
    w32(m, v + 0x10, width);
    w32(m, v + 0x14, height);
    w32(m, v + 0x18, color_bits);
    w32(m, v + 0x1c, width);
    if (r32(m, v) & 1)
        w32(m, v + 0x20, static_cast<s32>(height) >= 0 ? sra(height, 1) : sra(height + 1, 1));
    else
        w32(m, v + 0x20, height);
    w32(m, v + 0x24, color_bits);
    w32(m, v + 0x28, depth_bits);
    m.write(v + 0x64, 1, (r32(m, v) & 1) != 0);
    // +2c: GS PSMCT32 / PSMCT24 / PSMCT16 (2) or PSMCT16S (0xa).
    if (color_bits == 0x10)
        w32(m, v + 0x2c, color_bits == depth_bits ? 2 : 0xa);
    else
        w32(m, v + 0x2c, color_bits == 0x18 ? 1 : 0);
    // +30 PSMZ, +38 the depth range as a float.
    if (depth_bits == 0x10) {
        w32(m, v + 0x30, depth_bits == color_bits ? 0x32 : 0x3a);
        w32(m, v + 0x34, 0);
        w32(m, v + 0x38, 0x477fff00);
    } else if (depth_bits == 0x18) {
        w32(m, v + 0x30, 0x31);
        w32(m, v + 0x34, 0);
        w32(m, v + 0x38, 0x4b7fffff);
    } else {
        w32(m, v + 0x30, 0x30);
        w32(m, v + 0x34, 0);
        w32(m, v + 0x38, 0x4f800000);
    }
    w32(m, v + 0x88, 0);
    const float scale = fbits(0x457ff000);   // 4095.0f
    {
        const float x = div_s(scale, cvt_s_w(r32(m, v + 0x1c)));
        w32(m, v + 0x68 + (r32(m, v + 0x88) << 2), bitsf(x));
    }
    {
        const float y = div_s(scale, cvt_s_w(r32(m, v + 0x20)));
        w32(m, v + 0x78 + (r32(m, v + 0x88) << 2), bitsf(y));
    }
    {
        const u32 record = r32(m, v + 0xa70);
        m.write(v + record * 0x110 + 0x2f6, 1, 1);
        host.call(m, 0x102a28, {record});
    }
    {
        const u32 f = r32(m, v);
        if (f & 4)
            host.call(m, 0x102888, {0, 0, 0x50, 1});
        else if (f & 2)
            host.call(m, 0x102888, {0, 1, 3, 1});
        else
            host.call(m, 0x102888, {0, 1, 2, 1});
    }
    host.call(m, 0x10d3e0, {0});
    while (host.call(m, 0x103710, {0}) == 0) {
    }
    host.call(m, 0x1037a8, {0, 0});
    w32(m, v + 0xa70, 0);
    identity(m, v + 0xa80);
    w32(m, v + 0xa74, 0);
    identity(m, v + 0xec0);
    identity(m, v + 0xf40);
    m.write(v + 0xa78, 1, 1);
    zero_quad(m, v + 0xfa0);
    display_0037f740(m, v);
    register_images(m, v);   // inlined at 0037eae0..0037ec20, before 0037f670 lays out +12c/+134
    for (const u32 k : {0xe80u, 0xe90u, 0xea0u, 0xeb0u, 0xe84u, 0xe94u, 0xea4u, 0xeb4u, 0xe88u, 0xe98u, 0xea8u,
                        0xeb8u, 0xe8cu, 0xe9cu, 0xeacu, 0xebcu})
        w32(m, v + k, (k == 0xe80 || k == 0xea4 || k == 0xe98 || k == 0xebc) ? one : 0);
    layout_0037f670(m, v);
    {
        u32 p = locked_alloc(m, host, 0x494, 0x14b);
        if (p) p = host.call(m, 0x3831d0, {p, r32(m, v + 0x13c), 0x800});
        w32(m, v + 0x1058, p);
    }
    {
        u32 p = locked_alloc(m, host, 0x14, 0x14d);
        if (p) p = list_ctor_003d28d0(m, p);
        w32(m, v + 0x105c, p);
    }
    if (!(r32(m, v) & 8)) {
        for (u32 i = 0; i < 5; ++i)
            host.call(m, 0x3bb1c0, {v + 0x1068 + i * 0x14, 0x4bc208 + i * 0x10, 0x80, 0x80, 0x20, 0x10});
        host.call(m, 0x3bb1c0, {v + 0x10cc, 0x4bc258, 0x100, 0x100, r32(m, v + 0x24), 0});
    }
    w32(m, v + 0x1064, 0xffffffffU);
    w32(m, v + 0x10e0, 0);
    w32(m, v + 0x10e4, 0);
    w32(m, v + 0xd8, 1);
    w32(m, v + 0xdc, 6);
    w32(m, v + 0xe0, 0);
    w32(m, v + 0xe4, 0);
    w32(m, v + 0xe8, 0);
    w32(m, v + 0xec, 0);
    w32(m, v + 0xf0, 1);
    w32(m, v + 0xf4, 2);
    w32(m, v + 0xf8, 0xff000000U);
    w32(m, v + 0xfc, 0);
    w32(m, v + 0x100, 1);
    w32(m, v + 0x104, 1);
    w32(m, v + 0x108, 0);
    w32(m, v + 0x10c, 1);
    for (u32 k = 0x110; k <= 0x120; k += 4) w32(m, v + k, 0);
    w32(m, v + 0x124, 1);
    w32(m, v + 0x128, 0);
    w64(m, v + 0x10e8, r64(m, fade_slots));
    // 0.0f * 256.0f for +170..+178 and cvt.w.s of those products for +160..+168.
    w32(m, v + 0x170, 0);
    w32(m, v + 0x174, 0);
    w32(m, v + 0x178, 0);
    w32(m, v + 0x17c, 0x43800000);   // 256.0f
    w32(m, v + 0x160, 0);
    w32(m, v + 0x164, 0);
    w32(m, v + 0x168, 0);
    w32(m, v + 0x16c, 0x80);
    w64(m, v + 0xb8, 0);
    m.write(v + 0xd0, 1, 1);
    w32(m, v + 0x1d8, 0);
    w32(m, v + 0x1060, 0xffffffffU);
    w32(m, v + 0x1100, 0xffffffffU);
    w32(m, v + 0x1d0, 0);
    w32(m, v + 0x1d4, 0);
    w32(m, v + 0x1e4, 0x3c);
    w32(m, v + 0x1e8, 0);
    w32(m, v + 0x1024, qwords == 0xffffffffU ? 0xdac : qwords);
    w32(m, v + 0x1028, records == 0xffffffffU ? 0x15e : records);
    packet_buffers(m, host, v);
    {
        const u32 bytes = r32(m, v + 0x1028) << 2;
        w32(m, v + 0x103c, locked_alloc(m, host, bytes, 0x1d0));
    }
    w32(m, v + 0x1020, 0);
    w32(m, v + 0x1050, 0);
    w32(m, v + 0x1054, 0);
    roll_0037b000(m, v);
    roll_0037b000(m, v);
    w64(m, v + 0x10e8, r64(m, fade_slots));
    w64(m, v + 0xc8, 0x3f80000000000000ULL);   // RGBAQ: r, g, b = cvt.w.s(0.0f) = 0, q = 1.0f
    w32(m, v + 0x1e0, 0);
    if (!(r32(m, v) & 8)) {
        host.call(m, 0x379fe0, {v, 0, 0, 0, 0x80});
        w32(m, 0x10003820, 6);   // VIF0_ERR
        w32(m, 0x10003c20, 6);   // VIF1_ERR
    }
}

}  // namespace omega
