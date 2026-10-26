#include "particle_vu1.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

// 132vd2 (port-side speed only): the FMAC on Advanced SIMD where the target has it (the PS Vita); see Machine::fmac_neon.
#if defined(__arm__) && defined(__ARM_NEON) && !defined(OMEGA_PARTICLE_NO_NEON)
#include <arm_neon.h>
#define OMEGA_PARTICLE_NEON 1
#else
#define OMEGA_PARTICLE_NEON 0
#endif
#if OMEGA_PARTICLE_NEON && defined(OMEGA_PARTICLE_NEON_CHECK)
#include <atomic>
#include <iostream>
#include <iterator>
#else
#undef OMEGA_PARTICLE_NEON_CHECK
#define OMEGA_PARTICLE_NEON_CHECK 0
#endif
#if defined(__GNUC__)
#define OMEGA_PARTICLE_INLINE inline __attribute__((always_inline))
#define OMEGA_PARTICLE_NOINLINE __attribute__((noinline))
#else
#define OMEGA_PARTICLE_INLINE inline
#define OMEGA_PARTICLE_NOINLINE
#endif

namespace omega::particle {
namespace {

[[noreturn]] void fail(std::uint32_t pair, const char* what) {
    char b[96];
    std::snprintf(b, sizeof b, "VU1 particle program: %s at pair %03x (not modelled)", what, pair);
    throw std::runtime_error(b);
}

constexpr int X = 8, Y = 4, Z = 2, W = 1, XYZ = X | Y | Z, XYZW = 15;
// Checkpoint 132cp8: the largest VU value (exponent 255, F all ones), the
// +-MAX of overflow and division by zero (VU User's Manual 2.1.1 / 2.3).
constexpr std::uint32_t kVuMax = 0x7fffffffu;

// The pair being executed, for the error messages.
thread_local std::uint32_t g_pair = 0;

// An operand: a zero exponent reads as signed zero; exponent 255 is the
// normal binade 1.F x 2^128 (the VU has no infinity or NaN, Manual 2.1.1).
// Checkpoint 132win9 (port-side speed only, the same values): the rare
// exponent-255 operand is out of line so that the common path inlines.
double daz_max_binade(std::uint32_t b) {
    const double m = std::ldexp(1.0 + static_cast<double>(b & 0x7fffffu) / 8388608.0, 128);
    return (b >> 31) ? -m : m;
}
inline double daz(std::uint32_t b) {
    const std::uint32_t ex = (b >> 23) & 0xff;
    if (ex == 0) return (b >> 31) ? -0.0 : 0.0;
    if (ex == 0xff) return daz_max_binade(b);
    return static_cast<double>(std::bit_cast<float>(b));
}

// A result: binary32 round-to-nearest (the interpreter's stand-in) with
// exponent 255 as a normal binade; past it, +-MAX (overflow); below the
// normal range, signed zero. The operands are binary32 values below 2^129,
// so one double operation then this rounding is the correctly rounded
// binary32 operation (53 >= 2 * 24 + 2).
// Checkpoint 132win9 (port-side speed only): the NaN / sign / infinity
// tests on the bits (MSVC's std::isnan / std::isinf are out-of-line calls).
inline std::uint32_t rnd(double x) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    if ((bits & 0x7fffffffffffffffull) > 0x7ff0000000000000ull) fail(g_pair, "a NaN result");
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & 0x80000000u;
    const double a = std::bit_cast<double>(bits & 0x7fffffffffffffffull);
    const float f = static_cast<float>(a);
    const std::uint32_t b = std::bit_cast<std::uint32_t>(f);
    if (b != 0x7f800000u) return sign | (((b >> 23) & 0xff) == 0 ? 0u : b);
    const float h = static_cast<float>(a * 0.5);  // the 2^128 binade at half scale
    if (std::bit_cast<std::uint32_t>(h) == 0x7f800000u) return sign | kVuMax;
    return sign | (std::bit_cast<std::uint32_t>(h) + 0x800000u);
}

Qword bc(const Qword& v, int k) { return {v[k], v[k], v[k], v[k]}; }
Qword imm(std::uint32_t b) { return {b, b, b, b}; }

enum class Op { add, sub, mul, madd, msub };

// Port-side speed only (132vd1, research/VITA.md "In-level renderer: per-draw cost"; the same bits): an FMAC lane in
// binary32 arithmetic when no operand has exponent 255. The operands are binary32 values (a zero exponent read as
// signed zero), so the binary32 operation is correctly rounded, the value rnd(double operation) gives (the double
// result is exact or innocuously double-rounded, 53 >= 2 * 24 + 2), and its sign is the double result's, zeros
// included. A binary32 result with exponent 255 (overflow past FLT_MAX) returns false: the double path then makes the
// 2^128 binade / +-MAX; a zero exponent keeps the sign only, as rnd does. No operand is an infinity or NaN here, so no
// result is a NaN. The PS Vita's particle programs run some 100 FMACs per particle, most of the in-level render CPU.
inline bool fast_operand(std::uint32_t b) { return (b & 0x7f800000u) != 0x7f800000u; }
inline float fast_value(std::uint32_t b) { return std::bit_cast<float>((b & 0x7f800000u) ? b : b & 0x80000000u); }
inline bool fast_result(float r, std::uint32_t& out) {
    const std::uint32_t b = std::bit_cast<std::uint32_t>(r);
    const std::uint32_t e = b & 0x7f800000u;
    if (e == 0x7f800000u) return false;
    out = e ? b : b & 0x80000000u;
    return true;
}
inline bool fast_lane(Op op, std::uint32_t sb, std::uint32_t tb, std::uint32_t accb, std::uint32_t& out) {
    if (!fast_operand(sb) || !fast_operand(tb)) return false;
    const float a = fast_value(sb), b = fast_value(tb);
    switch (op) {
    case Op::add: return fast_result(a + b, out);
    case Op::sub: return fast_result(a - b, out);
    case Op::mul: return fast_result(a * b, out);
    case Op::madd:
    case Op::msub: {
        std::uint32_t p;  // the product rounded first (MADD / MSUB)
        if (!fast_operand(accb) || !fast_result(a * b, p)) return false;
        const float c = fast_value(accb), q = std::bit_cast<float>(p);
        return fast_result(op == Op::madd ? c + q : c - q, out);
    }
    }
    return false;
}

#if OMEGA_PARTICLE_NEON
inline bool neon_matches_vfp() {
    std::uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    return (fpscr & 0x01f70000u) == 0x01000000u;  // FZ (bit 24) set; RMode (23:22) nearest; Stride, Len 0
}
#endif

#if OMEGA_PARTICLE_NEON_CHECK
// The check build (-DOMEGA_PARTICLE_NEON_CHECK, research/VITA.md "Particle programs on NEON (132vd2)"): every FMAC of a
// thread in the NEON mode runs both paths; a difference throws, and a log line counts them every 2^20 FMACs.
std::atomic<std::uint32_t> g_neon_checked{0}, g_neon_fallback{0};
void neon_check(Op op, bool fast, const Qword& got, const Qword& expect, const Qword& S, const Qword& T,
                const Qword& acc, int dest) {
    const std::uint32_t n = g_neon_checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!fast) g_neon_fallback.fetch_add(1, std::memory_order_relaxed);
    if (fast && got != expect) {
        char b[400];
        std::snprintf(b, sizeof b,
                      "VU1 particle NEON check: op %d dest %x at pair %03x: NEON %08x %08x %08x %08x, lanes %08x %08x "
                      "%08x %08x; S %08x %08x %08x %08x; T %08x %08x %08x %08x; ACC %08x %08x %08x %08x",
                      static_cast<int>(op), dest, g_pair, got[0], got[1], got[2], got[3], expect[0], expect[1],
                      expect[2], expect[3], S[0], S[1], S[2], S[3], T[0], T[1], T[2], T[3], acc[0], acc[1], acc[2],
                      acc[3]);
        throw std::runtime_error(b);
    }
    if ((n & 0xfffffu) == 0)
        std::cerr << "VU1 particle NEON check: " << n << " FMACs compared, " << g_neon_fallback.load()
                  << " to the lanes, 0 differences\n";
}
void neon_selftest();
#endif

struct Machine {
    Vu1State& s;

    Qword& v(int r) { return s.vf[r]; }

    void write(Qword& dst, const Qword& out, int dest) {
        if (&dst == &s.vf[0]) return;
        for (int k = 0; k < 4; ++k)
            if (dest & (8 >> k)) dst[k] = out[k];
    }

#if OMEGA_PARTICLE_NEON
    // 132vd2: set when the thread's VFP mode is the one Advanced SIMD always uses (flush-to-zero, round to nearest).
    bool neon = neon_matches_vfp();

    // Port-side speed only (132vd2, research/VITA.md "Particle programs on NEON (132vd2)"; the same bits): the four
    // lanes of fast_lane at once. Advanced SIMD arithmetic is IEEE binary32 round to nearest with flush-to-zero of
    // denormal operands and of results tiny before rounding (ARM ARM A2.7.4 / A2.7.5, the standard FPSCR value), which
    // is what the VFP instructions of fast_lane do under FPSCR FZ = 1 and RMode = 0, the mode the play threads and the
    // pool jobs run in (enable_ps2_denormal_flush, FloatControlsScope); `neon` is false otherwise. MADD / MSUB round
    // the product first (VMUL then VADD / VSUB, as fast_lane). Returns false, writing nothing, when a dest lane has an
    // operand (or the ACC of MADD / MSUB) or a result with exponent 255: fmac_lanes then decides every lane as before.
    OMEGA_PARTICLE_INLINE bool fmac_neon(Op op, Qword& dst, const Qword& S, const Qword& T, int dest) {
        const uint32x4_t e = vdupq_n_u32(0x7f800000u);
        const uint32x4_t mask = {(dest & X) ? ~0u : 0u, (dest & Y) ? ~0u : 0u, (dest & Z) ? ~0u : 0u,
                                 (dest & W) ? ~0u : 0u};
        const uint32x4_t sb = vld1q_u32(S.data()), tb = vld1q_u32(T.data());
        uint32x4_t bad = vorrq_u32(vceqq_u32(vandq_u32(sb, e), e), vceqq_u32(vandq_u32(tb, e), e));
        const float32x4_t a = vreinterpretq_f32_u32(sb), b = vreinterpretq_f32_u32(tb);
        float32x4_t r;
        switch (op) {
        case Op::add: r = vaddq_f32(a, b); break;
        case Op::sub: r = vsubq_f32(a, b); break;
        case Op::mul: r = vmulq_f32(a, b); break;
        case Op::madd:
        case Op::msub: {
            const uint32x4_t cb = vld1q_u32(s.acc.data());
            bad = vorrq_u32(bad, vceqq_u32(vandq_u32(cb, e), e));
            const float32x4_t p = vmulq_f32(a, b), c = vreinterpretq_f32_u32(cb);
            r = op == Op::madd ? vaddq_f32(c, p) : vsubq_f32(c, p);  // a product past FLT_MAX makes r infinite
            break;
        }
        default: return false;
        }
        const uint32x4_t rb = vreinterpretq_u32_f32(r);
        bad = vandq_u32(vorrq_u32(bad, vceqq_u32(vandq_u32(rb, e), e)), mask);
        const uint32x2_t half = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
        if (vget_lane_u32(vpmax_u32(half, half), 0)) return false;
        if (&dst == &s.vf[0]) return true;
        vst1q_u32(dst.data(), dest == XYZW ? rb : vbslq_u32(mask, rb, vld1q_u32(dst.data())));
        return true;
    }
#endif

    // FMAC: dst (a VF or the ACC) = op(S, T) on the dest lanes.
    OMEGA_PARTICLE_INLINE void fmac(Op op, Qword& dst, const Qword& S, const Qword& T, int dest) {
#if OMEGA_PARTICLE_NEON
#if OMEGA_PARTICLE_NEON_CHECK
        if (neon) {  // the check build: both paths into copies, compared; dst from the lanes as before
            static const bool tested = (neon_selftest(), true);
            (void)tested;
            Qword got = dst, expect = dst;
            const bool fast = fmac_neon(op, got, S, T, dest);
            fmac_lanes(op, expect, S, T, dest);
            neon_check(op, fast, got, expect, S, T, s.acc, dest);
            fmac_lanes(op, dst, S, T, dest);
            return;
        }
#else
        if (neon && fmac_neon(op, dst, S, T, dest)) return;
#endif
#endif
        fmac_lanes(op, dst, S, T, dest);
    }

    OMEGA_PARTICLE_NOINLINE void fmac_lanes(Op op, Qword& dst, const Qword& S, const Qword& T, int dest) {
        Qword out = dst;
        for (int k = 0; k < 4; ++k) {
            if (!(dest & (8 >> k))) continue;
            if (fast_lane(op, S[k], T[k], s.acc[k], out[k])) continue;
            const double a = daz(S[k]), b = daz(T[k]);
            double r = 0;
            switch (op) {
            case Op::add: r = a + b; break;
            case Op::sub: r = a - b; break;
            case Op::mul: r = a * b; break;
            case Op::madd: r = daz(s.acc[k]) + daz(rnd(a * b)); break;
            case Op::msub: r = daz(s.acc[k]) - daz(rnd(a * b)); break;
            }
            out[k] = rnd(r);
        }
        write(dst, out, dest);
    }

    // OPMULA / OPMSUB (xyz): the cross-product permutation.
    void outer(bool sub, Qword& dst, const Qword& S, const Qword& T) {
        static constexpr int p[3][2] = {{1, 2}, {2, 0}, {0, 1}};
        Qword out = dst;
        for (int k = 0; k < 3; ++k) {
            const double prod = daz(S[p[k][0]]) * daz(T[p[k][1]]);
            out[k] = rnd(sub ? daz(s.acc[k]) - daz(rnd(prod)) : prod);
        }
        write(dst, out, XYZ);
    }

    // MAX/MINI: sign-magnitude order of the words (exponent 255 included).
    static std::int64_t key(std::uint32_t b) {
        const std::int64_t m = b & 0x7fffffffu;
        return (b >> 31) ? -m : m;
    }

    void minmax(bool max, Qword& dst, const Qword& S, const Qword& T, int dest) {
        Qword out = dst;
        for (int k = 0; k < 4; ++k) {
            if (!(dest & (8 >> k))) continue;
            const std::int64_t a = key(S[k]), b = key(T[k]);
            if (a == b && S[k] != T[k]) fail(g_pair, "MAX/MINI of equal values with different bits");
            out[k] = (max ? a >= b : a <= b) ? S[k] : T[k];
        }
        write(dst, out, dest);
    }

    void itof0(Qword& dst, const Qword& S, int dest) {
        Qword out = dst;
        for (int k = 0; k < 4; ++k)
            if (dest & (8 >> k)) out[k] = std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int32_t>(S[k])));
        write(dst, out, dest);
    }

    void ftoi0(Qword& dst, const Qword& S, int dest) {
        Qword out = dst;
        for (int k = 0; k < 4; ++k) {
            if (!(dest & (8 >> k))) continue;
            const double x = daz(S[k]);
            std::int32_t r;
            if (x >= 2147483647.0) r = 2147483647;
            else if (x <= -2147483648.0) r = INT32_MIN;
            else r = static_cast<std::int32_t>(x);  // truncation toward zero
            out[k] = static_cast<std::uint32_t>(r);
        }
        write(dst, out, dest);
    }

    // 132vc1: the loop bodies take these through their machine (SpecMachine keeps them in NEON registers).
    static Qword bc(const Qword& v, int k) { return {v[k], v[k], v[k], v[k]}; }
    void mr32(Qword& r) {
        const Qword o = r;
        write(r, Qword{o[1], o[2], o[3], o[0]}, XYZW);
    }

    Qword& mem(std::uint32_t qa) { return s.data[qa & 0x3ff]; }
    void lq(int ft, std::uint32_t qa) { write(v(ft), mem(qa), XYZW); }
    void lqi(int ft, int vi) {
        lq(ft, s.vi[vi]);
        s.vi[vi] = static_cast<std::uint16_t>(s.vi[vi] + 1);
    }
    void sqi(int fs, int vi) {
        mem(s.vi[vi]) = v(fs);
        s.vi[vi] = static_cast<std::uint16_t>(s.vi[vi] + 1);
    }
    void mtir(int it, std::uint32_t word) { s.vi[it] = static_cast<std::uint16_t>(word); }

    void div(std::uint32_t sb, std::uint32_t tb) {
        const double t = daz(tb), x = daz(sb);
        if (t == 0) {
            const bool neg = (std::signbit(x) != std::signbit(t));
            s.q = (neg ? 0x80000000u : 0) | kVuMax;
            return;
        }
        s.q = rnd(x / t);
    }
    void rsqrt(std::uint32_t sb, std::uint32_t tb) {
        const double t = daz(tb), x = daz(sb);
        if (t == 0) fail(g_pair, "RSQRT of zero");
        s.q = rnd(x / static_cast<double>(static_cast<float>(std::sqrt(std::fabs(t)))));
    }

    // SQRT (Checkpoint 122f, program 0x170): Q = sqrt(|t|); a negative t only
    // sets the invalid flag, which the program does not read.
    void sqrt(std::uint32_t tb) {
        s.q = rnd(std::sqrt(std::fabs(daz(tb))));
    }
    void rnext() {
        const std::uint32_t x = (s.r >> 4) & 1, y = (s.r >> 22) & 1;
        s.r = ((s.r << 1) ^ x ^ y) & 0x7fffffu;
    }
    void rget(int ft, int dest) { write(v(ft), imm(0x3f800000u | s.r), dest); }
    void rnext(int ft, int dest) {
        rnext();
        rget(ft, dest);
    }
    void rxor(std::uint32_t word) { s.r = (s.r ^ word) & 0x7fffffu; }
};

#if OMEGA_PARTICLE_NEON_CHECK
// The check build, once: 2^20 random FMACs with the exponents at the edges (0, 1, the 2^0 binades, 253 to 255, the
// tiny and the overflowing results), both paths compared on every dest mask.
void neon_selftest() {
    auto vu = std::make_unique<Vu1State>();
    Machine m{*vu};
    std::uint32_t x = 0x9e3779b9u;
    auto next = [&] {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    };
    auto value = [&] {
        static constexpr std::uint32_t edges[] = {0, 1, 2, 3, 24, 25, 126, 127, 128, 150, 151, 252, 253, 254, 255};
        const std::uint32_t r = next();
        const std::uint32_t ex = (r & 3) ? edges[(r >> 2) % std::size(edges)] : (r >> 8) & 0xff;
        return (next() & 0x807fffffu) | (ex << 23);
    };
    std::uint32_t fast = 0, differences = 0;
    for (std::uint32_t i = 0; i < (1u << 20); ++i) {
        Qword S, T, d;
        for (int k = 0; k < 4; ++k) S[k] = value(), T[k] = value(), vu->acc[k] = value(), d[k] = value();
        if (next() & 1) T = S;  // a - a, a * a
        const Op op = static_cast<Op>(next() % 5);
        const int dest = int(next() % 15) + 1;
        Qword got = d, expect = d;
        if (!m.fmac_neon(op, got, S, T, dest)) continue;
        ++fast;
        try {
            m.fmac_lanes(op, expect, S, T, dest);
        } catch (const std::exception&) {
            expect = Qword{~got[0], 0, 0, 0};
        }
        if (got != expect && differences++ < 8)
            std::cerr << "VU1 particle NEON self-test: op " << int(op) << " dest " << dest << " S " << std::hex << S[0]
                      << ' ' << S[1] << ' ' << S[2] << ' ' << S[3] << " T " << T[0] << ' ' << T[1] << ' ' << T[2] << ' '
                      << T[3] << " NEON " << got[0] << ' ' << got[1] << ' ' << got[2] << ' ' << got[3] << " lanes "
                      << expect[0] << ' ' << expect[1] << ' ' << expect[2] << ' ' << expect[3] << std::dec << '\n';
    }
    std::cerr << "VU1 particle NEON self-test: " << (1u << 20) << " random FMACs, " << fast << " on NEON, " << differences
              << " differences\n";
}
#endif


