#include "particle_vu1.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
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

}  // namespace

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
    for (;;) {
        P(0x2bb); m.div(v(4)[3], v(21)[0]);
        P(0x2bc); m.fmac(Op::mul, v(28), v(28), v(4), Y | Z); m.write(v(25), v(28), W);
        P(0x2bd); m.outer(false, acc, v(2), v(9)); m.mtir(5, v(2)[3]);
        P(0x2be); m.outer(true, v(9), v(9), v(2)); m.mtir(6, v(20)[2]);
        P(0x2bf); m.fmac(Op::mul, v(21), v(29), v(5), Z | W); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x2c0); m.fmac(Op::add, acc, v(0), bc(v(4), 0), X | Y);
        P(0x2c1); m.fmac(Op::msub, acc, v(31), bc(v(28), 2), X | Y);
        P(0x2c2); m.fmac(Op::madd, v(12), v(31), Q(), X | Y);
        P(0x2c3); m.fmac(Op::mul, v(26), v(9), v(9), XYZ); s.i = 0x40a00000u;  // 5.0
        P(0x2c4); m.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x2c5); m.fmac(Op::madd, v(27), v(31), bc(v(0), 3), XYZW);
        P(0x2c6); m.ftoi0(v(22), v(12), X | Y);
        P(0x2c7); m.fmac(Op::add, acc, v(26), bc(v(26), 1), X);
        P(0x2c8); m.fmac(Op::madd, v(26), v(31), bc(v(26), 2), X);
        P(0x2c9); m.itof0(v(22), v(22), X | Y); s.i = 0x40400000u;  // 3.0
        P(0x2ca); m.fmac(Op::mul, acc, v(12), I(), X | Y);
        {
            const Qword o = v(27);
            m.write(v(27), Qword{o[1], o[2], o[3], o[0]}, XYZW);  // mr32
        }
        P(0x2cb); m.fmac(Op::sub, v(29), v(29), bc(v(0), 3), Y); m.rsqrt(v(31)[0], v(26)[0]);
        P(0x2cc); m.fmac(Op::add, v(18), v(0), bc(v(8), 1), XYZ);
        P(0x2cd); m.fmac(Op::msub, v(25), v(22), I(), X | Y);
        P(0x2ce); m.fmac(Op::sub, v(12), v(12), v(22), X | Y);
        P(0x2cf); m.fmac(Op::mul, v(9), v(11), bc(v(29), 1), W);
        P(0x2d0); m.fmac(Op::mul, acc, v(18), bc(v(8), 0), XYZ);
        P(0x2d1); m.ftoi0(v(25), v(25), Y);
        P(0x2d2); m.fmac(Op::madd, v(18), v(18), bc(v(12), 0), XYZ);
        P(0x2d3); m.fmac(Op::sub, v(12), v(31), bc(v(28), 1), Y | Z);
        P(0x2d5); m.mtir(9, v(25)[1]); m.itof0(v(25), v(25), Y);  // the lower reads vf25.y first
        P(0x2d6); m.fmac(Op::mul, v(18), v(18), bc(v(18), 2), X | Y);
        P(0x2d8); m.fmac(Op::mul, v(9), v(9), Q(), XYZ); s.i = 0x38d1b717u;  // 1e-4
        P(0x2d9); m.fmac(Op::sub, v(25), v(25), bc(v(25), 1), X);
        {
            const std::uint32_t qa = (0x1eu + s.vi[9]) & 0x3ff;
            if (qa < 0x16 || qa > 0x20) fail(0x2d9, "LQ outside the uploaded definition block");
            m.lq(26, qa);
        }
        P(0x2da); m.fmac(Op::mul, v(18), v(18), bc(v(18), 2), X); m.div(v(0)[3], v(12)[1]);
        P(0x2db); m.fmac(Op::sub, v(22), v(19), bc(v(22), 0), W);
        P(0x2dc); m.minmax(false, v(12), v(12), bc(v(22), 3), X);
        P(0x2dd); m.fmac(Op::mul, v(9), v(9), v(1), W);
        P(0x2e0); m.minmax(true, v(12), v(12), I(), X); m.rxor(v(27)[0]);
        P(0x2e1); m.rget(27, X);
        P(0x2e2); m.fmac(Op::mul, acc, v(31), bc(v(26), 0), Y); m.rnext(29, X);
        P(0x2e3); m.fmac(Op::mul, acc, v(31), bc(v(26), 2), W); m.rnext(28, X);
        P(0x2e4); m.fmac(Op::madd, v(26), v(26), bc(v(25), 3), Y | W); m.rnext(30, X);
        P(0x2e5); m.minmax(false, v(12), v(12), bc(v(12), 1), X); m.rxor(v(27)[1]);
        P(0x2e6); m.fmac(Op::mul, v(22), v(18), v(15), XYZW); m.rget(29, Y);
        P(0x2e7); m.fmac(Op::mul, v(23), v(18), v(16), XYZW); m.rnext(28, Y);
        P(0x2e8); m.fmac(Op::add, acc, v(0), bc(v(26), 1), X); m.rnext(27, Y);
        P(0x2e9); m.fmac(Op::msub, acc, v(25), bc(v(26), 1), X); m.rnext(30, Y);
        P(0x2ea); m.fmac(Op::madd, v(26), v(25), bc(v(26), 3), X); m.rxor(v(27)[2]);
        P(0x2eb); m.fmac(Op::add, v(12), v(0), bc(v(12), 0), Z); m.rget(28, Z);
        P(0x2ec); m.fmac(Op::mul, v(24), v(18), v(17), XYZW); m.rnext(27, Z);
        P(0x2ed); m.rnext(29, Z);
        P(0x2ee); m.fmac(Op::mul, v(9), v(9), bc(v(26), 0), XYZ); m.rnext(30, Z);
        P(0x2ef); m.fmac(Op::mul, v(12), v(12), Q(), X | Z);
        P(0x2f0); m.fmac(Op::add, acc, v(22), bc(v(22), 1), X); m.rxor(v(27)[3]);
        P(0x2f1); m.fmac(Op::madd, acc, v(31), bc(v(22), 2), X); m.rget(27, W);
        P(0x2f2); m.fmac(Op::madd, v(10), v(31), bc(v(22), 3), X); m.rnext(29, W);
        P(0x2f3); m.fmac(Op::sub, v(12), v(31), bc(v(12), 0), Y | W); m.rnext(28, W);
        P(0x2f4); m.fmac(Op::mul, v(18), v(7), v(12), XYZW);
        P(0x2f5); m.fmac(Op::add, acc, v(23), bc(v(23), 0), Y);
        P(0x2f6); m.fmac(Op::madd, acc, v(31), bc(v(23), 2), Y);
        P(0x2f7); m.fmac(Op::madd, v(10), v(31), bc(v(23), 3), Y);
        P(0x2f8); m.fmac(Op::mul, v(14), v(6), v(12), X | Y);
        P(0x2f9); m.minmax(false, v(18), v(18), bc(v(0), 3), XYZW); m.rnext(30, W);
        P(0x2fa); m.fmac(Op::add, acc, v(24), bc(v(24), 0), Z);
        P(0x2fb); m.fmac(Op::madd, acc, v(31), bc(v(24), 1), Z);
        P(0x2fc); m.fmac(Op::madd, v(10), v(31), bc(v(24), 3), Z); m.lq(26, 15);
        P(0x2fd); m.fmac(Op::mul, acc, v(6), bc(v(0), 3), Z | W);
        P(0x2fe); m.fmac(Op::msub, v(22), v(6), v(18), Z | W);
        P(0x2ff); m.minmax(false, v(14), v(3), bc(v(14), 0), W);
        P(0x300); m.fmac(Op::add, acc, v(10), v(26), XYZ);
        P(0x301); m.fmac(Op::madd, v(10), v(31), v(9), XYZ);
        P(0x302); m.fmac(Op::mul, acc, v(3), bc(v(18), 0), XYZ);
        P(0x303); m.fmac(Op::madd, acc, v(11), bc(v(0), 3), XYZ);
        P(0x304); m.fmac(Op::msub, v(13), v(11), bc(v(18), 0), XYZ);
        P(0x305); m.fmac(Op::mul, v(14), v(14), bc(v(14), 3), Y);
        P(0x306); m.fmac(Op::mul, acc, v(22), bc(v(0), 3), Z);
        P(0x307); m.fmac(Op::madd, v(22), v(18), bc(v(26), 3), Z);
        P(0x308); m.fmac(Op::mul, acc, v(13), bc(v(18), 1), XYZ);
        P(0x309); m.minmax(false, v(14), v(14), bc(v(14), 1), W);
        P(0x30a); m.fmac(Op::madd, acc, v(19), bc(v(0), 3), XYZ);
        P(0x30b); m.fmac(Op::msub, v(13), v(19), bc(v(18), 1), XYZ);
        P(0x30c); m.fmac(Op::mul, acc, v(22), v(0), W);
        P(0x30d); m.fmac(Op::madd, v(10), v(18), bc(v(22), 2), W);
        P(0x30e); m.fmac(Op::mul, acc, v(14), bc(v(21), 2), W);
        P(0x30f); m.fmac(Op::madd, v(14), v(14), v(0), W);
        P(0x310); m.fmac(Op::mul, acc, v(13), bc(v(1), 3), XYZ);
        P(0x311); m.fmac(Op::msub, acc, v(13), bc(v(9), 3), XYZ);
        P(0x312); m.fmac(Op::madd, v(13), v(1), bc(v(0), 3), XYZ);
        P(0x313); m.fmac(Op::mul, acc, v(10), v(21), W);
        P(0x314); m.fmac(Op::madd, v(10), v(10), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x315); m.fmac(Op::mul, acc, v(14), I(), W);
        P(0x316); m.fmac(Op::msub, v(13), v(14), bc(v(8), 2), W); s.i = 0x3fc00000u;  // 1.5
        P(0x317); s.i = 0x3b7f9724u;
        P(0x318); m.fmac(Op::mul, v(9), v(1), I(), W);
        P(0x319); m.fmac(Op::mul, v(13), v(13), bc(v(9), 3), W); s.i = 0x3fc00000u;
        P(0x31a); m.fmac(Op::sub, v(29), v(29), I(), X | Z | W);
        P(0x31b); m.minmax(true, v(13), v(13), bc(v(0), 0), W);
        P(0x31c); m.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x31d); m.fmac(Op::msub, v(10), v(10), v(8), W);
        P(0x31e); m.fmac(Op::mul, acc, v(29), bc(v(5), 1), X);
        P(0x31f); m.ftoi0(v(13), v(13), XYZW);
        P(0x320); m.fmac(Op::mul, v(10), v(10), bc(v(20), 0), XYZW);
        P(0x321); m.fmac(Op::madd, v(21), v(5), bc(v(0), 3), X);
        P(0x322); m.fmac(Op::sub, v(28), v(28), bc(v(0), 3), XYZW); m.sqi(10, 3);
        P(0x323); m.fmac(Op::sub, v(9), v(30), I(), XYZ);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x2bb
        P(0x324); m.sqi(13, 4);  // the delay slot
        if (!again) break;
    }
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
    for (;;) {
        P(0x18b); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x18c); m.fmac(Op::sub, v(29), v(29), I(), XYZW); m.mtir(5, v(19)[3]);
        P(0x18d); m.fmac(Op::sub, v(10), v(30), I(), XYZ); m.mtir(6, v(24)[3]);
        P(0x18e); m.fmac(Op::sub, v(28), v(28), I(), XYZW);
        P(0x18f); m.fmac(Op::mul, acc, v(29), v(6), XYZW);
        P(0x190); m.fmac(Op::madd, v(21), v(5), bc(v(0), 3), XYZW);
        P(0x191); m.fmac(Op::add, acc, v(0), bc(v(0), 3), X);
        P(0x192); m.fmac(Op::sub, v(26), v(26), v(0), W);
        P(0x193); m.fmac(Op::msub, v(22), v(21), v(21), X);
        P(0x194); m.outer(false, acc, v(2), v(10));
        P(0x195); m.outer(true, v(10), v(10), v(2));
        P(0x196); m.fmac(Op::mul, v(20), v(28), v(8), XYZW); m.sqrt(v(22)[0]);
        P(0x197); m.fmac(Op::mul, v(15), v(10), v(10), XYZ);
        P(0x198); m.fmac(Op::mul, v(22), v(18), v(30), W);
        P(0x199); m.fmac(Op::mul, v(23), v(26), v(7), W);
        P(0x19a); m.fmac(Op::mul, v(22), v(22), v(1), W);
        P(0x19b); m.fmac(Op::add, acc, v(15), bc(v(15), 1), X);
        P(0x19c); m.fmac(Op::madd, v(15), v(31), bc(v(15), 2), X);
        P(0x19d); m.fmac(Op::add, v(22), v(0), Q(), X);
        P(0x19e); m.fmac(Op::add, v(20), v(20), v(7), X | Y);
        P(0x19f); m.rsqrt(v(0)[3], v(15)[0]);
        {
            P(0x1a0);
            const Qword o = v(27);
            m.write(v(27), Qword{o[1], o[2], o[3], o[0]}, XYZW);  // mr32
        }
        P(0x1a1); s.i = 0x40a00000u;  // 5.0
        P(0x1a2); m.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x1a3); m.fmac(Op::madd, v(27), v(31), bc(v(0), 3), XYZW); s.i = 0x3fc00000u;  // 1.5
        P(0x1a4); m.rxor(v(27)[0]);
        P(0x1a5); m.rget(30, X);
        P(0x1a6); m.rnext(29, X);
        P(0x1a7); m.rnext(28, X);
        P(0x1a8); m.rnext(27, X);
        P(0x1a9); m.rxor(v(27)[1]);
        P(0x1ab); m.rget(29, Y);
        P(0x1ac); m.rnext(28, Y);
        P(0x1ad); m.rnext(30, Y);
        P(0x1ae); m.rnext(27, Y);
        P(0x1af); m.rxor(v(27)[2]);
        P(0x1b1); m.rget(28, Z);
        P(0x1b2); m.rnext(30, Z);
        P(0x1b3); m.rnext(29, Z);
        P(0x1b4); m.rnext(27, Z);
        P(0x1b6); m.rxor(v(27)[3]);
        P(0x1b7); m.rget(30, W);
        P(0x1b8); m.rnext(29, W);
        P(0x1b9); m.rnext(28, W);
        P(0x1ba); m.rnext(27, W);
        P(0x1bb); m.rnext(26, W);
        P(0x1bc); m.fmac(Op::mul, acc, v(2), bc(v(21), 0), XYZ);  // waitq
        P(0x1bd); m.fmac(Op::mul, v(10), v(10), Q(), XYZ); s.i = 0x38d1b717u;  // 1e-4
        P(0x1be); m.fmac(Op::add, v(23), v(23), bc(v(7), 2), W);
        P(0x1bf); m.div(v(0)[3], v(21)[2]);
        P(0x1c1); m.fmac(Op::add, v(23), v(10), bc(v(0), 0), XYZ);
        P(0x1c2); m.fmac(Op::madd, v(10), v(10), bc(v(22), 0), XYZ);
        P(0x1c3); m.fmac(Op::mul, v(20), v(20), v(4), Y);
        P(0x1c5); m.fmac(Op::mul, v(10), v(10), bc(v(21), 1), XYZ);
        P(0x1c6); m.fmac(Op::mul, v(25), v(24), bc(v(20), 1), XYZ);
        P(0x1c7); m.fmac(Op::add, v(10), v(10), v(25), XYZ);
        P(0x1c8); m.lq(25, 15);
        P(0x1cb); m.fmac(Op::mul, acc, v(10), bc(v(20), 0), XYZ);
        P(0x1cc); m.fmac(Op::madd, acc, v(23), bc(v(23), 3), XYZ);
        P(0x1cd); m.fmac(Op::madd, v(10), v(25), bc(v(0), 3), XYZ);
        P(0x1ce); m.fmac(Op::sub, v(12), v(4), bc(v(21), 3), X);
        P(0x1cf); m.minmax(true, v(12), v(12), I(), X);
        P(0x1d0); m.minmax(false, v(12), v(12), bc(v(21), 2), X);
        P(0x1d1); m.fmac(Op::add, v(12), v(0), bc(v(12), 0), Z);
        P(0x1d3); m.fmac(Op::mul, v(12), v(12), Q(), X | Z);  // after the waitq at 0x1d2
        P(0x1d4); m.fmac(Op::sub, v(12), v(31), bc(v(12), 0), Y | W);
        P(0x1d5); m.fmac(Op::mul, v(14), v(9), v(12), X | Y);
        P(0x1d6); m.minmax(false, v(14), v(3), bc(v(14), 0), W);
        P(0x1d7); m.fmac(Op::mul, v(14), v(14), bc(v(14), 3), Y);
        P(0x1d8); m.minmax(false, v(14), v(14), bc(v(14), 1), W);
        P(0x1d9); m.fmac(Op::mul, acc, v(14), bc(v(20), 2), W);
        P(0x1da); m.fmac(Op::madd, v(14), v(14), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x1db); m.fmac(Op::mul, acc, v(14), I(), W);
        P(0x1dc); m.fmac(Op::msub, v(13), v(14), bc(v(4), 2), W);
        P(0x1dd); m.fmac(Op::mul, v(17), v(11), v(12), XYZW);
        P(0x1de); m.minmax(false, v(17), v(17), bc(v(0), 3), XYZW);
        P(0x1df); m.fmac(Op::mul, acc, v(9), bc(v(0), 3), Z | W);
        P(0x1e0); m.fmac(Op::msub, v(16), v(9), v(17), Z | W);
        P(0x1e1); m.fmac(Op::mul, acc, v(16), bc(v(0), 3), Z);
        P(0x1e2); m.fmac(Op::madd, v(16), v(17), bc(v(25), 3), Z);
        P(0x1e3); m.fmac(Op::mul, acc, v(16), v(0), W);
        P(0x1e4); m.fmac(Op::madd, v(10), v(17), bc(v(16), 2), W);
        P(0x1e5); m.fmac(Op::mul, acc, v(10), v(20), W);
        P(0x1e6); m.fmac(Op::madd, v(10), v(10), v(0), W);
        P(0x1e7); m.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x1e8); m.fmac(Op::msub, v(10), v(10), v(4), W);
        P(0x1e9); m.fmac(Op::mul, acc, v(3), bc(v(17), 0), XYZ);
        P(0x1ea); m.fmac(Op::madd, acc, v(18), bc(v(0), 3), XYZ);
        P(0x1eb); m.fmac(Op::msub, v(13), v(18), bc(v(17), 0), XYZ);
        P(0x1ec); m.fmac(Op::mul, acc, v(13), bc(v(17), 1), XYZ);
        P(0x1ed); m.fmac(Op::madd, acc, v(19), bc(v(0), 3), XYZ);
        P(0x1ee); m.fmac(Op::msub, v(13), v(19), bc(v(17), 1), XYZ);
        P(0x1ef); m.fmac(Op::mul, acc, v(13), bc(v(1), 3), XYZ);
        P(0x1f0); m.fmac(Op::msub, acc, v(13), bc(v(22), 3), XYZ);
        P(0x1f1); m.fmac(Op::madd, v(13), v(1), bc(v(0), 3), XYZ);
        P(0x1f2); m.fmac(Op::mul, v(10), v(10), bc(v(2), 3), XYZW);
        P(0x1f3); m.ftoi0(v(13), v(13), XYZW);
        P(0x1f4); s.i = 0x3fc00000u;  // 1.5
        P(0x1f5); m.sqi(10, 3);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x18b
        P(0x1f7); m.sqi(13, 4);  // the delay slot
        if (!again) break;
    }
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
    for (;;) {
        P(0x224); s.vi[2] = static_cast<std::uint16_t>(s.vi[2] - 1);
        P(0x225); m.fmac(Op::sub, v(29), v(29), I(), XYZW); m.mtir(5, v(19)[3]);
        P(0x226); m.mtir(6, v(24)[3]);
        P(0x227); m.fmac(Op::sub, v(23), v(30), I(), XYZ); m.mtir(6, v(24)[3]);
        P(0x228); m.fmac(Op::sub, v(28), v(28), I(), XYZW);
        P(0x229); m.fmac(Op::mul, acc, v(29), v(6), XYZW);
        P(0x22a); m.fmac(Op::madd, v(21), v(5), bc(v(0), 3), XYZW);
        P(0x22b); m.fmac(Op::add, acc, v(15), bc(v(15), 1), X);
        P(0x22c); m.fmac(Op::madd, v(22), v(0), bc(v(15), 2), X);
        P(0x22d); m.div(v(0)[3], v(22)[0]);
        P(0x22e); m.fmac(Op::mul, v(23), v(23), v(15), XYZ);
        P(0x230); m.fmac(Op::mul, v(10), v(23), Q(), XYZ);  // after the waitq at 0x22f
        P(0x231); m.fmac(Op::sub, v(26), v(26), v(0), W);
        P(0x232); m.fmac(Op::mul, v(20), v(28), v(8), XYZW);
        P(0x233); m.fmac(Op::mul, v(22), v(18), v(30), W);
        P(0x234); m.fmac(Op::mul, v(22), v(22), v(1), W);
        P(0x235); m.fmac(Op::add, v(20), v(20), v(7), X | Y);
        {
            P(0x236);
            const Qword o = v(27);
            m.write(v(27), Qword{o[1], o[2], o[3], o[0]}, XYZW);  // mr32
        }
        P(0x237); s.i = 0x40a00000u;  // 5.0
        P(0x238); m.fmac(Op::mul, acc, v(27), I(), XYZW);
        P(0x239); m.fmac(Op::madd, v(27), v(31), bc(v(0), 3), XYZW); s.i = 0x3fc00000u;  // 1.5
        P(0x23a); m.rxor(v(27)[0]);
        P(0x23b); m.rget(30, X);
        P(0x23c); m.rnext(29, X);
        P(0x23d); m.rnext(28, X);
        P(0x23e); m.rnext(27, X);
        P(0x23f); m.rxor(v(27)[1]);
        P(0x241); m.rget(29, Y);
        P(0x242); m.rnext(28, Y);
        P(0x243); m.rnext(30, Y);
        P(0x244); m.rnext(27, Y);
        P(0x245); m.rxor(v(27)[2]);
        P(0x247); m.rget(29, Z);
        P(0x248); m.rnext(28, Z);
        P(0x249); m.rnext(30, Z);
        P(0x24a); m.rnext(27, Z);
        P(0x24c); m.rxor(v(27)[3]);
        P(0x24d); m.rget(30, W);
        P(0x24e); m.rnext(29, W);
        P(0x24f); m.rnext(28, W);
        P(0x250); m.rnext(27, W);
        P(0x251); m.rnext(26, W);
        P(0x252); s.i = 0x38d1b717u;  // 1e-4
        P(0x253); m.div(v(0)[3], v(21)[2]);
        P(0x255); m.fmac(Op::mul, v(20), v(20), v(4), Y);
        P(0x257); m.fmac(Op::mul, acc, v(2), bc(v(21), 0), XYZ);
        P(0x258); m.fmac(Op::madd, acc, v(10), bc(v(0), 3), XYZ);
        P(0x259); m.fmac(Op::msub, v(10), v(10), bc(v(21), 0), XYZ);
        P(0x25a); m.fmac(Op::mul, v(10), v(10), bc(v(21), 1), XYZ);
        P(0x25b); m.fmac(Op::mul, v(25), v(24), bc(v(20), 1), XYZ);
        P(0x25c); m.fmac(Op::add, v(10), v(10), v(25), XYZ);
        P(0x25d); m.lq(25, 15);
        P(0x260); m.fmac(Op::mul, acc, v(10), bc(v(20), 0), XYZ);
        P(0x261); m.fmac(Op::madd, acc, v(23), bc(v(0), 3), XYZ);
        P(0x262); m.fmac(Op::madd, v(10), v(25), bc(v(0), 3), XYZ);
        P(0x263); m.fmac(Op::sub, v(12), v(4), bc(v(21), 3), X);
        P(0x264); m.minmax(true, v(12), v(12), I(), X);
        P(0x265); m.minmax(false, v(12), v(12), bc(v(21), 2), X);
        P(0x266); m.fmac(Op::add, v(12), v(0), bc(v(12), 0), Z);
        P(0x268); m.fmac(Op::mul, v(12), v(12), Q(), X | Z);  // after the waitq at 0x267
        P(0x269); m.fmac(Op::sub, v(12), v(31), bc(v(12), 0), Y | W);
        P(0x26a); m.fmac(Op::mul, v(14), v(9), v(12), X | Y);
        P(0x26b); m.minmax(false, v(14), v(3), bc(v(14), 0), W);
        P(0x26c); m.fmac(Op::mul, v(14), v(14), bc(v(14), 3), Y);
        P(0x26d); m.minmax(false, v(14), v(14), bc(v(14), 1), W);
        P(0x26e); m.fmac(Op::mul, acc, v(14), bc(v(20), 2), W);
        P(0x26f); m.fmac(Op::madd, v(14), v(14), v(0), W); s.i = 0x437f0000u;  // 255.0
        P(0x270); m.fmac(Op::mul, acc, v(14), I(), W);
        P(0x271); m.fmac(Op::msub, v(13), v(14), bc(v(4), 2), W);
        P(0x272); m.fmac(Op::mul, v(17), v(11), v(12), XYZW);
        P(0x273); m.minmax(false, v(17), v(17), bc(v(0), 3), XYZW);
        P(0x274); m.fmac(Op::mul, acc, v(9), bc(v(0), 3), Z | W);
        P(0x275); m.fmac(Op::msub, v(16), v(9), v(17), Z | W);
        P(0x276); m.fmac(Op::mul, acc, v(16), bc(v(0), 3), Z);
        P(0x277); m.fmac(Op::madd, v(16), v(17), bc(v(25), 3), Z);
        P(0x278); m.fmac(Op::mul, acc, v(16), v(0), W);
        P(0x279); m.fmac(Op::madd, v(10), v(17), bc(v(16), 2), W);
        P(0x27a); m.fmac(Op::mul, acc, v(10), v(20), W);
        P(0x27b); m.fmac(Op::madd, v(10), v(10), v(0), W);
        P(0x27c); m.fmac(Op::mul, acc, v(10), v(0), W);
        P(0x27d); m.fmac(Op::msub, v(10), v(10), v(4), W);
        P(0x27e); m.fmac(Op::mul, acc, v(3), bc(v(17), 0), XYZ);
        P(0x27f); m.fmac(Op::madd, acc, v(18), bc(v(0), 3), XYZ);
        P(0x280); m.fmac(Op::msub, v(13), v(18), bc(v(17), 0), XYZ);
        P(0x281); m.fmac(Op::mul, acc, v(13), bc(v(17), 1), XYZ);
        P(0x282); m.fmac(Op::madd, acc, v(19), bc(v(0), 3), XYZ);
        P(0x283); m.fmac(Op::msub, v(13), v(19), bc(v(17), 1), XYZ);
        P(0x284); m.fmac(Op::mul, acc, v(13), bc(v(1), 3), XYZ);
        P(0x285); m.fmac(Op::msub, v(13), v(13), bc(v(22), 3), XYZ);
        P(0x286); m.fmac(Op::mul, v(10), v(10), bc(v(2), 3), XYZW);
        P(0x287); m.ftoi0(v(13), v(13), XYZW);
        P(0x288); s.i = 0x3fc00000u;  // 1.5
        P(0x289); m.sqi(10, 3);
        const bool again = static_cast<std::int16_t>(s.vi[2]) > 0;  // ibgtz vi2, 0x224
        P(0x28b); m.sqi(13, 4);  // the delay slot
        if (!again) break;
    }
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
    for (int c = 0; c < 8; ++c) {
        g_pair = 0x3b + 5 * c;
        Qword clip{};
        m.fmac(Op::mul, s.acc, s.vf[1], bc(s.vf[corners[c][0]], 0), XYZW);
        m.fmac(Op::madd, s.acc, s.vf[2], bc(s.vf[corners[c][1]], 1), XYZW);
        m.fmac(Op::madd, s.acc, s.vf[3], bc(s.vf[corners[c][2]], 2), XYZW);
        m.fmac(Op::madd, clip, s.vf[4], bc(s.vf[0], 3), XYZW);
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
    std::vector<Qword> out;
    out.reserve(position.size() * 6);
    for (std::size_t k = 0; k < position.size(); ++k) {
        v(15) = position[k];
        v(23) = colour[k];
        P(flipped ? 0x128 : 0xce); m.fmac(Op::mul, v(9), v(8), bc(v(15), 3), XYZ);
        m.fmac(Op::mul, acc, v(1), bc(v(15), 0), XYZW);
        m.fmac(Op::madd, acc, v(2), bc(v(15), 1), XYZW);
        m.fmac(Op::madd, acc, v(3), bc(v(15), 2), XYZW);
        m.fmac(Op::madd, v(16), v(4), bc(v(0), 3), XYZW);
        m.fmac(Op::add, v(10), v(9), v(0), XYZ);
        // CLIPW.xyz vf16, vf16w; FCAND vi1, 0x3f; IADD vi9, vi1, vi10 (0x7fff).
        const double w = std::fabs(daz(v(16)[3]));
        bool clipped = false;
        for (int c = 0; c < 3; ++c) {
            const double x = daz(v(16)[c]);
            clipped = clipped || x > w || x < -w;
        }
        const std::uint32_t adc = clipped ? 0xffff8000u : 0x00007fffu;  // MFIR sign-extends vi9
        m.div(v(0)[3], v(16)[3]);
        m.fmac(Op::mul, v(17), v(16), v(5), XYZ);
        m.fmac(Op::add, acc, v(6), v(0), XYZ);
        m.fmac(Op::madd, v(18), v(17), imm(s.q), XYZ);
        m.fmac(Op::mul, v(10), v(10), imm(s.q), X | Y);
        if (flipped) {
            P(0x138); m.write(v(13), {0, 0, 0x3f800000u, 0}, Z | W);  // mr32.zw vf13, vf0
            m.write(v(14), {0, 0, 0x3f800000u, 0}, Z | W);
            m.fmac(Op::add, v(13), v(11), v(30), X | Y);
            m.fmac(Op::add, v(14), v(12), v(30), X | Y);
            m.fmac(Op::mul, v(26), v(13), imm(s.q), XYZW);
            m.fmac(Op::mul, v(25), v(14), imm(s.q), XYZW);
        } else {
            m.fmac(Op::mul, v(25), v(11), imm(s.q), XYZW);
            m.fmac(Op::mul, v(26), v(12), imm(s.q), XYZW);
        }
        m.fmac(Op::sub, v(19), v(18), v(10), XYZ);
        m.fmac(Op::add, v(20), v(18), v(10), XYZ);
        m.ftoi0(v(21), v(19), XYZ);
        m.ftoi0(v(22), v(20), XYZ);
        v(21)[3] = adc;
        v(22)[3] = adc;
        for (int r : {25, 23, 21, 26, 23, 22}) out.push_back(v(r));
        if (flipped) {  // mr32.xyzw vf11 / vf12 (x <- y, y <- z, z <- w, w <- x)
            P(0x140); v(11) = {v(11)[1], v(11)[2], v(11)[3], v(11)[0]};
            P(0x144); v(12) = {v(12)[1], v(12)[2], v(12)[3], v(12)[0]};
        }
    }
    return out;
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
    std::vector<Qword> out;
    out.reserve(position.size() * 14);
    // The clip-flag register (fcset 0 in the setup): CLIPW shifts a six-bit
    // judgment in per particle; FCAND 0x3ffff reads this one and the two before.
    std::uint32_t clip_flags = 0;
    for (std::size_t k = 0; k < position.size(); ++k) {
        v(15) = position[k];
        v(23) = colour[k];
        P(0x3b7); m.fmac(Op::mul, acc, v(1), bc(v(15), 0), XYZW);
        m.fmac(Op::madd, acc, v(2), bc(v(15), 1), XYZW);
        m.fmac(Op::madd, acc, v(3), bc(v(15), 2), XYZW);
        m.fmac(Op::madd, v(16), v(4), bc(v(0), 3), XYZW);
        P(0x3bb); m.fmac(Op::mul, v(7), v(27), bc(v(15), 3), X | Y);
        P(0x3bc); m.fmac(Op::mul, v(8), v(28), bc(v(15), 3), X | Y);
        P(0x3bf);
        const double w = std::fabs(daz(v(16)[3]));
        std::uint32_t j = 0;
        for (int c = 0; c < 3; ++c) {
            const double x = daz(v(16)[c]);
            if (x > w) j |= 1u << (2 * c);
            if (x < -w) j |= 2u << (2 * c);
        }
        m.div(v(0)[3], v(16)[3]);
        P(0x3c0); m.fmac(Op::mul, v(17), v(16), v(5), XYZ);
        P(0x3c1); m.fmac(Op::mul, v(13), v(16), v(10), X | Y);
        const std::uint32_t flags = ((clip_flags << 6) | j) & 0xffffffu;
        // FCAND vi1, 0x3ffff; IADD vi1, vi1, vi10 (0x7fff); MFIR sign-extends.
        const std::uint32_t adc = (flags & 0x3ffffu) ? 0xffff8000u : 0x00007fffu;
        P(0x3c5); m.fmac(Op::add, acc, v(6), v(0), XYZ);
        P(0x3c6); m.fmac(Op::madd, v(18), v(17), imm(s.q), XYZ);
        P(0x3c7); m.fmac(Op::add, acc, v(11), v(0), X | Y);
        P(0x3c8); m.fmac(Op::madd, v(14), v(13), imm(s.q), X | Y);
        P(0x3c9); m.fmac(Op::mul, v(7), v(7), imm(s.q), X | Y);
        P(0x3ca); m.fmac(Op::mul, acc, v(8), imm(s.q), X | Y);
        P(0x3cb); m.fmac(Op::madd, v(8), v(31), imm(s.i), X | Y);
        P(0x3cc); m.fmac(Op::mul, v(9), v(12), imm(s.q), XYZW);
        m.write(v(19), v(18), Z);
        P(0x3cd); m.fmac(Op::mul, v(13), v(24), imm(s.q), XYZW);
        m.write(v(20), v(18), Z);
        P(0x3ce); m.fmac(Op::sub, v(19), v(18), v(7), X | Y);
        P(0x3cf); m.fmac(Op::add, v(20), v(18), v(7), X | Y);
        P(0x3d0); m.fmac(Op::sub, v(25), v(14), v(8), X | Y);
        P(0x3d1); m.fmac(Op::add, v(26), v(14), v(8), X | Y);
        P(0x3d2); m.ftoi0(v(21), v(19), XYZ);
        P(0x3d3); m.ftoi0(v(22), v(20), XYZ);
        v(21)[3] = adc;
        v(22)[3] = adc;
        clip_flags = flags;
        for (int r : {29, 9, 23, 21, 13, 23, 22, 30, 25, 23, 21, 26, 23, 22}) out.push_back(v(r));
    }
    // Pairs 0x3e5..0x3e7: isw.x 0x8001 (the EOP bit) into the last GIF tag, -7(vi8).
    if (!out.empty()) out[out.size() - 7][0] = 0x8001u;
    return out;
}

}  // namespace omega::particle