// Port-side speed only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"; the same bits): the PS
// Vita runs each particle's iteration of a program loop (and each sprite of a sprite program, and the batch cull)
// speculatively on NEON without deciding every FMAC on its own: SpecMachine computes every FMAC as fmac_neon does,
// writes it, and only ORs the lanes fmac_neon would have refused (an operand, the ACC of MADD / MSUB or a result with
// exponent 255, in a dest lane) into `bad`; MAX / MINI, FTOI0, ITOF0, OPMULA / OPMSUB, MR32 and the moves also stay
// in NEON registers, each flagging the inputs where its exact port could differ (see each). After the iteration one
// test of `bad` decides: clean, every instruction's result is the one the exact path (Machine: fmac_neon, else the
// lanes) gives, so the state is that path's; otherwise (or on any throw, e.g. a garbage RSQRT operand after a refused
// lane) the registers, the two output words and the sprite output are put back as they were before the iteration
// and the iteration runs again on Machine, the old path, which gives the result (or the error) it always gave. This
// removes the NEON-to-core transfer and branch fmac_neon pays on every FMAC (about 100 a particle) and the core reads
// of broadcast lanes (bc), which on the Cortex-A9 stall until the NEON pipeline has written them.
struct Bc {
    const Qword* v;
    int k;
};

std::atomic<std::uint64_t> g_neon_rollbacks{0};
std::atomic<bool> g_speculate{true};  // set_particle_neon_speculation (an A/B switch of the Vita app)

#if OMEGA_PARTICLE_NEON
struct SpecMachine : Machine {
    uint32x4_t bad = vdupq_n_u32(0);

    static Bc bc(const Qword& v, int k) { return {&v, k}; }
    static OMEGA_PARTICLE_INLINE uint32x4_t load(const Qword& q) { return vld1q_u32(q.data()); }
    static OMEGA_PARTICLE_INLINE uint32x4_t load(const Bc& b) { return vld1q_dup_u32(b.v->data() + b.k); }
    static OMEGA_PARTICLE_INLINE uint32x4_t lanes(int dest) {
        return uint32x4_t{(dest & X) ? ~0u : 0u, (dest & Y) ? ~0u : 0u, (dest & Z) ? ~0u : 0u, (dest & W) ? ~0u : 0u};
    }
    static OMEGA_PARTICLE_INLINE uint32x4_t e255(uint32x4_t b) {
        const uint32x4_t e = vdupq_n_u32(0x7f800000u);
        return vceqq_u32(vandq_u32(b, e), e);
    }
    static OMEGA_PARTICLE_INLINE uint32x4_t e0(uint32x4_t b) {
        return vceqq_u32(vandq_u32(b, vdupq_n_u32(0x7f800000u)), vdupq_n_u32(0));
    }
    OMEGA_PARTICLE_INLINE void flag(uint32x4_t lanes_bad, uint32x4_t mask) { bad = vorrq_u32(bad, vandq_u32(lanes_bad, mask)); }
    OMEGA_PARTICLE_INLINE void store(Qword& dst, uint32x4_t r, uint32x4_t mask, int dest) {
        if (&dst == &s.vf[0]) return;
        vst1q_u32(dst.data(), dest == XYZW ? r : vbslq_u32(mask, r, vld1q_u32(dst.data())));
    }
    bool clean() const {
        const uint32x2_t h = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
        return vget_lane_u32(vpmax_u32(h, h), 0) == 0;
    }

    // fmac_neon's arithmetic; its refusal test only accumulated.
    template <class TS, class TT>
    OMEGA_PARTICLE_INLINE void fmac(Op op, Qword& dst, const TS& S, const TT& T, int dest) {
        const uint32x4_t mask = lanes(dest);
        const uint32x4_t sb = load(S), tb = load(T);
        uint32x4_t b = vorrq_u32(e255(sb), e255(tb));
        const float32x4_t a = vreinterpretq_f32_u32(sb), c = vreinterpretq_f32_u32(tb);
        float32x4_t r;
        switch (op) {
        case Op::add: r = vaddq_f32(a, c); break;
        case Op::sub: r = vsubq_f32(a, c); break;
        case Op::mul: r = vmulq_f32(a, c); break;
        default: {  // MADD / MSUB: the product rounded first
            const uint32x4_t accb = vld1q_u32(s.acc.data());
            b = vorrq_u32(b, e255(accb));
            const float32x4_t p = vmulq_f32(a, c), d = vreinterpretq_f32_u32(accb);
            r = op == Op::madd ? vaddq_f32(d, p) : vsubq_f32(d, p);
            break;
        }
        }
        const uint32x4_t rb = vreinterpretq_u32_f32(r);
        flag(vorrq_u32(b, e255(rb)), mask);
        store(dst, rb, mask, dest);
    }

    // OPMULA / OPMSUB on (S.y, S.z, S.x) and (T.z, T.x, T.y). The exact port rounds the double product (rnd); the
    // binary32 product is that value unless an operand or the result has exponent 255 (flagged) or it is tiny: a
    // product of two non-zero operands that comes out with a zero exponent is flagged too (Advanced SIMD flushes a
    // result tiny before rounding, rnd rounds first). OPMSUB's ACC - product is then fast_lane's MSUB (a difference
    // of binary32 values tiny enough to flush is exact, so both give its signed zero).
    OMEGA_PARTICLE_INLINE void outer(bool sub, Qword& dst, const Qword& S, const Qword& T) {
        const uint32x4_t lane0 = {~0u, 0u, 0u, 0u}, lane2 = {0u, 0u, ~0u, 0u};
        const uint32x4_t sb = load(S), tb = load(T);
        const uint32x4_t s1 = vbslq_u32(lane2, vextq_u32(sb, sb, 2), vextq_u32(sb, sb, 1));
        const uint32x4_t t1 = vbslq_u32(lane0, vextq_u32(tb, tb, 2), vextq_u32(tb, tb, 3));
        const uint32x4_t mask = lanes(XYZ);
        uint32x4_t b = vorrq_u32(e255(s1), e255(t1));
        const float32x4_t p = vmulq_f32(vreinterpretq_f32_u32(s1), vreinterpretq_f32_u32(t1));
        const uint32x4_t pb = vreinterpretq_u32_f32(p);
        b = vorrq_u32(b, vbicq_u32(e0(pb), vorrq_u32(e0(s1), e0(t1))));
        uint32x4_t rb = pb;
        if (sub) {
            const uint32x4_t accb = vld1q_u32(s.acc.data());
            b = vorrq_u32(b, e255(accb));
            rb = vreinterpretq_u32_f32(vsubq_f32(vreinterpretq_f32_u32(accb), p));
        }
        flag(vorrq_u32(b, e255(rb)), mask);
        store(dst, rb, mask, XYZ);
    }

    // MAX / MINI: the words' sign-magnitude keys (int32: -magnitude for a set sign); equal keys with different bits
    // (+0 / -0), which the exact port throws for, are flagged.
    template <class TT>
    OMEGA_PARTICLE_INLINE void minmax(bool max, Qword& dst, const Qword& S, const TT& T, int dest) {
        const uint32x4_t mask = lanes(dest);
        const uint32x4_t sb = load(S), tb = load(T);
        const uint32x4_t sign = vdupq_n_u32(0x80000000u), mag = vdupq_n_u32(0x7fffffffu);
        const int32x4_t ms = vreinterpretq_s32_u32(vandq_u32(sb, mag)), mt = vreinterpretq_s32_u32(vandq_u32(tb, mag));
        const int32x4_t ks = vbslq_s32(vtstq_u32(sb, sign), vnegq_s32(ms), ms);
        const int32x4_t kt = vbslq_s32(vtstq_u32(tb, sign), vnegq_s32(mt), mt);
        const uint32x4_t pick = max ? vcgeq_s32(ks, kt) : vcleq_s32(ks, kt);
        flag(vandq_u32(vceqq_s32(ks, kt), vmvnq_u32(vceqq_u32(sb, tb))), mask);
        store(dst, vbslq_u32(pick, sb, tb), mask, dest);
    }

    // FTOI0: truncation with saturation, a zero exponent read as zero (Advanced SIMD flushes denormal inputs);
    // exponent 255 (a 2^128 binade value for the VU, an infinity or NaN for NEON) is flagged.
    OMEGA_PARTICLE_INLINE void ftoi0(Qword& dst, const Qword& S, int dest) {
        const uint32x4_t mask = lanes(dest), sb = load(S);
        flag(e255(sb), mask);
        store(dst, vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(sb))), mask, dest);
    }
    // ITOF0: round to nearest, as the VFP conversion under the thread's FPSCR (nearest whenever NEON is used).
    OMEGA_PARTICLE_INLINE void itof0(Qword& dst, const Qword& S, int dest) {
        store(dst, vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(load(S)))), lanes(dest), dest);
    }
    OMEGA_PARTICLE_INLINE void write(Qword& dst, const Qword& out, int dest) { store(dst, load(out), lanes(dest), dest); }
    OMEGA_PARTICLE_INLINE void mr32(Qword& r) {
        const uint32x4_t v = load(r);
        store(r, vextq_u32(v, v, 1), lanes(XYZW), XYZW);
    }
};

// The registers before an iteration.
struct IterationSave {
    std::array<Qword, 32> vf;
    Qword acc;
    std::uint32_t i, q, r;
    std::array<std::uint16_t, 16> vi;
    explicit IterationSave(const Vu1State& s) : vf(s.vf), acc(s.acc), i(s.i), q(s.q), r(s.r), vi(s.vi) {}
    void restore(Vu1State& s) const {
        s.vf = vf;
        s.acc = acc;
        s.i = i;
        s.q = q;
        s.r = r;
        s.vi = vi;
    }
    bool operator==(const IterationSave& o) const {
        return vf == o.vf && acc == o.acc && i == o.i && q == o.q && r == o.r && vi == o.vi;
    }
};
#endif

#if OMEGA_PARTICLE_NEON_CHECK
std::atomic<std::uint64_t> g_spec_checked{0};
[[noreturn]] void spec_mismatch(const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "VU1 particle speculative NEON check: %s differs from the exact path (pair %03x)", what,
                  g_pair);
    throw std::runtime_error(b);
}
#endif

// One iteration `body(machine)` (returning the loop's branch), speculatively on the Vita. `outputs`: the iteration
// writes its two output words through SQI vi3 / vi4 (the particle programs); `out`: it appends to this vector (the
// sprite programs).
template <class Body>
OMEGA_PARTICLE_INLINE bool iterate(Machine& m, const Body& body, bool outputs, std::vector<Qword>* out = nullptr) {
#if OMEGA_PARTICLE_NEON
    if (m.neon && g_speculate.load(std::memory_order_relaxed)) {
        Vu1State& s = m.s;
        const IterationSave save(s);
        const std::uint32_t a3 = s.vi[3] & 0x3ffu, a4 = s.vi[4] & 0x3ffu;
        const Qword o3 = s.data[a3], o4 = s.data[a4];
        const std::size_t n = out ? out->size() : 0;
        SpecMachine x{{s, true}};
        bool again = false, ok = false;
        try {
            again = body(x);
            ok = x.clean();
        } catch (...) {
            ok = false;
        }
#if OMEGA_PARTICLE_NEON_CHECK
        // The check build: the exact iteration after every speculative one, compared when the latter was clean.
        const IterationSave spec(s);
        const Qword s3 = s.data[a3], s4 = s.data[a4];
        std::vector<Qword> spec_out;
        if (out) spec_out.assign(out->begin() + static_cast<std::ptrdiff_t>(n), out->end());
        const bool spec_again = again;
        if (outputs) s.data[a4] = o4, s.data[a3] = o3;
        if (out) out->resize(n);
        save.restore(s);
        again = body(m);
        if (ok) {
            if (!(spec == IterationSave(s))) spec_mismatch("a register");
            if (outputs && (s3 != s.data[a3] || s4 != s.data[a4])) spec_mismatch("an output word");
            if (out && !std::equal(spec_out.begin(), spec_out.end(), out->begin() + static_cast<std::ptrdiff_t>(n), out->end()))
                spec_mismatch("a sprite word");
            if (spec_again != again) spec_mismatch("the loop branch");
        } else {
            g_neon_rollbacks.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t checked = g_spec_checked.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((checked & 0xffffu) == 0)
            std::cerr << "VU1 particle speculative NEON check: " << checked << " iterations compared, "
                      << g_neon_rollbacks.load() << " rolled back, 0 differences\n";
        return again;
#else
        if (ok) return again;
        if (outputs) s.data[a4] = o4, s.data[a3] = o3;
        if (out) out->resize(n);
        save.restore(s);
        g_neon_rollbacks.fetch_add(1, std::memory_order_relaxed);
#endif
    }
#else
    (void)outputs;
    (void)out;
#endif
    return body(m);
}


#if OMEGA_PARTICLE_NEON
// Port-side speed only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"; the same bits): the
// particle programs' loop iterations with the VU registers held in NEON registers for the whole iteration
// (iteration_regs_*, generated from the loop bodies below). Each operation is SpecMachine's (the same arithmetic and
// the same refusal conditions, accumulated in `bad`); DIV / RSQRT / SQRT, the random register and the integer
// registers are the exact port's on lane copies; the two SQI outputs are written last. A refusal (a refusal lane,
// RSQRT of zero, the LQ range check) leaves the state untouched and the iteration runs on iterate() as before.
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_mask() {
    return uint32x4_t{(dest & X) ? ~0u : 0u, (dest & Y) ? ~0u : 0u, (dest & Z) ? ~0u : 0u, (dest & W) ? ~0u : 0u};
}
OMEGA_PARTICLE_INLINE uint32x4_t rx_e255(uint32x4_t b) {
    const uint32x4_t e = vdupq_n_u32(0x7f800000u);
    return vceqq_u32(vandq_u32(b, e), e);
}
OMEGA_PARTICLE_INLINE uint32x4_t rx_e0(uint32x4_t b) {
    return vceqq_u32(vandq_u32(b, vdupq_n_u32(0x7f800000u)), vdupq_n_u32(0));
}
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_merge(uint32x4_t d, uint32x4_t r) {
    if constexpr (dest == XYZW) return r;
    else return vbslq_u32(rx_mask<dest>(), r, d);
}
OMEGA_PARTICLE_INLINE bool rx_clean(uint32x4_t bad) {
    const uint32x2_t h = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
    return vget_lane_u32(vpmax_u32(h, h), 0) == 0;
}
template <int k> OMEGA_PARTICLE_INLINE uint32x4_t rx_bc(uint32x4_t v) {
    if constexpr (k < 2) return vdupq_lane_u32(vget_low_u32(v), k);
    else return vdupq_lane_u32(vget_high_u32(v), k - 2);
}
template <Op op, int dest>
OMEGA_PARTICLE_INLINE uint32x4_t rx_fmac(uint32x4_t d, uint32x4_t sb, uint32x4_t tb, uint32x4_t accb, uint32x4_t& bad) {
    uint32x4_t b = vorrq_u32(rx_e255(sb), rx_e255(tb));
    const float32x4_t a = vreinterpretq_f32_u32(sb), c = vreinterpretq_f32_u32(tb);
    float32x4_t r;
    if constexpr (op == Op::add) r = vaddq_f32(a, c);
    else if constexpr (op == Op::sub) r = vsubq_f32(a, c);
    else if constexpr (op == Op::mul) r = vmulq_f32(a, c);
    else {
        b = vorrq_u32(b, rx_e255(accb));
        const float32x4_t p = vmulq_f32(a, c), e = vreinterpretq_f32_u32(accb);
        r = op == Op::madd ? vaddq_f32(e, p) : vsubq_f32(e, p);
    }
    const uint32x4_t rb = vreinterpretq_u32_f32(r);
    bad = vorrq_u32(bad, vandq_u32(vorrq_u32(b, rx_e255(rb)), rx_mask<dest>()));
    return rx_merge<dest>(d, rb);
}
template <bool sub>
OMEGA_PARTICLE_INLINE uint32x4_t rx_outer(uint32x4_t d, uint32x4_t sb, uint32x4_t tb, uint32x4_t accb, uint32x4_t& bad) {
    const uint32x4_t lane0 = {~0u, 0u, 0u, 0u}, lane2 = {0u, 0u, ~0u, 0u};
    const uint32x4_t s1 = vbslq_u32(lane2, vextq_u32(sb, sb, 2), vextq_u32(sb, sb, 1));
    const uint32x4_t t1 = vbslq_u32(lane0, vextq_u32(tb, tb, 2), vextq_u32(tb, tb, 3));
    uint32x4_t b = vorrq_u32(rx_e255(s1), rx_e255(t1));
    const float32x4_t p = vmulq_f32(vreinterpretq_f32_u32(s1), vreinterpretq_f32_u32(t1));
    const uint32x4_t pb = vreinterpretq_u32_f32(p);
    b = vorrq_u32(b, vbicq_u32(rx_e0(pb), vorrq_u32(rx_e0(s1), rx_e0(t1))));
    uint32x4_t rb = pb;
    if constexpr (sub) {
        b = vorrq_u32(b, rx_e255(accb));
        rb = vreinterpretq_u32_f32(vsubq_f32(vreinterpretq_f32_u32(accb), p));
    }
    bad = vorrq_u32(bad, vandq_u32(vorrq_u32(b, rx_e255(rb)), rx_mask<XYZ>()));
    return rx_merge<XYZ>(d, rb);
}
template <bool max, int dest>
OMEGA_PARTICLE_INLINE uint32x4_t rx_minmax(uint32x4_t d, uint32x4_t sb, uint32x4_t tb, uint32x4_t& bad) {
    const uint32x4_t sign = vdupq_n_u32(0x80000000u), mag = vdupq_n_u32(0x7fffffffu);
    const int32x4_t ms = vreinterpretq_s32_u32(vandq_u32(sb, mag)), mt = vreinterpretq_s32_u32(vandq_u32(tb, mag));
    const int32x4_t ks = vbslq_s32(vtstq_u32(sb, sign), vnegq_s32(ms), ms);
    const int32x4_t kt = vbslq_s32(vtstq_u32(tb, sign), vnegq_s32(mt), mt);
    const uint32x4_t pick = max ? vcgeq_s32(ks, kt) : vcleq_s32(ks, kt);
    bad = vorrq_u32(bad, vandq_u32(vandq_u32(vceqq_s32(ks, kt), vmvnq_u32(vceqq_u32(sb, tb))), rx_mask<dest>()));
    return rx_merge<dest>(d, vbslq_u32(pick, sb, tb));
}
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_ftoi0(uint32x4_t d, uint32x4_t sb, uint32x4_t& bad) {
    bad = vorrq_u32(bad, vandq_u32(rx_e255(sb), rx_mask<dest>()));
    return rx_merge<dest>(d, vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(sb))));
}
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_itof0(uint32x4_t d, uint32x4_t sb) {
    return rx_merge<dest>(d, vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(sb))));
}
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_write(uint32x4_t d, uint32x4_t src) { return rx_merge<dest>(d, src); }
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_rget(uint32x4_t d, std::uint32_t r) {
    return rx_merge<dest>(d, vdupq_n_u32(0x3f800000u | r));
}
OMEGA_PARTICLE_INLINE void rx_rnext(std::uint32_t& r) {
    const std::uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
    r = ((r << 1) ^ x ^ y) & 0x7fffffu;
}
// Machine::div / rsqrt / sqrt (rsqrt of zero: false, the exact port throws).
std::uint32_t rx_div(std::uint32_t sb, std::uint32_t tb) {
    const double t = daz(tb), x = daz(sb);
    if (t == 0) return ((std::signbit(x) != std::signbit(t)) ? 0x80000000u : 0) | kVuMax;
    return rnd(x / t);
}
bool rx_rsqrt(std::uint32_t& q, std::uint32_t sb, std::uint32_t tb) {
    const double t = daz(tb), x = daz(sb);
    if (t == 0) return false;
    q = rnd(x / static_cast<double>(static_cast<float>(std::sqrt(std::fabs(t)))));
    return true;
}
std::uint32_t rx_sqrt(std::uint32_t tb) { return rnd(std::sqrt(std::fabs(daz(tb)))); }


// 132vc1 hw5 (port-side speed only; the same refusals): a value's exponent-255 test accumulated into `hi` as an
// unsigned max of (bits & 0x7fffffff) over the tested lanes; the iteration is refused when the max reaches
// 0x7f800000 (rx_clean of the final compare). FMAC as rx_fmac with only its result tested here (the generator tests
// each operand lane once, before its first use); FTOI0 as rx_ftoi0 with its input tested by the generator.
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_abs_mask() {
    return uint32x4_t{(dest & X) ? 0x7fffffffu : 0u, (dest & Y) ? 0x7fffffffu : 0u, (dest & Z) ? 0x7fffffffu : 0u,
                      (dest & W) ? 0x7fffffffu : 0u};
}
template <int lanes> OMEGA_PARTICLE_INLINE void rx_test(uint32x4_t v, uint32x4_t& hi) {
    hi = vmaxq_u32(hi, vandq_u32(v, rx_abs_mask<lanes>()));
}
template <Op op, int dest>
OMEGA_PARTICLE_INLINE uint32x4_t rx_fmac_t(uint32x4_t d, uint32x4_t sb, uint32x4_t tb, uint32x4_t accb, uint32x4_t& hi) {
    const float32x4_t a = vreinterpretq_f32_u32(sb), c = vreinterpretq_f32_u32(tb);
    float32x4_t r;
    if constexpr (op == Op::add) r = vaddq_f32(a, c);
    else if constexpr (op == Op::sub) r = vsubq_f32(a, c);
    else if constexpr (op == Op::mul) r = vmulq_f32(a, c);
    else {
        const float32x4_t p = vmulq_f32(a, c), e = vreinterpretq_f32_u32(accb);
        r = op == Op::madd ? vaddq_f32(e, p) : vsubq_f32(e, p);
    }
    const uint32x4_t rb = vreinterpretq_u32_f32(r);
    rx_test<dest>(rb, hi);
    return rx_merge<dest>(d, rb);
}
template <int dest> OMEGA_PARTICLE_INLINE uint32x4_t rx_ftoi0_t(uint32x4_t d, uint32x4_t sb) {
    return rx_merge<dest>(d, vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(sb))));
}

std::atomic<bool> g_regs{true};  // set_particle_neon_registers (132vc1)

// One loop iteration: the register-resident one when it is taken, else iterate() (speculative / exact). The check
// build compares the register-resident result with iterate()'s (itself checked against the exact path).
template <class Body>
OMEGA_PARTICLE_INLINE bool step(Machine& m, int (*regs)(Vu1State&), const Body& body) {
    if (m.neon && g_speculate.load(std::memory_order_relaxed) && g_regs.load(std::memory_order_relaxed)) {
        Vu1State& s = m.s;
#if OMEGA_PARTICLE_NEON_CHECK
        const IterationSave before(s);
        const std::uint32_t a3 = s.vi[3] & 0x3ffu, a4 = s.vi[4] & 0x3ffu;
        const Qword o3 = s.data[a3], o4 = s.data[a4];
        const int got = regs(s);
        const IterationSave after(s);
        const Qword r3 = s.data[a3], r4 = s.data[a4];
        s.data[a4] = o4, s.data[a3] = o3;
        before.restore(s);
        const bool again = iterate(m, body, true);
        if (got >= 0) {
            if (!(after == IterationSave(s)) || r3 != s.data[a3] || r4 != s.data[a4] || (got != 0) != again)
                spec_mismatch("a register-resident iteration");
        } else {
            g_neon_rollbacks.fetch_add(1, std::memory_order_relaxed);
        }
        return again;
#else
        const int got = regs(s);
        if (got >= 0) return got != 0;
        g_neon_rollbacks.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    return iterate(m, body, true);
}
#endif
#if OMEGA_PARTICLE_NEON
// Generated (132vc1 hw5, tools: local regs_gen2.py) from the loop body of program 0x29c above: the iteration with
// the VU registers in NEON registers, each value tested once. -1: refused (the state is untouched); else the loop
// branch, the state written.
OMEGA_PARTICLE_NOINLINE int iteration_regs_29c(Vu1State& s) {
    const uint32x4_t vf0 = {0u, 0u, 0u, 0x3f800000u};
    uint32x4_t acc = vld1q_u32(s.acc.data());
    std::uint32_t i = s.i, q = s.q, r = s.r;
    std::array<std::uint16_t, 16> vi = s.vi;
    uint32x4_t bad = vdupq_n_u32(0), hi = vdupq_n_u32(0);
    bool refused = false;
    std::uint32_t out_a[2];
    uint32x4_t out_v[2];
    int outs = 0;
    (void)vf0;
    uint32x4_t vf4 = vld1q_u32(s.vf[4].data());
    uint32x4_t vf21 = vld1q_u32(s.vf[21].data());
    q = rx_div(vgetq_lane_u32(vf4, 3), vgetq_lane_u32(vf21, 0));
    uint32x4_t vf28 = vld1q_u32(s.vf[28].data());
    vf28 = rx_fmac_t<Op::mul, 0x6>(vf28, vf28, vf4, acc, hi);
    uint32x4_t vf25 = vld1q_u32(s.vf[25].data());
    vf25 = rx_write<(0x1)>(vf25, vf28);
    uint32x4_t vf2 = vld1q_u32(s.vf[2].data());
    uint32x4_t vf9 = vld1q_u32(s.vf[9].data());
    acc = rx_outer<false>(acc, vf2, vf9, acc, bad);
    vi[5] = static_cast<std::uint16_t>(vgetq_lane_u32(vf2, 3));
    vf9 = rx_outer<true>(vf9, vf9, vf2, acc, bad);
    uint32x4_t vf20 = vld1q_u32(s.vf[20].data());
    vi[6] = static_cast<std::uint16_t>(vgetq_lane_u32(vf20, 2));
    uint32x4_t vf5 = vld1q_u32(s.vf[5].data());
    uint32x4_t vf29 = vld1q_u32(s.vf[29].data());
    vf21 = rx_fmac_t<Op::mul, 0x3>(vf21, vf29, vf5, acc, hi);
    vi[2] = static_cast<std::uint16_t>(vi[2] - 1);
    acc = rx_fmac_t<Op::add, 0xc>(acc, vf0, rx_bc<0>(vf4), acc, hi);
    uint32x4_t vf31 = vld1q_u32(s.vf[31].data());
    acc = rx_fmac_t<Op::msub, 0xc>(acc, vf31, rx_bc<2>(vf28), acc, hi);
    uint32x4_t vf12 = vld1q_u32(s.vf[12].data());
    vf12 = rx_fmac_t<Op::madd, 0xc>(vf12, vf31, vdupq_n_u32(q), acc, hi);
    uint32x4_t vf26 = vld1q_u32(s.vf[26].data());
    vf26 = rx_fmac_t<Op::mul, 0xe>(vf26, vf9, vf9, acc, hi);
    i = 0x40a00000u;
    uint32x4_t vf27 = vld1q_u32(s.vf[27].data());
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf27, vdupq_n_u32(i), acc, hi);
    vf27 = rx_fmac_t<Op::madd, 0xf>(vf27, vf31, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf22 = vld1q_u32(s.vf[22].data());
    vf22 = rx_ftoi0_t<0xc>(vf22, vf12);
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf26, rx_bc<1>(vf26), acc, hi);
    vf26 = rx_fmac_t<Op::madd, 0x8>(vf26, vf31, rx_bc<2>(vf26), acc, hi);
    vf22 = rx_itof0<(0xc)>(vf22, vf22);
    i = 0x40400000u;
    acc = rx_fmac_t<Op::mul, 0xc>(acc, vf12, vdupq_n_u32(i), acc, hi);
    vf27 = vextq_u32(vf27, vf27, 1);
    vf29 = rx_fmac_t<Op::sub, 0x4>(vf29, vf29, rx_bc<3>(vf0), acc, hi);
    if (!rx_rsqrt(q, vgetq_lane_u32(vf31, 0), vgetq_lane_u32(vf26, 0))) refused = true;
    uint32x4_t vf8 = vld1q_u32(s.vf[8].data());
    uint32x4_t vf18 = vld1q_u32(s.vf[18].data());
    vf18 = rx_fmac_t<Op::add, 0xe>(vf18, vf0, rx_bc<1>(vf8), acc, hi);
    vf25 = rx_fmac_t<Op::msub, 0xc>(vf25, vf22, vdupq_n_u32(i), acc, hi);
    vf12 = rx_fmac_t<Op::sub, 0xc>(vf12, vf12, vf22, acc, hi);
    uint32x4_t vf11 = vld1q_u32(s.vf[11].data());
    vf9 = rx_fmac_t<Op::mul, 0x1>(vf9, vf11, rx_bc<1>(vf29), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf18, rx_bc<0>(vf8), acc, hi);
    vf25 = rx_ftoi0_t<0x4>(vf25, vf25);
    vf18 = rx_fmac_t<Op::madd, 0xe>(vf18, vf18, rx_bc<0>(vf12), acc, hi);
    vf12 = rx_fmac_t<Op::sub, 0x6>(vf12, vf31, rx_bc<1>(vf28), acc, hi);
    vi[9] = static_cast<std::uint16_t>(vgetq_lane_u32(vf25, 1));
    vf25 = rx_itof0<(0x4)>(vf25, vf25);
    vf18 = rx_fmac_t<Op::mul, 0xc>(vf18, vf18, rx_bc<2>(vf18), acc, hi);
    vf9 = rx_fmac_t<Op::mul, 0xe>(vf9, vf9, vdupq_n_u32(q), acc, hi);
    i = 0x38d1b717u;
    vf25 = rx_fmac_t<Op::sub, 0x8>(vf25, vf25, rx_bc<1>(vf25), acc, hi);
    const std::uint32_t qa = (0x1eu + vi[9]) & 0x3ff;
    if (qa < 0x16 || qa > 0x20) refused = true;
    vf26 = vld1q_u32(s.data[(qa) & 0x3ffu].data());
    vf18 = rx_fmac_t<Op::mul, 0x8>(vf18, vf18, rx_bc<2>(vf18), acc, hi);
    q = rx_div(vgetq_lane_u32(vf0, 3), vgetq_lane_u32(vf12, 1));
    uint32x4_t vf19 = vld1q_u32(s.vf[19].data());
    vf22 = rx_fmac_t<Op::sub, 0x1>(vf22, vf19, rx_bc<0>(vf22), acc, hi);
    vf12 = rx_minmax<false, (0x8)>(vf12, vf12, rx_bc<3>(vf22), bad);
    uint32x4_t vf1 = vld1q_u32(s.vf[1].data());
    vf9 = rx_fmac_t<Op::mul, 0x1>(vf9, vf9, vf1, acc, hi);
    vf12 = rx_minmax<true, (0x8)>(vf12, vf12, vdupq_n_u32(i), bad);
    r = (r ^ vgetq_lane_u32(vf27, 0)) & 0x7fffffu;
    vf27 = rx_rget<(0x8)>(vf27, r);
    acc = rx_fmac_t<Op::mul, 0x4>(acc, vf31, rx_bc<0>(vf26), acc, hi);
    rx_rnext(r);
    vf29 = rx_rget<(0x8)>(vf29, r);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf31, rx_bc<2>(vf26), acc, hi);
    rx_rnext(r);
    vf28 = rx_rget<(0x8)>(vf28, r);
    vf26 = rx_fmac_t<Op::madd, 0x5>(vf26, vf26, rx_bc<3>(vf25), acc, hi);
    rx_rnext(r);
    uint32x4_t vf30 = vld1q_u32(s.vf[30].data());
    vf30 = rx_rget<(0x8)>(vf30, r);
    vf12 = rx_minmax<false, (0x8)>(vf12, vf12, rx_bc<1>(vf12), bad);
    r = (r ^ vgetq_lane_u32(vf27, 1)) & 0x7fffffu;
    uint32x4_t vf15 = vld1q_u32(s.vf[15].data());
    vf22 = rx_fmac_t<Op::mul, 0xf>(vf22, vf18, vf15, acc, hi);
    vf29 = rx_rget<(0x4)>(vf29, r);
    uint32x4_t vf16 = vld1q_u32(s.vf[16].data());
    uint32x4_t vf23 = vld1q_u32(s.vf[23].data());
    vf23 = rx_fmac_t<Op::mul, 0xf>(vf23, vf18, vf16, acc, hi);
    rx_rnext(r);
    vf28 = rx_rget<(0x4)>(vf28, r);
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf0, rx_bc<1>(vf26), acc, hi);
    rx_rnext(r);
    vf27 = rx_rget<(0x4)>(vf27, r);
    acc = rx_fmac_t<Op::msub, 0x8>(acc, vf25, rx_bc<1>(vf26), acc, hi);
    rx_rnext(r);
    vf30 = rx_rget<(0x4)>(vf30, r);
    vf26 = rx_fmac_t<Op::madd, 0x8>(vf26, vf25, rx_bc<3>(vf26), acc, hi);
    r = (r ^ vgetq_lane_u32(vf27, 2)) & 0x7fffffu;
    vf12 = rx_fmac_t<Op::add, 0x2>(vf12, vf0, rx_bc<0>(vf12), acc, hi);
    vf28 = rx_rget<(0x2)>(vf28, r);
    uint32x4_t vf17 = vld1q_u32(s.vf[17].data());
    uint32x4_t vf24 = vld1q_u32(s.vf[24].data());
    vf24 = rx_fmac_t<Op::mul, 0xf>(vf24, vf18, vf17, acc, hi);
    rx_rnext(r);
    vf27 = rx_rget<(0x2)>(vf27, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x2)>(vf29, r);
    vf9 = rx_fmac_t<Op::mul, 0xe>(vf9, vf9, rx_bc<0>(vf26), acc, hi);
    rx_rnext(r);
    vf30 = rx_rget<(0x2)>(vf30, r);
    vf12 = rx_fmac_t<Op::mul, 0xa>(vf12, vf12, vdupq_n_u32(q), acc, hi);
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf22, rx_bc<1>(vf22), acc, hi);
    r = (r ^ vgetq_lane_u32(vf27, 3)) & 0x7fffffu;
    acc = rx_fmac_t<Op::madd, 0x8>(acc, vf31, rx_bc<2>(vf22), acc, hi);
    vf27 = rx_rget<(0x1)>(vf27, r);
    uint32x4_t vf10 = vld1q_u32(s.vf[10].data());
    vf10 = rx_fmac_t<Op::madd, 0x8>(vf10, vf31, rx_bc<3>(vf22), acc, hi);
    rx_rnext(r);
    vf29 = rx_rget<(0x1)>(vf29, r);
    vf12 = rx_fmac_t<Op::sub, 0x5>(vf12, vf31, rx_bc<0>(vf12), acc, hi);
    rx_rnext(r);
    vf28 = rx_rget<(0x1)>(vf28, r);
    uint32x4_t vf7 = vld1q_u32(s.vf[7].data());
    vf18 = rx_fmac_t<Op::mul, 0xf>(vf18, vf7, vf12, acc, hi);
    acc = rx_fmac_t<Op::add, 0x4>(acc, vf23, rx_bc<0>(vf23), acc, hi);
    acc = rx_fmac_t<Op::madd, 0x4>(acc, vf31, rx_bc<2>(vf23), acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x4>(vf10, vf31, rx_bc<3>(vf23), acc, hi);
    uint32x4_t vf6 = vld1q_u32(s.vf[6].data());
    uint32x4_t vf14 = vld1q_u32(s.vf[14].data());
    vf14 = rx_fmac_t<Op::mul, 0xc>(vf14, vf6, vf12, acc, hi);
    vf18 = rx_minmax<false, (0xf)>(vf18, vf18, rx_bc<3>(vf0), bad);
    rx_rnext(r);
    vf30 = rx_rget<(0x1)>(vf30, r);
    acc = rx_fmac_t<Op::add, 0x2>(acc, vf24, rx_bc<0>(vf24), acc, hi);
    acc = rx_fmac_t<Op::madd, 0x2>(acc, vf31, rx_bc<1>(vf24), acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x2>(vf10, vf31, rx_bc<3>(vf24), acc, hi);
    vf26 = vld1q_u32(s.data[(15) & 0x3ffu].data());
    acc = rx_fmac_t<Op::mul, 0x3>(acc, vf6, rx_bc<3>(vf0), acc, hi);
    vf22 = rx_fmac_t<Op::msub, 0x3>(vf22, vf6, vf18, acc, hi);
    uint32x4_t vf3 = vld1q_u32(s.vf[3].data());
    vf14 = rx_minmax<false, (0x1)>(vf14, vf3, rx_bc<0>(vf14), bad);
    acc = rx_fmac_t<Op::add, 0xe>(acc, vf10, vf26, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0xe>(vf10, vf31, vf9, acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf3, rx_bc<0>(vf18), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf11, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf13 = vld1q_u32(s.vf[13].data());
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf11, rx_bc<0>(vf18), acc, hi);
    vf14 = rx_fmac_t<Op::mul, 0x4>(vf14, vf14, rx_bc<3>(vf14), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x2>(acc, vf22, rx_bc<3>(vf0), acc, hi);
    vf22 = rx_fmac_t<Op::madd, 0x2>(vf22, vf18, rx_bc<3>(vf26), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<1>(vf18), acc, hi);
    vf14 = rx_minmax<false, (0x1)>(vf14, vf14, rx_bc<1>(vf14), bad);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf19, rx_bc<3>(vf0), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf19, rx_bc<1>(vf18), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf22, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf18, rx_bc<2>(vf22), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, rx_bc<2>(vf21), acc, hi);
    vf14 = rx_fmac_t<Op::madd, 0x1>(vf14, vf14, vf0, acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<3>(vf1), acc, hi);
    acc = rx_fmac_t<Op::msub, 0xe>(acc, vf13, rx_bc<3>(vf9), acc, hi);
    vf13 = rx_fmac_t<Op::madd, 0xe>(vf13, vf1, rx_bc<3>(vf0), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf21, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf10, vf0, acc, hi);
    i = 0x437f0000u;
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, vdupq_n_u32(i), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0x1>(vf13, vf14, rx_bc<2>(vf8), acc, hi);
    i = 0x3fc00000u;
    i = 0x3b7f9724u;
    vf9 = rx_fmac_t<Op::mul, 0x1>(vf9, vf1, vdupq_n_u32(i), acc, hi);
    vf13 = rx_fmac_t<Op::mul, 0x1>(vf13, vf13, rx_bc<3>(vf9), acc, hi);
    i = 0x3fc00000u;
    vf29 = rx_fmac_t<Op::sub, 0xb>(vf29, vf29, vdupq_n_u32(i), acc, hi);
    vf13 = rx_minmax<true, (0x1)>(vf13, vf13, rx_bc<0>(vf0), bad);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::msub, 0x1>(vf10, vf10, vf8, acc, hi);
    acc = rx_fmac_t<Op::mul, 0x8>(acc, vf29, rx_bc<1>(vf5), acc, hi);
    vf13 = rx_ftoi0_t<0xf>(vf13, vf13);
    vf10 = rx_fmac_t<Op::mul, 0xf>(vf10, vf10, rx_bc<0>(vf20), acc, hi);
    vf21 = rx_fmac_t<Op::madd, 0x8>(vf21, vf5, rx_bc<3>(vf0), acc, hi);
    vf28 = rx_fmac_t<Op::sub, 0xf>(vf28, vf28, rx_bc<3>(vf0), acc, hi);
    out_a[outs] = vi[3] & 0x3ffu; out_v[outs++] = vf10; vi[3] = static_cast<std::uint16_t>(vi[3] + 1);
    vf9 = rx_fmac_t<Op::sub, 0xe>(vf9, vf30, vdupq_n_u32(i), acc, hi);
    const bool again = static_cast<std::int16_t>(vi[2]) > 0;
    out_a[outs] = vi[4] & 0x3ffu; out_v[outs++] = vf13; vi[4] = static_cast<std::uint16_t>(vi[4] + 1);
    if (refused || outs != 2 || !rx_clean(vorrq_u32(bad, vcgeq_u32(hi, vdupq_n_u32(0x7f800000u))))) return -1;
    vst1q_u32(s.vf[1].data(), vf1);
    vst1q_u32(s.vf[2].data(), vf2);
    vst1q_u32(s.vf[3].data(), vf3);
    vst1q_u32(s.vf[4].data(), vf4);
    vst1q_u32(s.vf[5].data(), vf5);
    vst1q_u32(s.vf[6].data(), vf6);
    vst1q_u32(s.vf[7].data(), vf7);
    vst1q_u32(s.vf[8].data(), vf8);
    vst1q_u32(s.vf[9].data(), vf9);
    vst1q_u32(s.vf[10].data(), vf10);
    vst1q_u32(s.vf[11].data(), vf11);
    vst1q_u32(s.vf[12].data(), vf12);
    vst1q_u32(s.vf[13].data(), vf13);
    vst1q_u32(s.vf[14].data(), vf14);
    vst1q_u32(s.vf[15].data(), vf15);
    vst1q_u32(s.vf[16].data(), vf16);
    vst1q_u32(s.vf[17].data(), vf17);
    vst1q_u32(s.vf[18].data(), vf18);
    vst1q_u32(s.vf[19].data(), vf19);
    vst1q_u32(s.vf[20].data(), vf20);
    vst1q_u32(s.vf[21].data(), vf21);
    vst1q_u32(s.vf[22].data(), vf22);
    vst1q_u32(s.vf[23].data(), vf23);
    vst1q_u32(s.vf[24].data(), vf24);
    vst1q_u32(s.vf[25].data(), vf25);
    vst1q_u32(s.vf[26].data(), vf26);
    vst1q_u32(s.vf[27].data(), vf27);
    vst1q_u32(s.vf[28].data(), vf28);
    vst1q_u32(s.vf[29].data(), vf29);
    vst1q_u32(s.vf[30].data(), vf30);
    vst1q_u32(s.vf[31].data(), vf31);
    vst1q_u32(s.acc.data(), acc);
    s.i = i, s.q = q, s.r = r;
    s.vi = vi;
    vst1q_u32(s.data[out_a[0]].data(), out_v[0]);
    vst1q_u32(s.data[out_a[1]].data(), out_v[1]);
    return again ? 1 : 0;
}

// Generated (132vc1 hw5, tools: local regs_gen2.py) from the loop body of program 0x170 above: the iteration with
// the VU registers in NEON registers, each value tested once. -1: refused (the state is untouched); else the loop
// branch, the state written.
OMEGA_PARTICLE_NOINLINE int iteration_regs_170(Vu1State& s) {
    const uint32x4_t vf0 = {0u, 0u, 0u, 0x3f800000u};
    uint32x4_t acc = vld1q_u32(s.acc.data());
    std::uint32_t i = s.i, q = s.q, r = s.r;
    std::array<std::uint16_t, 16> vi = s.vi;
    uint32x4_t bad = vdupq_n_u32(0), hi = vdupq_n_u32(0);
    bool refused = false;
    std::uint32_t out_a[2];
    uint32x4_t out_v[2];
    int outs = 0;
    (void)vf0;
    vi[2] = static_cast<std::uint16_t>(vi[2] - 1);
    uint32x4_t vf29 = vld1q_u32(s.vf[29].data());
    vf29 = rx_fmac_t<Op::sub, 0xf>(vf29, vf29, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf19 = vld1q_u32(s.vf[19].data());
    vi[5] = static_cast<std::uint16_t>(vgetq_lane_u32(vf19, 3));
    uint32x4_t vf10 = vld1q_u32(s.vf[10].data());
    uint32x4_t vf30 = vld1q_u32(s.vf[30].data());
    vf10 = rx_fmac_t<Op::sub, 0xe>(vf10, vf30, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf24 = vld1q_u32(s.vf[24].data());
    vi[6] = static_cast<std::uint16_t>(vgetq_lane_u32(vf24, 3));
    uint32x4_t vf28 = vld1q_u32(s.vf[28].data());
    vf28 = rx_fmac_t<Op::sub, 0xf>(vf28, vf28, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf6 = vld1q_u32(s.vf[6].data());
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf29, vf6, acc, hi);
    uint32x4_t vf5 = vld1q_u32(s.vf[5].data());
    uint32x4_t vf21 = vld1q_u32(s.vf[21].data());
    vf21 = rx_fmac_t<Op::madd, 0xf>(vf21, vf5, rx_bc<3>(vf0), acc, hi);
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf0, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf26 = vld1q_u32(s.vf[26].data());
    vf26 = rx_fmac_t<Op::sub, 0x1>(vf26, vf26, vf0, acc, hi);
    uint32x4_t vf22 = vld1q_u32(s.vf[22].data());
    vf22 = rx_fmac_t<Op::msub, 0x8>(vf22, vf21, vf21, acc, hi);
    uint32x4_t vf2 = vld1q_u32(s.vf[2].data());
    acc = rx_outer<false>(acc, vf2, vf10, acc, bad);
    vf10 = rx_outer<true>(vf10, vf10, vf2, acc, bad);
    uint32x4_t vf8 = vld1q_u32(s.vf[8].data());
    uint32x4_t vf20 = vld1q_u32(s.vf[20].data());
    vf20 = rx_fmac_t<Op::mul, 0xf>(vf20, vf28, vf8, acc, hi);
    q = rx_sqrt(vgetq_lane_u32(vf22, 0));
    uint32x4_t vf15 = vld1q_u32(s.vf[15].data());
    vf15 = rx_fmac_t<Op::mul, 0xe>(vf15, vf10, vf10, acc, hi);
    uint32x4_t vf18 = vld1q_u32(s.vf[18].data());
    vf22 = rx_fmac_t<Op::mul, 0x1>(vf22, vf18, vf30, acc, hi);
    uint32x4_t vf7 = vld1q_u32(s.vf[7].data());
    uint32x4_t vf23 = vld1q_u32(s.vf[23].data());
    vf23 = rx_fmac_t<Op::mul, 0x1>(vf23, vf26, vf7, acc, hi);
    uint32x4_t vf1 = vld1q_u32(s.vf[1].data());
    vf22 = rx_fmac_t<Op::mul, 0x1>(vf22, vf22, vf1, acc, hi);
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf15, rx_bc<1>(vf15), acc, hi);
    uint32x4_t vf31 = vld1q_u32(s.vf[31].data());
    vf15 = rx_fmac_t<Op::madd, 0x8>(vf15, vf31, rx_bc<2>(vf15), acc, hi);
    vf22 = rx_fmac_t<Op::add, 0x8>(vf22, vf0, vdupq_n_u32(q), acc, hi);
    vf20 = rx_fmac_t<Op::add, 0xc>(vf20, vf20, vf7, acc, hi);
    if (!rx_rsqrt(q, vgetq_lane_u32(vf0, 3), vgetq_lane_u32(vf15, 0))) refused = true;
    uint32x4_t vf27 = vld1q_u32(s.vf[27].data());
    vf27 = vextq_u32(vf27, vf27, 1);
    i = 0x40a00000u;
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf27, vdupq_n_u32(i), acc, hi);
    vf27 = rx_fmac_t<Op::madd, 0xf>(vf27, vf31, rx_bc<3>(vf0), acc, hi);
    i = 0x3fc00000u;
    r = (r ^ vgetq_lane_u32(vf27, 0)) & 0x7fffffu;
    vf30 = rx_rget<(0x8)>(vf30, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x8)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x8)>(vf28, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x8)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 1)) & 0x7fffffu;
    vf29 = rx_rget<(0x4)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x4)>(vf28, r);
    rx_rnext(r);
    vf30 = rx_rget<(0x4)>(vf30, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x4)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 2)) & 0x7fffffu;
    vf28 = rx_rget<(0x2)>(vf28, r);
    rx_rnext(r);
    vf30 = rx_rget<(0x2)>(vf30, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x2)>(vf29, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x2)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 3)) & 0x7fffffu;
    vf30 = rx_rget<(0x1)>(vf30, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x1)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x1)>(vf28, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x1)>(vf27, r);
    rx_rnext(r);
    vf26 = rx_rget<(0x1)>(vf26, r);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf2, rx_bc<0>(vf21), acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xe>(vf10, vf10, vdupq_n_u32(q), acc, hi);
    i = 0x38d1b717u;
    vf23 = rx_fmac_t<Op::add, 0x1>(vf23, vf23, rx_bc<2>(vf7), acc, hi);
    q = rx_div(vgetq_lane_u32(vf0, 3), vgetq_lane_u32(vf21, 2));
    vf23 = rx_fmac_t<Op::add, 0xe>(vf23, vf10, rx_bc<0>(vf0), acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0xe>(vf10, vf10, rx_bc<0>(vf22), acc, hi);
    uint32x4_t vf4 = vld1q_u32(s.vf[4].data());
    vf20 = rx_fmac_t<Op::mul, 0x4>(vf20, vf20, vf4, acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xe>(vf10, vf10, rx_bc<1>(vf21), acc, hi);
    uint32x4_t vf25 = vld1q_u32(s.vf[25].data());
    vf25 = rx_fmac_t<Op::mul, 0xe>(vf25, vf24, rx_bc<1>(vf20), acc, hi);
    vf10 = rx_fmac_t<Op::add, 0xe>(vf10, vf10, vf25, acc, hi);
    vf25 = vld1q_u32(s.data[(15) & 0x3ffu].data());
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf10, rx_bc<0>(vf20), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf23, rx_bc<3>(vf23), acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0xe>(vf10, vf25, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf12 = vld1q_u32(s.vf[12].data());
    vf12 = rx_fmac_t<Op::sub, 0x8>(vf12, vf4, rx_bc<3>(vf21), acc, hi);
    vf12 = rx_minmax<true, (0x8)>(vf12, vf12, vdupq_n_u32(i), bad);
    vf12 = rx_minmax<false, (0x8)>(vf12, vf12, rx_bc<2>(vf21), bad);
    vf12 = rx_fmac_t<Op::add, 0x2>(vf12, vf0, rx_bc<0>(vf12), acc, hi);
    vf12 = rx_fmac_t<Op::mul, 0xa>(vf12, vf12, vdupq_n_u32(q), acc, hi);
    vf12 = rx_fmac_t<Op::sub, 0x5>(vf12, vf31, rx_bc<0>(vf12), acc, hi);
    uint32x4_t vf9 = vld1q_u32(s.vf[9].data());
    uint32x4_t vf14 = vld1q_u32(s.vf[14].data());
    vf14 = rx_fmac_t<Op::mul, 0xc>(vf14, vf9, vf12, acc, hi);
    uint32x4_t vf3 = vld1q_u32(s.vf[3].data());
    vf14 = rx_minmax<false, (0x1)>(vf14, vf3, rx_bc<0>(vf14), bad);
    vf14 = rx_fmac_t<Op::mul, 0x4>(vf14, vf14, rx_bc<3>(vf14), acc, hi);
    vf14 = rx_minmax<false, (0x1)>(vf14, vf14, rx_bc<1>(vf14), bad);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, rx_bc<2>(vf20), acc, hi);
    vf14 = rx_fmac_t<Op::madd, 0x1>(vf14, vf14, vf0, acc, hi);
    i = 0x437f0000u;
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf13 = vld1q_u32(s.vf[13].data());
    vf13 = rx_fmac_t<Op::msub, 0x1>(vf13, vf14, rx_bc<2>(vf4), acc, hi);
    uint32x4_t vf11 = vld1q_u32(s.vf[11].data());
    uint32x4_t vf17 = vld1q_u32(s.vf[17].data());
    vf17 = rx_fmac_t<Op::mul, 0xf>(vf17, vf11, vf12, acc, hi);
    vf17 = rx_minmax<false, (0xf)>(vf17, vf17, rx_bc<3>(vf0), bad);
    acc = rx_fmac_t<Op::mul, 0x3>(acc, vf9, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf16 = vld1q_u32(s.vf[16].data());
    vf16 = rx_fmac_t<Op::msub, 0x3>(vf16, vf9, vf17, acc, hi);
    acc = rx_fmac_t<Op::mul, 0x2>(acc, vf16, rx_bc<3>(vf0), acc, hi);
    vf16 = rx_fmac_t<Op::madd, 0x2>(vf16, vf17, rx_bc<3>(vf25), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf16, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf17, rx_bc<2>(vf16), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf20, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf10, vf0, acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::msub, 0x1>(vf10, vf10, vf4, acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf3, rx_bc<0>(vf17), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf18, rx_bc<3>(vf0), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf18, rx_bc<0>(vf17), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<1>(vf17), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf19, rx_bc<3>(vf0), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf19, rx_bc<1>(vf17), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<3>(vf1), acc, hi);
    acc = rx_fmac_t<Op::msub, 0xe>(acc, vf13, rx_bc<3>(vf22), acc, hi);
    vf13 = rx_fmac_t<Op::madd, 0xe>(vf13, vf1, rx_bc<3>(vf0), acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xf>(vf10, vf10, rx_bc<3>(vf2), acc, hi);
    vf13 = rx_ftoi0_t<0xf>(vf13, vf13);
    i = 0x3fc00000u;
    out_a[outs] = vi[3] & 0x3ffu; out_v[outs++] = vf10; vi[3] = static_cast<std::uint16_t>(vi[3] + 1);
    const bool again = static_cast<std::int16_t>(vi[2]) > 0;
    out_a[outs] = vi[4] & 0x3ffu; out_v[outs++] = vf13; vi[4] = static_cast<std::uint16_t>(vi[4] + 1);
    if (refused || outs != 2 || !rx_clean(vorrq_u32(bad, vcgeq_u32(hi, vdupq_n_u32(0x7f800000u))))) return -1;
    vst1q_u32(s.vf[1].data(), vf1);
    vst1q_u32(s.vf[2].data(), vf2);
    vst1q_u32(s.vf[3].data(), vf3);
    vst1q_u32(s.vf[4].data(), vf4);
    vst1q_u32(s.vf[5].data(), vf5);
    vst1q_u32(s.vf[6].data(), vf6);
    vst1q_u32(s.vf[7].data(), vf7);
    vst1q_u32(s.vf[8].data(), vf8);
    vst1q_u32(s.vf[9].data(), vf9);
    vst1q_u32(s.vf[10].data(), vf10);
    vst1q_u32(s.vf[11].data(), vf11);
    vst1q_u32(s.vf[12].data(), vf12);
    vst1q_u32(s.vf[13].data(), vf13);
    vst1q_u32(s.vf[14].data(), vf14);
    vst1q_u32(s.vf[15].data(), vf15);
    vst1q_u32(s.vf[16].data(), vf16);
    vst1q_u32(s.vf[17].data(), vf17);
    vst1q_u32(s.vf[18].data(), vf18);
    vst1q_u32(s.vf[19].data(), vf19);
    vst1q_u32(s.vf[20].data(), vf20);
    vst1q_u32(s.vf[21].data(), vf21);
    vst1q_u32(s.vf[22].data(), vf22);
    vst1q_u32(s.vf[23].data(), vf23);
    vst1q_u32(s.vf[24].data(), vf24);
    vst1q_u32(s.vf[25].data(), vf25);
    vst1q_u32(s.vf[26].data(), vf26);
    vst1q_u32(s.vf[27].data(), vf27);
    vst1q_u32(s.vf[28].data(), vf28);
    vst1q_u32(s.vf[29].data(), vf29);
    vst1q_u32(s.vf[30].data(), vf30);
    vst1q_u32(s.vf[31].data(), vf31);
    vst1q_u32(s.acc.data(), acc);
    s.i = i, s.q = q, s.r = r;
    s.vi = vi;
    vst1q_u32(s.data[out_a[0]].data(), out_v[0]);
    vst1q_u32(s.data[out_a[1]].data(), out_v[1]);
    return again ? 1 : 0;
}

// Generated (132vc1 hw5, tools: local regs_gen2.py) from the loop body of program 0x208 above: the iteration with
// the VU registers in NEON registers, each value tested once. -1: refused (the state is untouched); else the loop
// branch, the state written.
OMEGA_PARTICLE_NOINLINE int iteration_regs_208(Vu1State& s) {
    const uint32x4_t vf0 = {0u, 0u, 0u, 0x3f800000u};
    uint32x4_t acc = vld1q_u32(s.acc.data());
    std::uint32_t i = s.i, q = s.q, r = s.r;
    std::array<std::uint16_t, 16> vi = s.vi;
    uint32x4_t bad = vdupq_n_u32(0), hi = vdupq_n_u32(0);
    bool refused = false;
    std::uint32_t out_a[2];
    uint32x4_t out_v[2];
    int outs = 0;
    (void)vf0;
    vi[2] = static_cast<std::uint16_t>(vi[2] - 1);
    uint32x4_t vf29 = vld1q_u32(s.vf[29].data());
    vf29 = rx_fmac_t<Op::sub, 0xf>(vf29, vf29, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf19 = vld1q_u32(s.vf[19].data());
    vi[5] = static_cast<std::uint16_t>(vgetq_lane_u32(vf19, 3));
    uint32x4_t vf24 = vld1q_u32(s.vf[24].data());
    vi[6] = static_cast<std::uint16_t>(vgetq_lane_u32(vf24, 3));
    uint32x4_t vf23 = vld1q_u32(s.vf[23].data());
    uint32x4_t vf30 = vld1q_u32(s.vf[30].data());
    vf23 = rx_fmac_t<Op::sub, 0xe>(vf23, vf30, vdupq_n_u32(i), acc, hi);
    vi[6] = static_cast<std::uint16_t>(vgetq_lane_u32(vf24, 3));
    uint32x4_t vf28 = vld1q_u32(s.vf[28].data());
    vf28 = rx_fmac_t<Op::sub, 0xf>(vf28, vf28, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf6 = vld1q_u32(s.vf[6].data());
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf29, vf6, acc, hi);
    uint32x4_t vf5 = vld1q_u32(s.vf[5].data());
    uint32x4_t vf21 = vld1q_u32(s.vf[21].data());
    vf21 = rx_fmac_t<Op::madd, 0xf>(vf21, vf5, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf15 = vld1q_u32(s.vf[15].data());
    acc = rx_fmac_t<Op::add, 0x8>(acc, vf15, rx_bc<1>(vf15), acc, hi);
    uint32x4_t vf22 = vld1q_u32(s.vf[22].data());
    vf22 = rx_fmac_t<Op::madd, 0x8>(vf22, vf0, rx_bc<2>(vf15), acc, hi);
    q = rx_div(vgetq_lane_u32(vf0, 3), vgetq_lane_u32(vf22, 0));
    vf23 = rx_fmac_t<Op::mul, 0xe>(vf23, vf23, vf15, acc, hi);
    uint32x4_t vf10 = vld1q_u32(s.vf[10].data());
    vf10 = rx_fmac_t<Op::mul, 0xe>(vf10, vf23, vdupq_n_u32(q), acc, hi);
    uint32x4_t vf26 = vld1q_u32(s.vf[26].data());
    vf26 = rx_fmac_t<Op::sub, 0x1>(vf26, vf26, vf0, acc, hi);
    uint32x4_t vf8 = vld1q_u32(s.vf[8].data());
    uint32x4_t vf20 = vld1q_u32(s.vf[20].data());
    vf20 = rx_fmac_t<Op::mul, 0xf>(vf20, vf28, vf8, acc, hi);
    uint32x4_t vf18 = vld1q_u32(s.vf[18].data());
    vf22 = rx_fmac_t<Op::mul, 0x1>(vf22, vf18, vf30, acc, hi);
    uint32x4_t vf1 = vld1q_u32(s.vf[1].data());
    vf22 = rx_fmac_t<Op::mul, 0x1>(vf22, vf22, vf1, acc, hi);
    uint32x4_t vf7 = vld1q_u32(s.vf[7].data());
    vf20 = rx_fmac_t<Op::add, 0xc>(vf20, vf20, vf7, acc, hi);
    uint32x4_t vf27 = vld1q_u32(s.vf[27].data());
    vf27 = vextq_u32(vf27, vf27, 1);
    i = 0x40a00000u;
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf27, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf31 = vld1q_u32(s.vf[31].data());
    vf27 = rx_fmac_t<Op::madd, 0xf>(vf27, vf31, rx_bc<3>(vf0), acc, hi);
    i = 0x3fc00000u;
    r = (r ^ vgetq_lane_u32(vf27, 0)) & 0x7fffffu;
    vf30 = rx_rget<(0x8)>(vf30, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x8)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x8)>(vf28, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x8)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 1)) & 0x7fffffu;
    vf29 = rx_rget<(0x4)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x4)>(vf28, r);
    rx_rnext(r);
    vf30 = rx_rget<(0x4)>(vf30, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x4)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 2)) & 0x7fffffu;
    vf29 = rx_rget<(0x2)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x2)>(vf28, r);
    rx_rnext(r);
    vf30 = rx_rget<(0x2)>(vf30, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x2)>(vf27, r);
    r = (r ^ vgetq_lane_u32(vf27, 3)) & 0x7fffffu;
    vf30 = rx_rget<(0x1)>(vf30, r);
    rx_rnext(r);
    vf29 = rx_rget<(0x1)>(vf29, r);
    rx_rnext(r);
    vf28 = rx_rget<(0x1)>(vf28, r);
    rx_rnext(r);
    vf27 = rx_rget<(0x1)>(vf27, r);
    rx_rnext(r);
    vf26 = rx_rget<(0x1)>(vf26, r);
    i = 0x38d1b717u;
    q = rx_div(vgetq_lane_u32(vf0, 3), vgetq_lane_u32(vf21, 2));
    uint32x4_t vf4 = vld1q_u32(s.vf[4].data());
    vf20 = rx_fmac_t<Op::mul, 0x4>(vf20, vf20, vf4, acc, hi);
    uint32x4_t vf2 = vld1q_u32(s.vf[2].data());
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf2, rx_bc<0>(vf21), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf10, rx_bc<3>(vf0), acc, hi);
    vf10 = rx_fmac_t<Op::msub, 0xe>(vf10, vf10, rx_bc<0>(vf21), acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xe>(vf10, vf10, rx_bc<1>(vf21), acc, hi);
    uint32x4_t vf25 = vld1q_u32(s.vf[25].data());
    vf25 = rx_fmac_t<Op::mul, 0xe>(vf25, vf24, rx_bc<1>(vf20), acc, hi);
    vf10 = rx_fmac_t<Op::add, 0xe>(vf10, vf10, vf25, acc, hi);
    vf25 = vld1q_u32(s.data[(15) & 0x3ffu].data());
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf10, rx_bc<0>(vf20), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf23, rx_bc<3>(vf0), acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0xe>(vf10, vf25, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf12 = vld1q_u32(s.vf[12].data());
    vf12 = rx_fmac_t<Op::sub, 0x8>(vf12, vf4, rx_bc<3>(vf21), acc, hi);
    vf12 = rx_minmax<true, (0x8)>(vf12, vf12, vdupq_n_u32(i), bad);
    vf12 = rx_minmax<false, (0x8)>(vf12, vf12, rx_bc<2>(vf21), bad);
    vf12 = rx_fmac_t<Op::add, 0x2>(vf12, vf0, rx_bc<0>(vf12), acc, hi);
    vf12 = rx_fmac_t<Op::mul, 0xa>(vf12, vf12, vdupq_n_u32(q), acc, hi);
    vf12 = rx_fmac_t<Op::sub, 0x5>(vf12, vf31, rx_bc<0>(vf12), acc, hi);
    uint32x4_t vf9 = vld1q_u32(s.vf[9].data());
    uint32x4_t vf14 = vld1q_u32(s.vf[14].data());
    vf14 = rx_fmac_t<Op::mul, 0xc>(vf14, vf9, vf12, acc, hi);
    uint32x4_t vf3 = vld1q_u32(s.vf[3].data());
    vf14 = rx_minmax<false, (0x1)>(vf14, vf3, rx_bc<0>(vf14), bad);
    vf14 = rx_fmac_t<Op::mul, 0x4>(vf14, vf14, rx_bc<3>(vf14), acc, hi);
    vf14 = rx_minmax<false, (0x1)>(vf14, vf14, rx_bc<1>(vf14), bad);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, rx_bc<2>(vf20), acc, hi);
    vf14 = rx_fmac_t<Op::madd, 0x1>(vf14, vf14, vf0, acc, hi);
    i = 0x437f0000u;
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf14, vdupq_n_u32(i), acc, hi);
    uint32x4_t vf13 = vld1q_u32(s.vf[13].data());
    vf13 = rx_fmac_t<Op::msub, 0x1>(vf13, vf14, rx_bc<2>(vf4), acc, hi);
    uint32x4_t vf11 = vld1q_u32(s.vf[11].data());
    uint32x4_t vf17 = vld1q_u32(s.vf[17].data());
    vf17 = rx_fmac_t<Op::mul, 0xf>(vf17, vf11, vf12, acc, hi);
    vf17 = rx_minmax<false, (0xf)>(vf17, vf17, rx_bc<3>(vf0), bad);
    acc = rx_fmac_t<Op::mul, 0x3>(acc, vf9, rx_bc<3>(vf0), acc, hi);
    uint32x4_t vf16 = vld1q_u32(s.vf[16].data());
    vf16 = rx_fmac_t<Op::msub, 0x3>(vf16, vf9, vf17, acc, hi);
    acc = rx_fmac_t<Op::mul, 0x2>(acc, vf16, rx_bc<3>(vf0), acc, hi);
    vf16 = rx_fmac_t<Op::madd, 0x2>(vf16, vf17, rx_bc<3>(vf25), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf16, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf17, rx_bc<2>(vf16), acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf20, acc, hi);
    vf10 = rx_fmac_t<Op::madd, 0x1>(vf10, vf10, vf0, acc, hi);
    acc = rx_fmac_t<Op::mul, 0x1>(acc, vf10, vf0, acc, hi);
    vf10 = rx_fmac_t<Op::msub, 0x1>(vf10, vf10, vf4, acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf3, rx_bc<0>(vf17), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf18, rx_bc<3>(vf0), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf18, rx_bc<0>(vf17), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<1>(vf17), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xe>(acc, vf19, rx_bc<3>(vf0), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf19, rx_bc<1>(vf17), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xe>(acc, vf13, rx_bc<3>(vf1), acc, hi);
    vf13 = rx_fmac_t<Op::msub, 0xe>(vf13, vf13, rx_bc<3>(vf22), acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xf>(vf10, vf10, rx_bc<3>(vf2), acc, hi);
    vf13 = rx_ftoi0_t<0xf>(vf13, vf13);
    i = 0x3fc00000u;
    out_a[outs] = vi[3] & 0x3ffu; out_v[outs++] = vf10; vi[3] = static_cast<std::uint16_t>(vi[3] + 1);
    const bool again = static_cast<std::int16_t>(vi[2]) > 0;
    out_a[outs] = vi[4] & 0x3ffu; out_v[outs++] = vf13; vi[4] = static_cast<std::uint16_t>(vi[4] + 1);
    if (refused || outs != 2 || !rx_clean(vorrq_u32(bad, vcgeq_u32(hi, vdupq_n_u32(0x7f800000u))))) return -1;
    vst1q_u32(s.vf[1].data(), vf1);
    vst1q_u32(s.vf[2].data(), vf2);
    vst1q_u32(s.vf[3].data(), vf3);
    vst1q_u32(s.vf[4].data(), vf4);
    vst1q_u32(s.vf[5].data(), vf5);
    vst1q_u32(s.vf[6].data(), vf6);
    vst1q_u32(s.vf[7].data(), vf7);
    vst1q_u32(s.vf[8].data(), vf8);
    vst1q_u32(s.vf[9].data(), vf9);
    vst1q_u32(s.vf[10].data(), vf10);
    vst1q_u32(s.vf[11].data(), vf11);
    vst1q_u32(s.vf[12].data(), vf12);
    vst1q_u32(s.vf[13].data(), vf13);
    vst1q_u32(s.vf[14].data(), vf14);
    vst1q_u32(s.vf[15].data(), vf15);
    vst1q_u32(s.vf[16].data(), vf16);
    vst1q_u32(s.vf[17].data(), vf17);
    vst1q_u32(s.vf[18].data(), vf18);
    vst1q_u32(s.vf[19].data(), vf19);
    vst1q_u32(s.vf[20].data(), vf20);
    vst1q_u32(s.vf[21].data(), vf21);
    vst1q_u32(s.vf[22].data(), vf22);
    vst1q_u32(s.vf[23].data(), vf23);
    vst1q_u32(s.vf[24].data(), vf24);
    vst1q_u32(s.vf[25].data(), vf25);
    vst1q_u32(s.vf[26].data(), vf26);
    vst1q_u32(s.vf[27].data(), vf27);
    vst1q_u32(s.vf[28].data(), vf28);
    vst1q_u32(s.vf[29].data(), vf29);
    vst1q_u32(s.vf[30].data(), vf30);
    vst1q_u32(s.vf[31].data(), vf31);
    vst1q_u32(s.acc.data(), acc);
    s.i = i, s.q = q, s.r = r;
    s.vi = vi;
    vst1q_u32(s.data[out_a[0]].data(), out_v[0]);
    vst1q_u32(s.data[out_a[1]].data(), out_v[1]);
    return again ? 1 : 0;
}
#endif
#if OMEGA_PARTICLE_NEON
// 132vc1 hw5 (port-side speed only, research/VITA.md "Render pass on the Vita's cores (132vc1)"; the same bits): one
// sprite of the sprite programs 0x5e0 / 0x8a0 (run_sprite_program_into's loop body) with the registers in NEON
// registers. Each FMAC is SpecMachine's arithmetic and its refusal lanes (an operand, the ACC of MADD or a result with
// exponent 255, in a dest lane) are those of its result alone, as in the register-resident program iterations (an
// infinity or NaN operand lane always gives an infinity or NaN result lane).
// False: refused, nothing written (the state, `out`); the caller then runs the sprite on iterate() as before.
OMEGA_PARTICLE_NOINLINE bool sprite_regs(Vu1State& s, const Qword& position, const Qword& colour, bool flipped,
                                         std::vector<Qword>& out) {
    uint32x4_t hi = vdupq_n_u32(0);
    const uint32x4_t vf0 = {0u, 0u, 0u, 0x3f800000u};
    const uint32x4_t vf15 = vld1q_u32(position.data()), vf23 = vld1q_u32(colour.data());
    const uint32x4_t vf1 = vld1q_u32(s.vf[1].data()), vf2 = vld1q_u32(s.vf[2].data()), vf3 = vld1q_u32(s.vf[3].data());
    const uint32x4_t vf4 = vld1q_u32(s.vf[4].data()), vf5 = vld1q_u32(s.vf[5].data()), vf6 = vld1q_u32(s.vf[6].data());
    const uint32x4_t vf8 = vld1q_u32(s.vf[8].data()), vf11 = vld1q_u32(s.vf[11].data()), vf12 = vld1q_u32(s.vf[12].data());
    const uint32x4_t vf30 = vld1q_u32(s.vf[30].data());
    uint32x4_t vf9 = vld1q_u32(s.vf[9].data()), vf10 = vld1q_u32(s.vf[10].data()), vf16 = vld1q_u32(s.vf[16].data());
    uint32x4_t vf17 = vld1q_u32(s.vf[17].data()), vf18 = vld1q_u32(s.vf[18].data()), vf19 = vld1q_u32(s.vf[19].data());
    uint32x4_t vf20 = vld1q_u32(s.vf[20].data()), vf21 = vld1q_u32(s.vf[21].data()), vf22 = vld1q_u32(s.vf[22].data());
    uint32x4_t vf25 = vld1q_u32(s.vf[25].data()), vf26 = vld1q_u32(s.vf[26].data());
    uint32x4_t vf13 = vld1q_u32(s.vf[13].data()), vf14 = vld1q_u32(s.vf[14].data());
    uint32x4_t acc = vld1q_u32(s.acc.data());
    vf9 = rx_fmac_t<Op::mul, 0xe>(vf9, vf8, rx_bc<3>(vf15), acc, hi);
    acc = rx_fmac_t<Op::mul, 0xf>(acc, vf1, rx_bc<0>(vf15), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xf>(acc, vf2, rx_bc<1>(vf15), acc, hi);
    acc = rx_fmac_t<Op::madd, 0xf>(acc, vf3, rx_bc<2>(vf15), acc, hi);
    vf16 = rx_fmac_t<Op::madd, 0xf>(vf16, vf4, rx_bc<3>(vf0), acc, hi);
    vf10 = rx_fmac_t<Op::add, 0xe>(vf10, vf9, vf0, acc, hi);
    // CLIPW.xyz vf16, vf16w: |x| > |w| for x, y or z (daz: a zero exponent is zero, as the flush-to-zero compare reads
    // it; exponent 255 cannot pass the result test above).
    const float32x4_t c16 = vreinterpretq_f32_u32(vf16);
    const uint32x4_t out_of = vcagtq_f32(c16, vdupq_lane_f32(vget_high_f32(c16), 1));
    const uint32x2_t ox = vorr_u32(vget_low_u32(out_of), vand_u32(vget_high_u32(out_of), uint32x2_t{~0u, 0u}));
    const bool clipped = vget_lane_u32(vpmax_u32(ox, ox), 0) != 0;
    const std::uint32_t adc = clipped ? 0xffff8000u : 0x00007fffu;
    const std::uint32_t q = rx_div(0x3f800000u, vgetq_lane_u32(vf16, 3));
    const uint32x4_t qv = vdupq_n_u32(q);
    vf17 = rx_fmac_t<Op::mul, 0xe>(vf17, vf16, vf5, acc, hi);
    acc = rx_fmac_t<Op::add, 0xe>(acc, vf6, vf0, acc, hi);
    vf18 = rx_fmac_t<Op::madd, 0xe>(vf18, vf17, qv, acc, hi);
    vf10 = rx_fmac_t<Op::mul, 0xc>(vf10, vf10, qv, acc, hi);
    if (flipped) {
        const uint32x4_t zw = {0u, 0u, 0x3f800000u, 0u};
        vf13 = rx_write<0x3>(vf13, zw);
        vf14 = rx_write<0x3>(vf14, zw);
        vf13 = rx_fmac_t<Op::add, 0xc>(vf13, vf11, vf30, acc, hi);
        vf14 = rx_fmac_t<Op::add, 0xc>(vf14, vf12, vf30, acc, hi);
        vf26 = rx_fmac_t<Op::mul, 0xf>(vf26, vf13, qv, acc, hi);
        vf25 = rx_fmac_t<Op::mul, 0xf>(vf25, vf14, qv, acc, hi);
    } else {
        vf25 = rx_fmac_t<Op::mul, 0xf>(vf25, vf11, qv, acc, hi);
        vf26 = rx_fmac_t<Op::mul, 0xf>(vf26, vf12, qv, acc, hi);
    }
    vf19 = rx_fmac_t<Op::sub, 0xe>(vf19, vf18, vf10, acc, hi);
    vf20 = rx_fmac_t<Op::add, 0xe>(vf20, vf18, vf10, acc, hi);
    vf21 = vsetq_lane_u32(adc, rx_ftoi0_t<0xe>(vf21, vf19), 3);
    vf22 = vsetq_lane_u32(adc, rx_ftoi0_t<0xe>(vf22, vf20), 3);
    if (!rx_clean(vcgeq_u32(hi, vdupq_n_u32(0x7f800000u)))) return false;
    vst1q_u32(s.vf[9].data(), vf9);
    vst1q_u32(s.vf[10].data(), vf10);
    vst1q_u32(s.vf[15].data(), vf15);
    vst1q_u32(s.vf[16].data(), vf16);
    vst1q_u32(s.vf[17].data(), vf17);
    vst1q_u32(s.vf[18].data(), vf18);
    vst1q_u32(s.vf[19].data(), vf19);
    vst1q_u32(s.vf[20].data(), vf20);
    vst1q_u32(s.vf[21].data(), vf21);
    vst1q_u32(s.vf[22].data(), vf22);
    vst1q_u32(s.vf[23].data(), vf23);
    vst1q_u32(s.vf[25].data(), vf25);
    vst1q_u32(s.vf[26].data(), vf26);
    vst1q_u32(s.acc.data(), acc);
    s.q = q;
    if (flipped) {  // mr32.xyzw vf11 / vf12, as the loop body ends
        vst1q_u32(s.vf[13].data(), vf13);
        vst1q_u32(s.vf[14].data(), vf14);
        vst1q_u32(s.vf[11].data(), vextq_u32(vf11, vf11, 1));
        vst1q_u32(s.vf[12].data(), vextq_u32(vf12, vf12, 1));
    }
    const std::size_t n = out.size();
    out.resize(n + 6);
    vst1q_u32(out[n].data(), vf25);
    vst1q_u32(out[n + 1].data(), vf23);
    vst1q_u32(out[n + 2].data(), vf21);
    vst1q_u32(out[n + 3].data(), vf26);
    vst1q_u32(out[n + 4].data(), vf23);
    vst1q_u32(out[n + 5].data(), vf22);
    return true;
}
#endif

}  // namespace

std::uint64_t particle_neon_rollbacks() { return g_neon_rollbacks.load(std::memory_order_relaxed); }
void set_particle_neon_speculation(bool on) { g_speculate.store(on, std::memory_order_relaxed); }
bool particle_neon_speculation() { return g_speculate.load(std::memory_order_relaxed); }
void set_particle_neon_registers(bool on) {
#if OMEGA_PARTICLE_NEON
    g_regs.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

Vu1State& thread_vu1_state() {
    thread_local std::unique_ptr<Vu1State> state;
    if (!state) return *(state = std::make_unique<Vu1State>());
    Vu1State& s = *state;
    s.vf = {};
    s.vf[0] = {0, 0, 0, 0x3f800000u};
    s.acc = {};
    s.i = s.q = s.r = 0;
    s.vi = {};
    return s;
}

std::uint32_t run_particle_program(Vu1State& vu, std::uint32_t top) {
    Machine m{vu};
    auto& s = vu;
    auto v = [&](int r) -> Qword& { return s.vf[r]; };
    auto I = [&] { return imm(s.i); };
    auto Q = [&] { return imm(s.q); };
    auto& acc = s.acc;
    auto P = [](std::uint32_t pair) { g_pair = pair; };
    if (v(0) != Qword{0, 0, 0, 0x3f800000u}) throw std::runtime_error("VU1 particle program: vf0 is not (0, 0, 0, 1)");

    P(0x29c); m.minmax(true, v(31), v(0), bc(v(0), 3), XYZW); s.vi[12] = static_cast<std::uint16_t>(top & 0x3ff);
    P(0x29d); s.vi[12] = static_cast<std::uint16_t>(s.vi[12] + 0xb);
    P(0x29e); m.minmax(true, v(18), v(0), bc(v(0), 0), W); m.lq(27, s.vi[12] + 1u);
    P(0x29f); s.i = 0x3fc00000u;  // 1.5
    P(0x2a0); s.vi[11] = static_cast<std::uint16_t>(m.mem(s.vi[12])[0]);
    P(0x2a1); s.vi[8] = 0x10;
    P(0x2a2); m.lqi(1, 8);
    P(0x2a3); m.fmac(Op::add, v(29), v(27), v(31), XYZW); m.lqi(2, 8);
    P(0x2a4); m.fmac(Op::add, v(30), v(27), v(31), XYZW); m.lqi(8, 8);
    P(0x2a5); m.lqi(15, 8);
    P(0x2a6); m.lqi(16, 8);
    P(0x2a7); m.lqi(17, 8);
    P(0x2a8); m.fmac(Op::add, v(28), v(27), v(31), XYZW); m.lqi(3, 8);
    P(0x2a9); m.itof0(v(1), v(1), XYZ); m.lqi(5, 8);
    P(0x2aa); m.fmac(Op::add, v(27), v(27), v(31), XYZW); m.lqi(6, 8);
    P(0x2ab); m.fmac(Op::sub, v(29), v(29), I(), X | Z | W); m.lqi(7, 8);
    P(0x2ac); m.fmac(Op::sub, v(9), v(30), I(), XYZ); m.lqi(4, 8);
    P(0x2ad); m.fmac(Op::add, v(5), v(5), v(5), Y | Z | W);
    P(0x2ae); s.i = 0x437f0000u;  // 255.0
    P(0x2af); m.fmac(Op::mul, acc, v(1), I(), XYZ);
    P(0x2b0); m.fmac(Op::msub, v(1), v(1), bc(v(1), 3), XYZ);
    P(0x2b1); m.lqi(11, 8);
    P(0x2b2); m.fmac(Op::mul, acc, v(29), bc(v(5), 1), X); m.lqi(19, 8);
    P(0x2b3); m.lqi(20, 8);
    P(0x2b4); s.i = 0x3b7f9724u;  // 0.0039
    P(0x2b5); m.fmac(Op::mul, v(1), v(1), I(), XYZ); s.vi[3] = static_cast<std::uint16_t>(s.vi[12] + 1);
    P(0x2b6); m.fmac(Op::sub, v(28), v(28), bc(v(0), 3), XYZW); s.vi[9] = 0x7fff;
    P(0x2b7); m.fmac(Op::madd, v(21), v(5), bc(v(0), 3), X);
    P(0x2b8); s.vi[2] = s.vi[9] & s.vi[11];
    P(0x2b9); m.ftoi0(v(20), v(20), Z); s.vi[4] = static_cast<std::uint16_t>(s.vi[3] + s.vi[2]);
    P(0x2ba); m.ftoi0(v(2), v(2), W); s.r = v(27)[0] & 0x7fffffu;  // rinit R, vf27x
    // 132vc1: one particle per iteration (iterate: speculatively on the Vita).
    const auto iteration = [&](auto& x) -> bool {
        P(0x2bb); x.div(v(4)[3], v(21)[0]);
        P(0x2bc); x.fmac(Op::mul, v(28), v(28), v(4), Y | Z); x.write(v(25), v(28), W);
        P(0x2bd); x.outer(false, acc, v(2), v(9)); x.mtir(5, v(2)[3]);
        P(0x2be); x.outer(true, v(9), v(9), v(2)); x.mtir(6, v(20)[2]);
        P(0x2bf); x.fmac(Op::mul, v(21), v(29), v(5), Z | W); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x2c0); x.fmac(Op::add, acc, v(0), x.bc(v(4), 0), X | Y);
        P(0x2c1); x.fmac(Op::msub, acc, v(31), x.bc(v(28), 2), X | Y);
        P(0x2c2); x.fmac(Op::madd, v(12), v(31), Q(), X | Y);
        P(0x2c3); x.fmac(Op::mul, v(26), v(9), v(9), XYZ); s.i = 0x40a00000u;  // 5.0
        P(0x2c4); x.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x2c5); x.fmac(Op::madd, v(27), v(31), x.bc(v(0), 3), XYZW);
        P(0x2c6); x.ftoi0(v(22), v(12), X | Y);
        P(0x2c7); x.fmac(Op::add, acc, v(26), x.bc(v(26), 1), X);
        P(0x2c8); x.fmac(Op::madd, v(26), v(31), x.bc(v(26), 2), X);
        P(0x2c9); x.itof0(v(22), v(22), X | Y); s.i = 0x40400000u;  // 3.0
        P(0x2ca); x.fmac(Op::mul, acc, v(12), I(), X | Y);
        {
            x.mr32(v(27));  // mr32
        }
        P(0x2cb); x.fmac(Op::sub, v(29), v(29), x.bc(v(0), 3), Y); x.rsqrt(v(31)[0], v(26)[0]);
        P(0x2cc); x.fmac(Op::add, v(18), v(0), x.bc(v(8), 1), XYZ);
        P(0x2cd); x.fmac(Op::msub, v(25), v(22), I(), X | Y);
        P(0x2ce); x.fmac(Op::sub, v(12), v(12), v(22), X | Y);
        P(0x2cf); x.fmac(Op::mul, v(9), v(11), x.bc(v(29), 1), W);
        P(0x2d0); x.fmac(Op::mul, acc, v(18), x.bc(v(8), 0), XYZ);
        P(0x2d1); x.ftoi0(v(25), v(25), Y);
        P(0x2d2); x.fmac(Op::madd, v(18), v(18), x.bc(v(12), 0), XYZ);
        P(0x2d3); x.fmac(Op::sub, v(12), v(31), x.bc(v(28), 1), Y | Z);
        P(0x2d5); x.mtir(9, v(25)[1]); x.itof0(v(25), v(25), Y);  // the lower reads vf25.y first
        P(0x2d6); x.fmac(Op::mul, v(18), v(18), x.bc(v(18), 2), X | Y);
        P(0x2d8); x.fmac(Op::mul, v(9), v(9), Q(), XYZ); s.i = 0x38d1b717u;  // 1e-4
        P(0x2d9); x.fmac(Op::sub, v(25), v(25), x.bc(v(25), 1), X);
        {
            const std::uint32_t qa = (0x1eu + s.vi[9]) & 0x3ff;
            if (qa < 0x16 || qa > 0x20) fail(0x2d9, "LQ outside the uploaded definition block");
            x.lq(26, qa);
        }
        P(0x2da); x.fmac(Op::mul, v(18), v(18), x.bc(v(18), 2), X); x.div(v(0)[3], v(12)[1]);
        P(0x2db); x.fmac(Op::sub, v(22), v(19), x.bc(v(22), 0), W);
        P(0x2dc); x.minmax(false, v(12), v(12), x.bc(v(22), 3), X);
        P(0x2dd); x.fmac(Op::mul, v(9), v(9), v(1), W);
        P(0x2e0); x.minmax(true, v(12), v(12), I(), X); x.rxor(v(27)[0]);
        P(0x2e1); x.rget(27, X);
        P(0x2e2); x.fmac(Op::mul, acc, v(31), x.bc(v(26), 0), Y); x.rnext(29, X);
        P(0x2e3); x.fmac(Op::mul, acc, v(31), x.bc(v(26), 2), W); x.rnext(28, X);
        P(0x2e4); x.fmac(Op::madd, v(26), v(26), x.bc(v(25), 3), Y | W); x.rnext(30, X);
        P(0x2e5); x.minmax(false, v(12), v(12), x.bc(v(12), 1), X); x.rxor(v(27)[1]);
        P(0x2e6); x.fmac(Op::mul, v(22), v(18), v(15), XYZW); x.rget(29, Y);
        P(0x2e7); x.fmac(Op::mul, v(23), v(18), v(16), XYZW); x.rnext(28, Y);
        P(0x2e8); x.fmac(Op::add, acc, v(0), x.bc(v(26), 1), X); x.rnext(27, Y);
        P(0x2e9); x.fmac(Op::msub, acc, v(25), x.bc(v(26), 1), X); x.rnext(30, Y);
        P(0x2ea); x.fmac(Op::madd, v(26), v(25), x.bc(v(26), 3), X); x.rxor(v(27)[2]);
        P(0x2eb); x.fmac(Op::add, v(12), v(0), x.bc(v(12), 0), Z); x.rget(28, Z);
        P(0x2ec); x.fmac(Op::mul, v(24), v(18), v(17), XYZW); x.rnext(27, Z);
        P(0x2ed); x.rnext(29, Z);
        P(0x2ee); x.fmac(Op::mul, v(9), v(9), x.bc(v(26), 0), XYZ); x.rnext(30, Z);
        P(0x2ef); x.fmac(Op::mul, v(12), v(12), Q(), X | Z);
        P(0x2f0); x.fmac(Op::add, acc, v(22), x.bc(v(22), 1), X); x.rxor(v(27)[3]);
        P(0x2f1); x.fmac(Op::madd, acc, v(31), x.bc(v(22), 2), X); x.rget(27, W);
        P(0x2f2); x.fmac(Op::madd, v(10), v(31), x.bc(v(22), 3), X); x.rnext(29, W);
        P(0x2f3); x.fmac(Op::sub, v(12), v(31), x.bc(v(12), 0), Y | W); x.rnext(28, W);
        P(0x2f4); x.fmac(Op::mul, v(18), v(7), v(12), XYZW);
        P(0x2f5); x.fmac(Op::add, acc, v(23), x.bc(v(23), 0), Y);
        P(0x2f6); x.fmac(Op::madd, acc, v(31), x.bc(v(23), 2), Y);
        P(0x2f7); x.fmac(Op::madd, v(10), v(31), x.bc(v(23), 3), Y);
        P(0x2f8); x.fmac(Op::mul, v(14), v(6), v(12), X | Y);
        P(0x2f9); x.minmax(false, v(18), v(18), x.bc(v(0), 3), XYZW); x.rnext(30, W);
        P(0x2fa); x.fmac(Op::add, acc, v(24), x.bc(v(24), 0), Z);
        P(0x2fb); x.fmac(Op::madd, acc, v(31), x.bc(v(24), 1), Z);
        P(0x2fc); x.fmac(Op::madd, v(10), v(31), x.bc(v(24), 3), Z); x.lq(26, 15);
        P(0x2fd); x.fmac(Op::mul, acc, v(6), x.bc(v(0), 3), Z | W);
        P(0x2fe); x.fmac(Op::msub, v(22), v(6), v(18), Z | W);
        P(0x2ff); x.minmax(false, v(14), v(3), x.bc(v(14), 0), W);
        P(0x300); x.fmac(Op::add, acc, v(10), v(26), XYZ);
        P(0x301); x.fmac(Op::madd, v(10), v(31), v(9), XYZ);
        P(0x302); x.fmac(Op::mul, acc, v(3), x.bc(v(18), 0), XYZ);
        P(0x303); x.fmac(Op::madd, acc, v(11), x.bc(v(0), 3), XYZ);
        P(0x304); x.fmac(Op::msub, v(13), v(11), x.bc(v(18), 0), XYZ);
        P(0x305); x.fmac(Op::mul, v(14), v(14), x.bc(v(14), 3), Y);
        P(0x306); x.fmac(Op::mul, acc, v(22), x.bc(v(0), 3), Z);
        P(0x307); x.fmac(Op::madd, v(22), v(18), x.bc(v(26), 3), Z);
        P(0x308); x.fmac(Op::mul, acc, v(13), x.bc(v(18), 1), XYZ);
        P(0x309); x.minmax(false, v(14), v(14), x.bc(v(14), 1), W);
        P(0x30a); x.fmac(Op::madd, acc, v(19), x.bc(v(0), 3), XYZ);
        P(0x30b); x.fmac(Op::msub, v(13), v(19), x.bc(v(18), 1), XYZ);
        P(0x30c); x.fmac(Op::mul, acc, v(22), v(0), W);
        P(0x30d); x.fmac(Op::madd, v(10), v(18), x.bc(v(22), 2), W);
        P(0x30e); x.fmac(Op::mul, acc, v(14), x.bc(v(21), 2), W);
        P(0x30f); x.fmac(Op::madd, v(14), v(14), v(0), W);
        P(0x310); x.fmac(Op::mul, acc, v(13), x.bc(v(1), 3), XYZ);
        P(0x311); x.fmac(Op::msub, acc, v(13), x.bc(v(9), 3), XYZ);
        P(0x312); x.fmac(Op::madd, v(13), v(1), x.bc(v(0), 3), XYZ);
        P(0x313); x.fmac(Op::mul, acc, v(10), v(21), W);
        P(0x314); x.fmac(Op::madd, v(10), v(10), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x315); x.fmac(Op::mul, acc, v(14), I(), W);
        P(0x316); x.fmac(Op::msub, v(13), v(14), x.bc(v(8), 2), W); s.i = 0x3fc00000u;  // 1.5
        P(0x317); s.i = 0x3b7f9724u;
        P(0x318); x.fmac(Op::mul, v(9), v(1), I(), W);
        P(0x319); x.fmac(Op::mul, v(13), v(13), x.bc(v(9), 3), W); s.i = 0x3fc00000u;
        P(0x31a); x.fmac(Op::sub, v(29), v(29), I(), X | Z | W);
        P(0x31b); x.minmax(true, v(13), v(13), x.bc(v(0), 0), W);
        P(0x31c); x.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x31d); x.fmac(Op::msub, v(10), v(10), v(8), W);
        P(0x31e); x.fmac(Op::mul, acc, v(29), x.bc(v(5), 1), X);
        P(0x31f); x.ftoi0(v(13), v(13), XYZW);
        P(0x320); x.fmac(Op::mul, v(10), v(10), x.bc(v(20), 0), XYZW);
        P(0x321); x.fmac(Op::madd, v(21), v(5), x.bc(v(0), 3), X);
        P(0x322); x.fmac(Op::sub, v(28), v(28), x.bc(v(0), 3), XYZW); x.sqi(10, 3);
        P(0x323); x.fmac(Op::sub, v(9), v(30), I(), XYZ);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x2bb
        P(0x324); x.sqi(13, 4);  // the delay slot
        return again;
    };
#if OMEGA_PARTICLE_NEON
    while (step(m, iteration_regs_29c, iteration)) {
    }
#else
    while (iterate(m, iteration, true)) {
    }
#endif
    if (static_cast<std::int16_t>(s.vi[6]) > 0) return 0x1cc8;
    if (static_cast<std::int16_t>(s.vi[5]) > 0) return 0x8a0;
    return 0x5e0;
}

// VU1 program 0x170 (byte 0xb80; MSCAL 0x16 of the type-0 emitter draw
// 00398590 through the table 00538080 / 003700d0), pair by pair. Like 0x29c
// every VF read waits for its FMAC result (the stall model), Q is only read
// after the WAITQ (0x1bc / 0x1d2) or >= 7 cycles after the SQRT (0x196 ->
// 0x19d), and every register it reads is set by the program or the packet.
std::uint32_t run_type0_program(Vu1State& vu, std::uint32_t top) {
    Machine m{vu};
    auto& s = vu;
    auto v = [&](int r) -> Qword& { return s.vf[r]; };
    auto I = [&] { return imm(s.i); };
    auto Q = [&] { return imm(s.q); };
    auto& acc = s.acc;
    auto P = [](std::uint32_t pair) { g_pair = pair; };
    if (v(0) != Qword{0, 0, 0, 0x3f800000u}) throw std::runtime_error("VU1 particle program: vf0 is not (0, 0, 0, 1)");

    P(0x170); m.minmax(true, v(31), v(0), bc(v(0), 3), XYZW); s.vi[12] = static_cast<std::uint16_t>(top & 0x3ff);
    P(0x171); s.vi[12] = static_cast<std::uint16_t>(s.vi[12] + 0xb);
    P(0x172); m.lq(27, s.vi[12] + 1u);
    P(0x173); s.vi[11] = static_cast<std::uint16_t>(m.mem(s.vi[12])[0]);
    P(0x174); s.vi[8] = 0x10;
    P(0x175); m.lqi(1, 8);
    P(0x176); m.fmac(Op::add, v(29), v(27), bc(v(0), 3), XYZW); m.lqi(2, 8);
    P(0x177); m.fmac(Op::add, v(30), v(27), bc(v(0), 3), XYZW); m.lqi(4, 8);
    P(0x178); m.lqi(24, 8);
    P(0x179); m.itof0(v(1), v(1), XYZ); m.lqi(5, 8);
    P(0x17a); m.lqi(6, 8);
    P(0x17b); m.lqi(7, 8);
    P(0x17c); m.lqi(8, 8);
    P(0x17d); s.i = 0x437f0000u;  // 255.0
    P(0x17e); m.fmac(Op::mul, acc, v(1), I(), XYZ); m.lqi(9, 8);
    P(0x17f); m.fmac(Op::msub, v(1), v(1), bc(v(1), 3), XYZ); m.lqi(11, 8);
    P(0x180); m.lqi(18, 8);
    P(0x181); m.lqi(19, 8);
    P(0x182); m.lqi(3, 8);
    P(0x183); s.i = 0x3b7f9724u;  // 0.0039
    P(0x184); m.fmac(Op::mul, v(1), v(1), I(), XYZ); s.vi[3] = static_cast<std::uint16_t>(s.vi[12] + 1);
    P(0x185); m.fmac(Op::add, v(27), v(27), bc(v(0), 3), XYZW); s.vi[9] = 0x7fff;
    P(0x186); m.fmac(Op::add, v(6), v(6), v(6), XYZW); s.vi[2] = s.vi[9] & s.vi[11];
    P(0x187); m.fmac(Op::add, v(8), v(8), v(8), XYZW); s.vi[4] = static_cast<std::uint16_t>(s.vi[3] + s.vi[2]);
    P(0x188); m.fmac(Op::add, v(26), v(27), v(0), W); s.i = 0x3fc00000u;  // 1.5
    P(0x189); m.ftoi0(v(19), v(19), W); m.write(v(28), v(27), XYZW);  // move
    P(0x18a); m.ftoi0(v(24), v(24), W); s.r = v(27)[0] & 0x7fffffu;    // rinit R, vf27x
    // 132vc1: one particle per iteration (iterate: speculatively on the Vita).
    const auto iteration = [&](auto& x) -> bool {
        P(0x18b); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x18c); x.fmac(Op::sub, v(29), v(29), I(), XYZW); x.mtir(5, v(19)[3]);
        P(0x18d); x.fmac(Op::sub, v(10), v(30), I(), XYZ); x.mtir(6, v(24)[3]);
        P(0x18e); x.fmac(Op::sub, v(28), v(28), I(), XYZW);
        P(0x18f); x.fmac(Op::mul, acc, v(29), v(6), XYZW);
        P(0x190); x.fmac(Op::madd, v(21), v(5), x.bc(v(0), 3), XYZW);
        P(0x191); x.fmac(Op::add, acc, v(0), x.bc(v(0), 3), X);
        P(0x192); x.fmac(Op::sub, v(26), v(26), v(0), W);
        P(0x193); x.fmac(Op::msub, v(22), v(21), v(21), X);
        P(0x194); x.outer(false, acc, v(2), v(10));
        P(0x195); x.outer(true, v(10), v(10), v(2));
        P(0x196); x.fmac(Op::mul, v(20), v(28), v(8), XYZW); x.sqrt(v(22)[0]);
        P(0x197); x.fmac(Op::mul, v(15), v(10), v(10), XYZ);
        P(0x198); x.fmac(Op::mul, v(22), v(18), v(30), W);
        P(0x199); x.fmac(Op::mul, v(23), v(26), v(7), W);
        P(0x19a); x.fmac(Op::mul, v(22), v(22), v(1), W);
        P(0x19b); x.fmac(Op::add, acc, v(15), x.bc(v(15), 1), X);
        P(0x19c); x.fmac(Op::madd, v(15), v(31), x.bc(v(15), 2), X);
        P(0x19d); x.fmac(Op::add, v(22), v(0), Q(), X);
        P(0x19e); x.fmac(Op::add, v(20), v(20), v(7), X | Y);
        P(0x19f); x.rsqrt(v(0)[3], v(15)[0]);
        {
            P(0x1a0);
            x.mr32(v(27));  // mr32
        }
        P(0x1a1); s.i = 0x40a00000u;  // 5.0
        P(0x1a2); x.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x1a3); x.fmac(Op::madd, v(27), v(31), x.bc(v(0), 3), XYZW); s.i = 0x3fc00000u;  // 1.5
        P(0x1a4); x.rxor(v(27)[0]);
        P(0x1a5); x.rget(30, X);
        P(0x1a6); x.rnext(29, X);
        P(0x1a7); x.rnext(28, X);
        P(0x1a8); x.rnext(27, X);
        P(0x1a9); x.rxor(v(27)[1]);
        P(0x1ab); x.rget(29, Y);
        P(0x1ac); x.rnext(28, Y);
        P(0x1ad); x.rnext(30, Y);
        P(0x1ae); x.rnext(27, Y);
        P(0x1af); x.rxor(v(27)[2]);
        P(0x1b1); x.rget(28, Z);
        P(0x1b2); x.rnext(30, Z);
        P(0x1b3); x.rnext(29, Z);
        P(0x1b4); x.rnext(27, Z);
        P(0x1b6); x.rxor(v(27)[3]);
        P(0x1b7); x.rget(30, W);
        P(0x1b8); x.rnext(29, W);
        P(0x1b9); x.rnext(28, W);
        P(0x1ba); x.rnext(27, W);
        P(0x1bb); x.rnext(26, W);
        P(0x1bc); x.fmac(Op::mul, acc, v(2), x.bc(v(21), 0), XYZ);  // waitq
        P(0x1bd); x.fmac(Op::mul, v(10), v(10), Q(), XYZ); s.i = 0x38d1b717u;  // 1e-4
        P(0x1be); x.fmac(Op::add, v(23), v(23), x.bc(v(7), 2), W);
        P(0x1bf); x.div(v(0)[3], v(21)[2]);
        P(0x1c1); x.fmac(Op::add, v(23), v(10), x.bc(v(0), 0), XYZ);
        P(0x1c2); x.fmac(Op::madd, v(10), v(10), x.bc(v(22), 0), XYZ);
        P(0x1c3); x.fmac(Op::mul, v(20), v(20), v(4), Y);
        P(0x1c5); x.fmac(Op::mul, v(10), v(10), x.bc(v(21), 1), XYZ);
        P(0x1c6); x.fmac(Op::mul, v(25), v(24), x.bc(v(20), 1), XYZ);
        P(0x1c7); x.fmac(Op::add, v(10), v(10), v(25), XYZ);
        P(0x1c8); x.lq(25, 15);
        P(0x1cb); x.fmac(Op::mul, acc, v(10), x.bc(v(20), 0), XYZ);
        P(0x1cc); x.fmac(Op::madd, acc, v(23), x.bc(v(23), 3), XYZ);
        P(0x1cd); x.fmac(Op::madd, v(10), v(25), x.bc(v(0), 3), XYZ);
        P(0x1ce); x.fmac(Op::sub, v(12), v(4), x.bc(v(21), 3), X);
        P(0x1cf); x.minmax(true, v(12), v(12), I(), X);
        P(0x1d0); x.minmax(false, v(12), v(12), x.bc(v(21), 2), X);
        P(0x1d1); x.fmac(Op::add, v(12), v(0), x.bc(v(12), 0), Z);
        P(0x1d3); x.fmac(Op::mul, v(12), v(12), Q(), X | Z);  // after the waitq at 0x1d2
        P(0x1d4); x.fmac(Op::sub, v(12), v(31), x.bc(v(12), 0), Y | W);
        P(0x1d5); x.fmac(Op::mul, v(14), v(9), v(12), X | Y);
        P(0x1d6); x.minmax(false, v(14), v(3), x.bc(v(14), 0), W);
        P(0x1d7); x.fmac(Op::mul, v(14), v(14), x.bc(v(14), 3), Y);
        P(0x1d8); x.minmax(false, v(14), v(14), x.bc(v(14), 1), W);
        P(0x1d9); x.fmac(Op::mul, acc, v(14), x.bc(v(20), 2), W);
        P(0x1da); x.fmac(Op::madd, v(14), v(14), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x1db); x.fmac(Op::mul, acc, v(14), I(), W);
        P(0x1dc); x.fmac(Op::msub, v(13), v(14), x.bc(v(4), 2), W);
        P(0x1dd); x.fmac(Op::mul, v(17), v(11), v(12), XYZW);
        P(0x1de); x.minmax(false, v(17), v(17), x.bc(v(0), 3), XYZW);
        P(0x1df); x.fmac(Op::mul, acc, v(9), x.bc(v(0), 3), Z | W);
        P(0x1e0); x.fmac(Op::msub, v(16), v(9), v(17), Z | W);
        P(0x1e1); x.fmac(Op::mul, acc, v(16), x.bc(v(0), 3), Z);
        P(0x1e2); x.fmac(Op::madd, v(16), v(17), x.bc(v(25), 3), Z);
        P(0x1e3); x.fmac(Op::mul, acc, v(16), v(0), W);
        P(0x1e4); x.fmac(Op::madd, v(10), v(17), x.bc(v(16), 2), W);
        P(0x1e5); x.fmac(Op::mul, acc, v(10), v(20), W);
        P(0x1e6); x.fmac(Op::madd, v(10), v(10), v(0), W);
        P(0x1e7); x.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x1e8); x.fmac(Op::msub, v(10), v(10), v(4), W);
        P(0x1e9); x.fmac(Op::mul, acc, v(3), x.bc(v(17), 0), XYZ);
        P(0x1ea); x.fmac(Op::madd, acc, v(18), x.bc(v(0), 3), XYZ);
        P(0x1eb); x.fmac(Op::msub, v(13), v(18), x.bc(v(17), 0), XYZ);
        P(0x1ec); x.fmac(Op::mul, acc, v(13), x.bc(v(17), 1), XYZ);
        P(0x1ed); x.fmac(Op::madd, acc, v(19), x.bc(v(0), 3), XYZ);
        P(0x1ee); x.fmac(Op::msub, v(13), v(19), x.bc(v(17), 1), XYZ);
        P(0x1ef); x.fmac(Op::mul, acc, v(13), x.bc(v(1), 3), XYZ);
        P(0x1f0); x.fmac(Op::msub, acc, v(13), x.bc(v(22), 3), XYZ);
        P(0x1f1); x.fmac(Op::madd, v(13), v(1), x.bc(v(0), 3), XYZ);
        P(0x1f2); x.fmac(Op::mul, v(10), v(10), x.bc(v(2), 3), XYZW);
        P(0x1f3); x.ftoi0(v(13), v(13), XYZW);
        P(0x1f4); s.i = 0x3fc00000u;  // 1.5
        P(0x1f5); x.sqi(10, 3);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x18b
        P(0x1f7); x.sqi(13, 4);  // the delay slot
        return again;
    };
#if OMEGA_PARTICLE_NEON
    while (step(m, iteration_regs_170, iteration)) {
    }
#else
    while (iterate(m, iteration, true)) {
    }
#endif
    // 0x1f8 ibgtz vi6 -> 0x204 b 0x1cc8; 0x1fa ibgtz vi5 -> 0x200 b 0x8a0; 0x1fc b 0x5e0.
    if (static_cast<std::int16_t>(s.vi[6]) > 0) return 0x1cc8;
    if (static_cast<std::int16_t>(s.vi[5]) > 0) return 0x8a0;
    return 0x5e0;
}

// Checkpoint 132cp8: VU1 program 0x208 (byte 0x1040; MSCAL 0x17 of the
// ShatterWindow draw 0039c680 through the table 00538080 / 003700d0), pair by
// pair, under the rules of 0x170: every VF read waits for its FMAC result, Q
// is only read after a WAITQ (0x22f, 0x267), and an upper op reading I in a
// LOI pair reads the previous I (none here does). Pair 0x256 has the D bit,
// a debug break only when FBRST DE1 is set; the game's only FBRST write
// (00102a58) sets RS1, so it is a no-op.
std::uint32_t run_shatter_program(Vu1State& vu, std::uint32_t top) {
    Machine m{vu};
    auto& s = vu;
    auto v = [&](int r) -> Qword& { return s.vf[r]; };
    auto I = [&] { return imm(s.i); };
    auto Q = [&] { return imm(s.q); };
    auto& acc = s.acc;
    auto P = [](std::uint32_t pair) { g_pair = pair; };
    if (v(0) != Qword{0, 0, 0, 0x3f800000u}) throw std::runtime_error("VU1 particle program: vf0 is not (0, 0, 0, 1)");

    P(0x208); m.minmax(true, v(31), v(0), bc(v(0), 3), XYZW); s.vi[12] = static_cast<std::uint16_t>(top & 0x3ff);
    P(0x209); s.vi[12] = static_cast<std::uint16_t>(s.vi[12] + 0xb);
    P(0x20a); m.lq(27, s.vi[12] + 1u);
    P(0x20b); s.vi[11] = static_cast<std::uint16_t>(m.mem(s.vi[12])[0]);
    P(0x20c); s.vi[8] = 0x10;
    P(0x20d); m.lqi(1, 8);
    P(0x20e); m.fmac(Op::add, v(29), v(27), bc(v(0), 3), XYZW); m.lqi(2, 8);
    P(0x20f); m.fmac(Op::add, v(30), v(27), bc(v(0), 3), XYZW); m.lqi(4, 8);
    P(0x210); m.lqi(24, 8);
    P(0x211); m.lqi(15, 8);
    P(0x212); m.itof0(v(1), v(1), XYZ); m.lqi(5, 8);
    P(0x213); m.lqi(6, 8);
    P(0x214); m.lqi(7, 8);
    P(0x215); m.lqi(8, 8);
    P(0x216); s.i = 0x437f0000u;  // 255.0
    P(0x217); m.fmac(Op::mul, acc, v(1), I(), XYZ); m.lqi(9, 8);
    P(0x218); m.fmac(Op::msub, v(1), v(1), bc(v(1), 3), XYZ); m.lqi(11, 8);
    P(0x219); m.lqi(18, 8);
    P(0x21a); m.lqi(19, 8);
    P(0x21b); m.lqi(3, 8);
    P(0x21c); s.i = 0x3b7f9724u;  // 0.0039
    P(0x21d); m.fmac(Op::mul, v(1), v(1), I(), XYZ); s.vi[3] = static_cast<std::uint16_t>(s.vi[12] + 1);
    P(0x21e); m.fmac(Op::add, v(27), v(27), bc(v(0), 3), XYZW); s.vi[9] = 0x7fff;
    P(0x21f); m.fmac(Op::add, v(6), v(6), v(6), XYZW); s.vi[2] = s.vi[9] & s.vi[11];
    P(0x220); m.fmac(Op::add, v(8), v(8), v(8), XYZW); s.vi[4] = static_cast<std::uint16_t>(s.vi[3] + s.vi[2]);
    P(0x221); m.fmac(Op::add, v(26), v(27), v(0), W); s.i = 0x3fc00000u;  // 1.5
    P(0x222); m.ftoi0(v(19), v(19), W); m.write(v(28), v(27), XYZW);  // move
    P(0x223); m.ftoi0(v(24), v(24), W); s.r = v(27)[0] & 0x7fffffu;    // rinit R, vf27x
    // 132vc1: one particle per iteration (iterate: speculatively on the Vita).
    const auto iteration = [&](auto& x) -> bool {
        P(0x224); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x225); x.fmac(Op::sub, v(29), v(29), I(), XYZW); x.mtir(5, v(19)[3]);
        P(0x226); x.mtir(6, v(24)[3]);
        P(0x227); x.fmac(Op::sub, v(23), v(30), I(), XYZ); x.mtir(6, v(24)[3]);
        P(0x228); x.fmac(Op::sub, v(28), v(28), I(), XYZW);
        P(0x229); x.fmac(Op::mul, acc, v(29), v(6), XYZW);
        P(0x22a); x.fmac(Op::madd, v(21), v(5), x.bc(v(0), 3), XYZW);
        P(0x22b); x.fmac(Op::add, acc, v(15), x.bc(v(15), 1), X);
        P(0x22c); x.fmac(Op::madd, v(22), v(0), x.bc(v(15), 2), X);
        P(0x22d); x.div(v(0)[3], v(22)[0]);
        P(0x22e); x.fmac(Op::mul, v(23), v(23), v(15), XYZ);
        P(0x230); x.fmac(Op::mul, v(10), v(23), Q(), XYZ);  // after the waitq at 0x22f
        P(0x231); x.fmac(Op::sub, v(26), v(26), v(0), W);
        P(0x232); x.fmac(Op::mul, v(20), v(28), v(8), XYZW);
        P(0x233); x.fmac(Op::mul, v(22), v(18), v(30), W);
        P(0x234); x.fmac(Op::mul, v(22), v(22), v(1), W);
        P(0x235); x.fmac(Op::add, v(20), v(20), v(7), X | Y);
        {
            P(0x236);
            x.mr32(v(27));  // mr32
        }
        P(0x237); s.i = 0x40a00000u;  // 5.0
        P(0x238); x.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x239); x.fmac(Op::madd, v(27), v(31), x.bc(v(0), 3), XYZW); s.i = 0x3fc00000u;  // 1.5
        P(0x23a); x.rxor(v(27)[0]);
        P(0x23b); x.rget(30, X);
        P(0x23c); x.rnext(29, X);
        P(0x23d); x.rnext(28, X);
        P(0x23e); x.rnext(27, X);
        P(0x23f); x.rxor(v(27)[1]);
        P(0x241); x.rget(29, Y);
        P(0x242); x.rnext(28, Y);
        P(0x243); x.rnext(30, Y);
        P(0x244); x.rnext(27, Y);
        P(0x245); x.rxor(v(27)[2]);
        P(0x247); x.rget(29, Z);
        P(0x248); x.rnext(28, Z);
        P(0x249); x.rnext(30, Z);
        P(0x24a); x.rnext(27, Z);
        P(0x24c); x.rxor(v(27)[3]);
        P(0x24d); x.rget(30, W);
        P(0x24e); x.rnext(29, W);
        P(0x24f); x.rnext(28, W);
        P(0x250); x.rnext(27, W);
        P(0x251); x.rnext(26, W);
        P(0x252); s.i = 0x38d1b717u;  // 1e-4
        P(0x253); x.div(v(0)[3], v(21)[2]);
        P(0x255); x.fmac(Op::mul, v(20), v(20), v(4), Y);
        P(0x257); x.fmac(Op::mul, acc, v(2), x.bc(v(21), 0), XYZ);
        P(0x258); x.fmac(Op::madd, acc, v(10), x.bc(v(0), 3), XYZ);
        P(0x259); x.fmac(Op::msub, v(10), v(10), x.bc(v(21), 0), XYZ);
        P(0x25a); x.fmac(Op::mul, v(10), v(10), x.bc(v(21), 1), XYZ);
        P(0x25b); x.fmac(Op::mul, v(25), v(24), x.bc(v(20), 1), XYZ);
        P(0x25c); x.fmac(Op::add, v(10), v(10), v(25), XYZ);
        P(0x25d); x.lq(25, 15);
        P(0x260); x.fmac(Op::mul, acc, v(10), x.bc(v(20), 0), XYZ);
        P(0x261); x.fmac(Op::madd, acc, v(23), x.bc(v(0), 3), XYZ);
        P(0x262); x.fmac(Op::madd, v(10), v(25), x.bc(v(0), 3), XYZ);
        P(0x263); x.fmac(Op::sub, v(12), v(4), x.bc(v(21), 3), X);
        P(0x264); x.minmax(true, v(12), v(12), I(), X);
        P(0x265); x.minmax(false, v(12), v(12), x.bc(v(21), 2), X);
        P(0x266); x.fmac(Op::add, v(12), v(0), x.bc(v(12), 0), Z);
        P(0x268); x.fmac(Op::mul, v(12), v(12), Q(), X | Z);  // after the waitq at 0x267
        P(0x269); x.fmac(Op::sub, v(12), v(31), x.bc(v(12), 0), Y | W);
        P(0x26a); x.fmac(Op::mul, v(14), v(9), v(12), X | Y);
        P(0x26b); x.minmax(false, v(14), v(3), x.bc(v(14), 0), W);
        P(0x26c); x.fmac(Op::mul, v(14), v(14), x.bc(v(14), 3), Y);
        P(0x26d); x.minmax(false, v(14), v(14), x.bc(v(14), 1), W);
        P(0x26e); x.fmac(Op::mul, acc, v(14), x.bc(v(20), 2), W);
        P(0x26f); x.fmac(Op::madd, v(14), v(14), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x270); x.fmac(Op::mul, acc, v(14), I(), W);
        P(0x271); x.fmac(Op::msub, v(13), v(14), x.bc(v(4), 2), W);
        P(0x272); x.fmac(Op::mul, v(17), v(11), v(12), XYZW);
        P(0x273); x.minmax(false, v(17), v(17), x.bc(v(0), 3), XYZW);
        P(0x274); x.fmac(Op::mul, acc, v(9), x.bc(v(0), 3), Z | W);
        P(0x275); x.fmac(Op::msub, v(16), v(9), v(17), Z | W);
        P(0x276); x.fmac(Op::mul, acc, v(16), x.bc(v(0), 3), Z);
        P(0x277); x.fmac(Op::madd, v(16), v(17), x.bc(v(25), 3), Z);
        P(0x278); x.fmac(Op::mul, acc, v(16), v(0), W);
        P(0x279); x.fmac(Op::madd, v(10), v(17), x.bc(v(16), 2), W);
        P(0x27a); x.fmac(Op::mul, acc, v(10), v(20), W);
        P(0x27b); x.fmac(Op::madd, v(10), v(10), v(0), W);
        P(0x27c); x.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x27d); x.fmac(Op::msub, v(10), v(10), v(4), W);
        P(0x27e); x.fmac(Op::mul, acc, v(3), x.bc(v(17), 0), XYZ);
        P(0x27f); x.fmac(Op::madd, acc, v(18), x.bc(v(0), 3), XYZ);
        P(0x280); x.fmac(Op::msub, v(13), v(18), x.bc(v(17), 0), XYZ);
        P(0x281); x.fmac(Op::mul, acc, v(13), x.bc(v(17), 1), XYZ);
        P(0x282); x.fmac(Op::madd, acc, v(19), x.bc(v(0), 3), XYZ);
        P(0x283); x.fmac(Op::msub, v(13), v(19), x.bc(v(17), 1), XYZ);
        P(0x284); x.fmac(Op::mul, acc, v(13), x.bc(v(1), 3), XYZ);
        P(0x285); x.fmac(Op::msub, v(13), v(13), x.bc(v(22), 3), XYZ);
        P(0x286); x.fmac(Op::mul, v(10), v(10), x.bc(v(2), 3), XYZW);
        P(0x287); x.ftoi0(v(13), v(13), XYZW);
        P(0x288); s.i = 0x3fc00000u;  // 1.5
        P(0x289); x.sqi(10, 3);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x224
        P(0x28b); x.sqi(13, 4);  // the delay slot
        return again;
    };
#if OMEGA_PARTICLE_NEON
    while (step(m, iteration_regs_208, iteration)) {
    }
#else
    while (iterate(m, iteration, true)) {
    }
#endif
    // 0x28c ibgtz vi6 -> 0x298 b 0x19a8; 0x28e ibgtz vi5 -> 0x294 b 0x8a0; 0x290 b 0x5e0.
    if (static_cast<std::int16_t>(s.vi[6]) > 0) return 0x19a8;
    if (static_cast<std::int16_t>(s.vi[5]) > 0) return 0x8a0;
    return 0x5e0;
}

std::uint32_t cull_batch(const Qword& bound_min, const Qword& bound_max, const std::array<Qword, 4>& cull) {
    if ((bound_max[0] & 0xffffu) == 0xffffu) return 0;  // pair 0xd: ibeq vi1, 0xffff
    Vu1State& state = thread_vu1_state();
    Machine m{state};
    auto& s = state;
    for (int k = 0; k < 4; ++k) s.vf[1 + k] = cull[k];
    s.vf[10] = bound_min;
    s.vf[11] = bound_max;
    // Pairs 0x3b..0x61: the corner components (vf10 = min, vf11 = max) in program order.
    static constexpr int corners[8][3] = {{10, 10, 10}, {11, 11, 11}, {10, 10, 11}, {10, 11, 10},
                                          {10, 11, 11}, {11, 10, 10}, {11, 10, 11}, {11, 11, 10}};
    std::uint32_t all = 0x3f, any = 0, below = 0;
    // 132vc1: the corners speculatively on the Vita (they write only the ACC, read before written).
    const auto clip_corners = [&](auto& x) {
        all = 0x3f, any = 0, below = 0;
        for (int c = 0; c < 8; ++c) {
            g_pair = 0x3b + 5 * c;
            Qword clip{};
            x.fmac(Op::mul, s.acc, s.vf[1], x.bc(s.vf[corners[c][0]], 0), XYZW);
            x.fmac(Op::madd, s.acc, s.vf[2], x.bc(s.vf[corners[c][1]], 1), XYZW);
            x.fmac(Op::madd, s.acc, s.vf[3], x.bc(s.vf[corners[c][2]], 2), XYZW);
            x.fmac(Op::madd, clip, s.vf[4], x.bc(s.vf[0], 3), XYZW);
            const double w = std::fabs(daz(clip[3]));
            std::uint32_t j = 0;
            for (int a = 0; a < 3; ++a) {
                const double x = daz(clip[a]);
                if (x > w) j |= 1u << (2 * a);
                if (x < -w) j |= 2u << (2 * a);
            }
            all &= j;  // iand vi3, vi3, vi1 (fcget)
            any |= j;  // ior vi5, vi5, vi1
            below += (j & 0x20) ? 1 : 0;  // fcand vi1, 0x20; iadd vi4, vi4, vi1
        }
    };
#if OMEGA_PARTICLE_NEON && !OMEGA_PARTICLE_NEON_CHECK
    bool done = false;
    if (m.neon && g_speculate.load(std::memory_order_relaxed)) {
        SpecMachine x{{s, true}};
        clip_corners(x);
        done = x.clean();
        if (!done) g_neon_rollbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (!done) clip_corners(m);
#else
    clip_corners(m);
#endif
    if (all & 0x3f) return 0;  // pair 0x70: iblez vi3 accepts only vi3 = 0
    return below ? 3 : (any & 0x3f) ? 2 : 1;
}

Qword sprite_scale_qword(const float (&projection)[16], float a, float b) {
    const float ia = 1.0f / a, ib = 1.0f / b;  // 00377dd4 / 00377de0
    return {std::bit_cast<std::uint32_t>(projection[0] * ia), std::bit_cast<std::uint32_t>(projection[5] * ib),
            std::bit_cast<std::uint32_t>(projection[10]), std::bit_cast<std::uint32_t>(projection[15])};
}

std::vector<Qword> run_sprite_program(std::uint32_t stop, const std::vector<Qword>& position,
                                      const std::vector<Qword>& colour, const SpriteFrame& frame) {
    std::vector<Qword> out;
    run_sprite_program_into(out, stop, position, colour, frame);
    return out;
}

void run_sprite_program_into(std::vector<Qword>& out, std::uint32_t stop, const std::vector<Qword>& position,
                             const std::vector<Qword>& colour, const SpriteFrame& frame) {
    if (stop != 0x5e0 && stop != 0x8a0) fail(stop / 8, "a sprite program other than 0x5e0 / 0x8a0");
    if (position.size() != colour.size()) throw std::runtime_error("VU1 sprite program: position/colour counts differ");
    const bool flipped = stop == 0x8a0;
    const std::uint32_t base = flipped ? 0x116 : 0xbe;
    Vu1State& state = thread_vu1_state();
    Machine m{state};
    auto& s = state;
    auto v = [&](int r) -> Qword& { return s.vf[r]; };
    auto& acc = s.acc;
    auto P = [&](std::uint32_t pair) { g_pair = pair; };
    // The setup routine (pairs 0x7b..0x82): vf1..vf6 from data 5..0xa.
    for (int k = 0; k < 4; ++k) v(1 + k) = frame.clip[k];
    v(5) = frame.scale;
    v(6) = frame.offset;
    // Loop invariants (0x5e0 pairs 0xbe..0xc9; 0x8a0 0x116..0x123).
    P(base); m.fmac(Op::sub, v(30), v(0), v(0), XYZW);
    m.fmac(Op::sub, v(11), v(0), v(0), XYZW);
    m.fmac(Op::sub, v(12), v(0), v(0), XYZW);
    m.write(v(8), frame.sprite_scale, X | Y);  // lq.xy vf8, 0xe(vi0)
    if (!flipped) {
        P(0xc4); m.fmac(Op::add, v(11), v(11), bc(v(0), 3), Z);
        P(0xc5); m.fmac(Op::add, v(12), v(12), bc(v(0), 3), XYZ);
    } else {
        P(0x11c); m.fmac(Op::add, v(11), v(11), bc(v(0), 3), XYZW);
        P(0x11d); m.fmac(Op::add, v(12), v(12), bc(v(0), 3), XYZW);
    }
    P(base + 8); m.fmac(Op::mul, v(8), v(8), v(5), X | Y);
    m.fmac(Op::sub, v(8), v(0), v(0), Z);
    if (flipped) {
        P(0x120); m.fmac(Op::sub, v(11), v(11), bc(v(0), 3), X | Y);
        P(0x121); m.fmac(Op::sub, v(12), v(12), bc(v(0), 3), Z | W);
    }
    s.i = 0xbf800000u;  // loi -1.0
    P(flipped ? 0x123 : 0xc9); m.fmac(Op::mul, v(8), v(8), imm(s.i), Y);
    out.clear();
    out.reserve(position.size() * 6);
    for (std::size_t k = 0; k < position.size(); ++k) {
        // 132vc1: one sprite per iteration (iterate: speculatively on the Vita).
        const auto particle = [&](auto& x) -> bool {
            v(15) = position[k];
            v(23) = colour[k];
            P(flipped ? 0x128 : 0xce); x.fmac(Op::mul, v(9), v(8), x.bc(v(15), 3), XYZ);
            x.fmac(Op::mul, acc, v(1), x.bc(v(15), 0), XYZW);
            x.fmac(Op::madd, acc, v(2), x.bc(v(15), 1), XYZW);
            x.fmac(Op::madd, acc, v(3), x.bc(v(15), 2), XYZW);
            x.fmac(Op::madd, v(16), v(4), x.bc(v(0), 3), XYZW);
            x.fmac(Op::add, v(10), v(9), v(0), XYZ);
            // CLIPW.xyz vf16, vf16w; FCAND vi1, 0x3f; IADD vi9, vi1, vi10 (0x7fff).
            const double w = std::fabs(daz(v(16)[3]));
            bool clipped = false;
            for (int c = 0; c < 3; ++c) {
                const double x = daz(v(16)[c]);
                clipped = clipped || x > w || x < -w;
            }
            const std::uint32_t adc = clipped ? 0xffff8000u : 0x00007fffu;  // MFIR sign-extends vi9
            x.div(v(0)[3], v(16)[3]);
            x.fmac(Op::mul, v(17), v(16), v(5), XYZ);
            x.fmac(Op::add, acc, v(6), v(0), XYZ);
            x.fmac(Op::madd, v(18), v(17), imm(s.q), XYZ);
            x.fmac(Op::mul, v(10), v(10), imm(s.q), X | Y);
            if (flipped) {
                P(0x138); x.write(v(13), {0, 0, 0x3f800000u, 0}, Z | W);  // mr32.zw vf13, vf0
                x.write(v(14), {0, 0, 0x3f800000u, 0}, Z | W);
                x.fmac(Op::add, v(13), v(11), v(30), X | Y);
                x.fmac(Op::add, v(14), v(12), v(30), X | Y);
                x.fmac(Op::mul, v(26), v(13), imm(s.q), XYZW);
                x.fmac(Op::mul, v(25), v(14), imm(s.q), XYZW);
            } else {
                x.fmac(Op::mul, v(25), v(11), imm(s.q), XYZW);
                x.fmac(Op::mul, v(26), v(12), imm(s.q), XYZW);
            }
            x.fmac(Op::sub, v(19), v(18), v(10), XYZ);
            x.fmac(Op::add, v(20), v(18), v(10), XYZ);
            x.ftoi0(v(21), v(19), XYZ);
            x.ftoi0(v(22), v(20), XYZ);
            v(21)[3] = adc;
            v(22)[3] = adc;
            for (int r : {25, 23, 21, 26, 23, 22}) out.push_back(v(r));
            if (flipped) {  // mr32.xyzw vf11 / vf12 (x <- y, y <- z, z <- w, w <- x)
                P(0x140); v(11) = {v(11)[1], v(11)[2], v(11)[3], v(11)[0]};
                P(0x144); v(12) = {v(12)[1], v(12)[2], v(12)[3], v(12)[0]};
            }
            return true;
        };
#if OMEGA_PARTICLE_NEON
        // 132vc1 hw5: the register-resident sprite first (refused: the sprite on iterate() as before). The check build
        // runs both and compares the state and the output whenever the register-resident sprite was taken.
        if (m.neon && g_speculate.load(std::memory_order_relaxed) && g_regs.load(std::memory_order_relaxed)) {
#if OMEGA_PARTICLE_NEON_CHECK
            auto copy = std::make_unique<Vu1State>(s);
            std::vector<Qword> regs_out = out;
            const bool took = sprite_regs(*copy, position[k], colour[k], flipped, regs_out);
            iterate(m, particle, false, &out);
            if (took && (std::memcmp(copy.get(), &s, sizeof s) != 0 || regs_out != out))
                spec_mismatch("a register-resident sprite");
            continue;
#else
            if (sprite_regs(s, position[k], colour[k], flipped, out)) continue;
#endif
        }
#endif
        iterate(m, particle, false, &out);
    }
}

Qword heat_haze_clip_qword(float a, float b) {
    // 0037d0f4 / 0037d128: 0.5 * stack a, -0.5 * stack b; 0037d118 / 0037d11c: 1.0, 1.0.
    return {std::bit_cast<std::uint32_t>(0.5f * a), std::bit_cast<std::uint32_t>(-0.5f * b), 0x3f800000u, 0x3f800000u};
}

Qword heat_haze_uv_qword(std::int32_t width, std::int32_t height) {
    // 0037d120 / 0037d124: 0.5, 0.5; 0037d114 / 0037d104: 1.0 / float(W << 4), 1.0 / float(H << 4).
    const float w = static_cast<float>(width << 4), h = static_cast<float>(height << 4);
    return {0x3f000000u, 0x3f000000u, std::bit_cast<std::uint32_t>(1.0f / w), std::bit_cast<std::uint32_t>(1.0f / h)};
}

std::vector<Qword> run_heat_haze_program(const std::vector<Qword>& position, const std::vector<Qword>& colour,
                                         const SpriteFrame& frame, const Qword& tag) {
    std::vector<Qword> out;
    run_heat_haze_program_into(out, position, colour, frame, tag);
    return out;
}

void run_heat_haze_program_into(std::vector<Qword>& out, const std::vector<Qword>& position,
                                const std::vector<Qword>& colour, const SpriteFrame& frame, const Qword& tag) {
    if (position.size() != colour.size()) throw std::runtime_error("VU1 heat-haze program: position/colour counts differ");
    Vu1State& state = thread_vu1_state();
    Machine m{state};
    auto& s = state;
    auto v = [&](int r) -> Qword& { return s.vf[r]; };
    auto& acc = s.acc;
    auto P = [&](std::uint32_t pair) { g_pair = pair; };
    // The setup routine (pairs 0x7b..0x82): vf1..vf6 from data 5..0xa; vf9 = 0.
    for (int k = 0; k < 4; ++k) v(1 + k) = frame.clip[k];
    v(5) = frame.scale;
    v(6) = frame.offset;
    // Loop invariants, pairs 0x39e..0x3b2.
    P(0x39e); m.fmac(Op::sub, v(27), v(0), v(0), XYZW);
    v(10) = frame.haze_clip;  // lq vf10, 0x2c(vi0)
    P(0x39f); m.fmac(Op::sub, v(28), v(0), v(0), XYZW);
    v(11) = frame.haze_uv;    // lq vf11, 0x2d(vi0)
    P(0x3a0); m.fmac(Op::add, v(31), v(0), bc(v(0), 3), XYZ);
    m.write(v(8), frame.sprite_scale, X | Y);  // lq.xy vf8, 0xe(vi0)
    P(0x3a2); m.fmac(Op::add, v(27), v(27), bc(v(10), 2), X);
    P(0x3a3); m.fmac(Op::add, v(27), v(27), bc(v(10), 3), Y);
    P(0x3a4); m.fmac(Op::add, v(28), v(28), bc(v(11), 2), X);
    P(0x3a5); m.fmac(Op::add, v(28), v(28), bc(v(11), 3), Y);
    P(0x3a6); m.fmac(Op::mul, v(8), v(8), v(5), X | Y);
    const Qword zw10{0, 0, 0x3f800000u, 0};  // mr32.zw vf25 / vf26, vf0
    m.write(v(26), zw10, Z | W);
    P(0x3a7); m.fmac(Op::mul, v(27), v(27), v(8), X | Y);
    m.write(v(25), zw10, Z | W);
    P(0x3a8); m.fmac(Op::mul, v(28), v(28), v(8), X | Y);
    P(0x3a9); m.fmac(Op::sub, v(7), v(0), v(0), XYZW);
    v(29) = tag;              // lq vf29, 3(vi12): TOP + 5, def+0xd0
    P(0x3aa); m.fmac(Op::sub, v(12), v(0), v(0), XYZW);
    s.i = 0x4a824824u;        // loi 4269074.0
    P(0x3ab); m.fmac(Op::sub, v(24), v(0), v(0), XYZW);
    P(0x3ad); m.fmac(Op::add, v(7), v(7), imm(s.i), Z);
    v(30) = {v(29)[1], v(29)[2], v(29)[3], v(29)[0]};  // mr32 vf30, vf29
    P(0x3af); m.fmac(Op::add, v(12), v(12), bc(v(0), 3), Z);
    P(0x3b0); m.fmac(Op::add, v(24), v(24), bc(v(0), 3), XYZ);
    P(0x3b1); m.ftoi0(v(29), v(7), Z | W);
    v(30) = {v(30)[1], v(30)[2], v(30)[3], v(30)[0]};  // mr32 vf30, vf30
    P(0x3b2); m.ftoi0(v(30), v(7), Z | W);
    s.i = 0x3c800000u;        // loi 1 / 64
    out.clear();
    out.reserve(position.size() * 14);
    // The clip-flag register (fcset 0 in the setup): CLIPW shifts a six-bit
    // judgment in per particle; FCAND 0x3ffff reads this one and the two before.
    std::uint32_t clip_flags = 0, next_flags = 0;
    for (std::size_t k = 0; k < position.size(); ++k) {
        // 132vc1: one particle per iteration (iterate: speculatively on the Vita).
        const auto particle = [&](auto& x) -> bool {
            v(15) = position[k];
            v(23) = colour[k];
            P(0x3b7); x.fmac(Op::mul, acc, v(1), x.bc(v(15), 0), XYZW);
            x.fmac(Op::madd, acc, v(2), x.bc(v(15), 1), XYZW);
            x.fmac(Op::madd, acc, v(3), x.bc(v(15), 2), XYZW);
            x.fmac(Op::madd, v(16), v(4), x.bc(v(0), 3), XYZW);
            P(0x3bb); x.fmac(Op::mul, v(7), v(27), x.bc(v(15), 3), X | Y);
            P(0x3bc); x.fmac(Op::mul, v(8), v(28), x.bc(v(15), 3), X | Y);
            P(0x3bf);
            const double w = std::fabs(daz(v(16)[3]));
            std::uint32_t j = 0;
            for (int c = 0; c < 3; ++c) {
                const double x = daz(v(16)[c]);
                if (x > w) j |= 1u << (2 * c);
                if (x < -w) j |= 2u << (2 * c);
            }
            x.div(v(0)[3], v(16)[3]);
            P(0x3c0); x.fmac(Op::mul, v(17), v(16), v(5), XYZ);
            P(0x3c1); x.fmac(Op::mul, v(13), v(16), v(10), X | Y);
            const std::uint32_t flags = ((clip_flags << 6) | j) & 0xffffffu;
            // FCAND vi1, 0x3ffff; IADD vi1, vi1, vi10 (0x7fff); MFIR sign-extends.
            const std::uint32_t adc = (flags & 0x3ffffu) ? 0xffff8000u : 0x00007fffu;
            P(0x3c5); x.fmac(Op::add, acc, v(6), v(0), XYZ);
            P(0x3c6); x.fmac(Op::madd, v(18), v(17), imm(s.q), XYZ);
            P(0x3c7); x.fmac(Op::add, acc, v(11), v(0), X | Y);
            P(0x3c8); x.fmac(Op::madd, v(14), v(13), imm(s.q), X | Y);
            P(0x3c9); x.fmac(Op::mul, v(7), v(7), imm(s.q), X | Y);
            P(0x3ca); x.fmac(Op::mul, acc, v(8), imm(s.q), X | Y);
            P(0x3cb); x.fmac(Op::madd, v(8), v(31), imm(s.i), X | Y);
            P(0x3cc); x.fmac(Op::mul, v(9), v(12), imm(s.q), XYZW);
            x.write(v(19), v(18), Z);
            P(0x3cd); x.fmac(Op::mul, v(13), v(24), imm(s.q), XYZW);
            x.write(v(20), v(18), Z);
            P(0x3ce); x.fmac(Op::sub, v(19), v(18), v(7), X | Y);
            P(0x3cf); x.fmac(Op::add, v(20), v(18), v(7), X | Y);
            P(0x3d0); x.fmac(Op::sub, v(25), v(14), v(8), X | Y);
            P(0x3d1); x.fmac(Op::add, v(26), v(14), v(8), X | Y);
            P(0x3d2); x.ftoi0(v(21), v(19), XYZ);
            P(0x3d3); x.ftoi0(v(22), v(20), XYZ);
            v(21)[3] = adc;
            v(22)[3] = adc;
            next_flags = flags;
            for (int r : {29, 9, 23, 21, 13, 23, 22, 30, 25, 23, 21, 26, 23, 22}) out.push_back(v(r));
            return true;
        };
        iterate(m, particle, false, &out);
        clip_flags = next_flags;
    }
    // Pairs 0x3e5..0x3e7: isw.x 0x8001 (the EOP bit) into the last GIF tag, -7(vi8).
    if (!out.empty()) out[out.size() - 7][0] = 0x8001u;
}

}  // namespace omega::particle
