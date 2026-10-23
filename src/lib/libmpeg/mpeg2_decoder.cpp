#include "mpeg2_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace omega::mpeg2 {

const std::array<std::array<std::int32_t, 8>, 8> idct_basis{{
    {23170, 23170, 23170, 23170, 23170, 23170, 23170, 23170},
    {32138, 27246, 18205, 6393, -6393, -18205, -27246, -32138},
    {30274, 12540, -12540, -30274, -30274, -12540, 12540, 30274},
    {27246, -6393, -32138, -18205, 18205, 32138, 6393, -27246},
    {23170, -23170, -23170, 23170, 23170, -23170, -23170, 23170},
    {18205, -32138, 6393, 27246, -27246, -6393, 32138, -18205},
    {12540, -30274, 30274, -12540, -12540, 30274, -30274, 12540},
    {6393, -18205, 27246, -32138, 32138, -27246, 18205, -6393},
}};

const std::array<std::uint8_t, 64> zigzag_scan{
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const std::array<std::uint8_t, 64> alternate_scan{
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
const std::array<std::uint8_t, 64> default_intra_matrix{
    8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83};
const std::array<std::uint8_t, 32> non_linear_quantiser_scale{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

void idct_8x8(const std::int32_t* f, std::int32_t* out) {
    std::int64_t r[8][8];
    bool row_used[8];
    for (int u = 0; u < 8; ++u) {
        const std::int32_t* row = f + u * 8;
        bool any = false;
        for (int v = 0; v < 8; ++v) any |= row[v] != 0;
        row_used[u] = any;
        if (!any) continue;
        for (int x = 0; x < 8; ++x) {
            std::int64_t sum = 0;
            for (int v = 0; v < 8; ++v) sum += std::int64_t(row[v]) * idct_basis[v][x];
            r[u][x] = sum;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            std::int64_t sum = 0;
            for (int u = 0; u < 8; ++u)
                if (row_used[u]) sum += std::int64_t(idct_basis[u][y]) * r[u][x];
            std::int64_t value = (sum + (std::int64_t(1) << 31)) >> 32;
            out[y * 8 + x] = static_cast<std::int32_t>(std::clamp<std::int64_t>(value, -256, 255));
        }
    }
}

// The same sums regrouped: K[u][7-x] = (-1)^u K[u][x] splits each into an even and an odd half (out[x] = E + O,
// out[7-x] = E - O), the row pass stays in 32 bits (|F| <= 12288 and every basis column's absolute sum is 173136,
// so |R| < 2^31; larger input takes idct_8x8) and the column pass multiplies 32 x 32 -> 64. Integer and exact.
void idct_8x8_fast(const std::int32_t* f, std::int32_t* out) {
    const auto& K = idct_basis;
    int rows[8];
    int used = 0;
    bool dc_only = true;
    for (int u = 0; u < 8; ++u) {
        const std::int32_t* row = f + u * 8;
        bool any = false;
        for (int v = 0; v < 8; ++v) {
            if (static_cast<std::uint32_t>(row[v] + 12288) > 24576u) {
                idct_8x8(f, out);
                return;
            }
            any |= row[v] != 0;
            if (v | u) dc_only &= row[v] == 0;
        }
        if (any) rows[used++] = u;
    }
    if (dc_only) {
        const std::int64_t sum = std::int64_t(K[0][0]) * (f[0] * K[0][0]);
        const auto value = static_cast<std::int32_t>(std::clamp<std::int64_t>((sum + (std::int64_t(1) << 31)) >> 32, -256, 255));
        for (int i = 0; i < 64; ++i) out[i] = value;
        return;
    }
    std::int32_t r[8][8];
    for (int i = 0; i < used; ++i) {
        const int u = rows[i];
        const std::int32_t* row = f + u * 8;
        for (int x = 0; x < 4; ++x) {
            const std::int32_t e = row[0] * K[0][x] + row[2] * K[2][x] + row[4] * K[4][x] + row[6] * K[6][x];
            const std::int32_t o = row[1] * K[1][x] + row[3] * K[3][x] + row[5] * K[5][x] + row[7] * K[7][x];
            r[u][x] = e + o;
            r[u][7 - x] = e - o;
        }
    }
    int even[4], odd[4];
    int n_even = 0, n_odd = 0;
    for (int i = 0; i < used; ++i) (rows[i] & 1 ? odd[n_odd++] : even[n_even++]) = rows[i];
    const auto put = [&](int y, int x, std::int64_t sum) {
        out[y * 8 + x] = static_cast<std::int32_t>(std::clamp<std::int64_t>((sum + (std::int64_t(1) << 31)) >> 32, -256, 255));
    };
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 4; ++y) {
            std::int64_t e = 0, o = 0;
            for (int i = 0; i < n_even; ++i) e += std::int64_t(K[even[i]][y]) * r[even[i]][x];
            for (int i = 0; i < n_odd; ++i) o += std::int64_t(K[odd[i]][y]) * r[odd[i]][x];
            put(y, x, e + o);
            put(7 - y, x, e - o);
        }
    }
}

#if defined(__ARM_NEON)
namespace {

// K[v][0..3] (the row pass's other half from K[v][7-x] = (-1)^v K[v][x]).
alignas(16) const std::int16_t idct_k16[8][4] = {
    {23170, 23170, 23170, 23170}, {32138, 27246, 18205, 6393},  {30274, 12540, -12540, -30274},
    {27246, -6393, -32138, -18205}, {23170, -23170, -23170, 23170}, {18205, -32138, 6393, 27246},
    {12540, -30274, 30274, -12540}, {6393, -18205, 27246, -32138}};

// idct_8x8_fast's sums on NEON for |F| <= 2048 (|R| <= 2048 * 173136 < 2^31): the row pass 16 x 16 -> 32-bit
// multiply-accumulates over the rows set in `rows` (the others are zero), the column pass 32 x 32 -> 64-bit ones
// with the even / odd split, then (S + 2^31) >> 32 (the high word: a plain add and narrowing shift) clamped to
// [-256, 255]. The same integers as idct_8x8.
inline void idct_neon(const std::int16_t* f, unsigned rows, int16x8_t (&out)[8]) {
    alignas(16) std::int32_t r[8][8];
    int even[4], odd[4];
    int ne = 0, no = 0;
    for (int u = 0; u < 8; ++u) {
        if (!((rows >> u) & 1)) continue;
        if (u & 1) odd[no++] = u;
        else even[ne++] = u;
        const int16x8_t F = vld1q_s16(f + 8 * u);
        const int16x4_t lo = vget_low_s16(F), hi = vget_high_s16(F);
        int32x4_t e = vmull_lane_s16(vld1_s16(idct_k16[0]), lo, 0);
        e = vmlal_lane_s16(e, vld1_s16(idct_k16[2]), lo, 2);
        e = vmlal_lane_s16(e, vld1_s16(idct_k16[4]), hi, 0);
        e = vmlal_lane_s16(e, vld1_s16(idct_k16[6]), hi, 2);
        int32x4_t o = vmull_lane_s16(vld1_s16(idct_k16[1]), lo, 1);
        o = vmlal_lane_s16(o, vld1_s16(idct_k16[3]), lo, 3);
        o = vmlal_lane_s16(o, vld1_s16(idct_k16[5]), hi, 1);
        o = vmlal_lane_s16(o, vld1_s16(idct_k16[7]), hi, 3);
        vst1q_s32(r[u], vaddq_s32(e, o));
        const int32x4_t d = vrev64q_s32(vsubq_s32(e, o));   // R[7 - x] = E[x] - O[x]
        vst1q_s32(r[u] + 4, vcombine_s32(vget_high_s32(d), vget_low_s32(d)));
    }
    const int64x2_t half = vdupq_n_s64(std::int64_t(1) << 31);
    const int32x4_t lowest = vdupq_n_s32(-256), highest = vdupq_n_s32(255);
    const auto narrow = [&](int64x2_t a, int64x2_t b, int64x2_t c, int64x2_t d) {
        const int32x4_t x0 = vcombine_s32(vshrn_n_s64(vaddq_s64(a, half), 32), vshrn_n_s64(vaddq_s64(b, half), 32));
        const int32x4_t x1 = vcombine_s32(vshrn_n_s64(vaddq_s64(c, half), 32), vshrn_n_s64(vaddq_s64(d, half), 32));
        return vcombine_s16(vmovn_s32(vminq_s32(vmaxq_s32(x0, lowest), highest)),
                            vmovn_s32(vminq_s32(vmaxq_s32(x1, lowest), highest)));
    };
    for (int y = 0; y < 4; ++y) {
        const int64x2_t zero = vdupq_n_s64(0);
        int64x2_t e0 = zero, e1 = zero, e2 = zero, e3 = zero, o0 = zero, o1 = zero, o2 = zero, o3 = zero;
        for (int i = 0; i < ne; ++i) {
            const std::int32_t k = idct_basis[std::size_t(even[i])][std::size_t(y)];
            const int32x4_t a = vld1q_s32(r[even[i]]), b = vld1q_s32(r[even[i]] + 4);
            e0 = vmlal_n_s32(e0, vget_low_s32(a), k);
            e1 = vmlal_n_s32(e1, vget_high_s32(a), k);
            e2 = vmlal_n_s32(e2, vget_low_s32(b), k);
            e3 = vmlal_n_s32(e3, vget_high_s32(b), k);
        }
        for (int i = 0; i < no; ++i) {
            const std::int32_t k = idct_basis[std::size_t(odd[i])][std::size_t(y)];
            const int32x4_t a = vld1q_s32(r[odd[i]]), b = vld1q_s32(r[odd[i]] + 4);
            o0 = vmlal_n_s32(o0, vget_low_s32(a), k);
            o1 = vmlal_n_s32(o1, vget_high_s32(a), k);
            o2 = vmlal_n_s32(o2, vget_low_s32(b), k);
            o3 = vmlal_n_s32(o3, vget_high_s32(b), k);
        }
        out[y] = narrow(vaddq_s64(e0, o0), vaddq_s64(e1, o1), vaddq_s64(e2, o2), vaddq_s64(e3, o3));
        out[7 - y] = narrow(vsubq_s64(e0, o0), vsubq_s64(e1, o1), vsubq_s64(e2, o2), vsubq_s64(e3, o3));
    }
}

}  // namespace

void idct_8x8_neon(const std::int16_t* f, std::int16_t* out) {
    unsigned rows = 0;
    for (int i = 0; i < 64; ++i)
        if (f[i]) rows |= 1u << (i >> 3);
    int16x8_t o[8];
    idct_neon(f, rows, o);
    for (int y = 0; y < 8; ++y) vst1q_s16(out + 8 * y, o[y]);
}
#endif

namespace {

// ----------------------------------------------------------------- Annex B text
struct TextVlc {
    const char* bits;
    int value;
};
struct TextDct {
    const char* bits;
    int run, level;
};

// B.1 macroblock_address_increment (34 = macroblock_escape, 35 = stuffing).
constexpr TextVlc text_b1[] = {
    {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"0001 1", 6}, {"0001 0", 7},
    {"0000 111", 8}, {"0000 110", 9}, {"0000 1011", 10}, {"0000 1010", 11}, {"0000 1001", 12},
    {"0000 1000", 13}, {"0000 0111", 14}, {"0000 0110", 15}, {"0000 0101 11", 16},
    {"0000 0101 10", 17}, {"0000 0101 01", 18}, {"0000 0101 00", 19}, {"0000 0100 11", 20},
    {"0000 0100 10", 21}, {"0000 0100 011", 22}, {"0000 0100 010", 23}, {"0000 0100 001", 24},
    {"0000 0100 000", 25}, {"0000 0011 111", 26}, {"0000 0011 110", 27}, {"0000 0011 101", 28},
    {"0000 0011 100", 29}, {"0000 0011 011", 30}, {"0000 0011 010", 31}, {"0000 0011 001", 32},
    {"0000 0011 000", 33}, {"0000 0001 000", 34}, {"0000 0001 111", 35}};

// B.2-B.4 macroblock_type; value bits: 16 quant, 8 motion_forward, 4 motion_backward, 2 pattern, 1 intra.
constexpr TextVlc text_b2[] = {{"1", 1}, {"01", 16 | 1}};
constexpr TextVlc text_b3[] = {
    {"1", 8 | 2}, {"01", 2}, {"001", 8}, {"0001 1", 1}, {"0001 0", 16 | 8 | 2}, {"0000 1", 16 | 2},
    {"0000 01", 16 | 1}};
constexpr TextVlc text_b4[] = {
    {"10", 8 | 4}, {"11", 8 | 4 | 2}, {"010", 4}, {"011", 4 | 2}, {"0010", 8}, {"0011", 8 | 2},
    {"0001 1", 1}, {"0001 0", 16 | 8 | 4 | 2}, {"0000 11", 16 | 8 | 2}, {"0000 10", 16 | 4 | 2},
    {"0000 01", 16 | 1}};

// B.9 coded_block_pattern (the 4:2:0 cbp; "0000 0000 1" codes cbp 0).
constexpr TextVlc text_b9[] = {
    {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"1001 1", 12}, {"1001 0", 48},
    {"1000 1", 20}, {"1000 0", 40}, {"0111 1", 28}, {"0111 0", 44}, {"0110 1", 52}, {"0110 0", 56},
    {"0101 1", 1}, {"0101 0", 61}, {"0100 1", 2}, {"0100 0", 62}, {"0011 11", 24}, {"0011 10", 36},
    {"0011 01", 3}, {"0011 00", 63}, {"0010 111", 5}, {"0010 110", 9}, {"0010 101", 17},
    {"0010 100", 33}, {"0010 011", 6}, {"0010 010", 10}, {"0010 001", 18}, {"0010 000", 34},
    {"0001 1111", 7}, {"0001 1110", 11}, {"0001 1101", 19}, {"0001 1100", 35}, {"0001 1011", 13},
    {"0001 1010", 49}, {"0001 1001", 21}, {"0001 1000", 41}, {"0001 0111", 14}, {"0001 0110", 50},
    {"0001 0101", 22}, {"0001 0100", 42}, {"0001 0011", 15}, {"0001 0010", 51}, {"0001 0001", 23},
    {"0001 0000", 43}, {"0000 1111", 25}, {"0000 1110", 37}, {"0000 1101", 26}, {"0000 1100", 38},
    {"0000 1011", 29}, {"0000 1010", 45}, {"0000 1001", 53}, {"0000 1000", 57}, {"0000 0111", 30},
    {"0000 0110", 46}, {"0000 0101", 54}, {"0000 0100", 58}, {"0000 0011 1", 31}, {"0000 0011 0", 47},
    {"0000 0010 1", 55}, {"0000 0010 0", 59}, {"0000 0001 1", 27}, {"0000 0001 0", 39},
    {"0000 0000 1", 0}};

// B.10 motion_code magnitude (the sign bit follows every non-zero code).
constexpr TextVlc text_b10[] = {
    {"1", 0}, {"01", 1}, {"001", 2}, {"0001", 3}, {"0000 11", 4}, {"0000 101", 5}, {"0000 100", 6},
    {"0000 011", 7}, {"0000 0101 1", 8}, {"0000 0101 0", 9}, {"0000 0100 1", 10},
    {"0000 0100 01", 11}, {"0000 0100 00", 12}, {"0000 0011 11", 13}, {"0000 0011 10", 14},
    {"0000 0011 01", 15}, {"0000 0011 00", 16}};

// B.12 / B.13 dct_dc_size.
constexpr TextVlc text_b12[] = {
    {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"1111 0", 6},
    {"1111 10", 7}, {"1111 110", 8}, {"1111 1110", 9}, {"1111 1111 0", 10}, {"1111 1111 1", 11}};
constexpr TextVlc text_b13[] = {
    {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"1111 0", 5}, {"1111 10", 6},
    {"1111 110", 7}, {"1111 1110", 8}, {"1111 1111 0", 9}, {"1111 1111 10", 10}, {"1111 1111 11", 11}};

// The 14..16-bit codes shared by B.14 and B.15.
#define OMEGA_DCT_LONG_CODES                                                                         \
    {"0000 0000 0111 11", 0, 16}, {"0000 0000 0111 10", 0, 17}, {"0000 0000 0111 01", 0, 18},        \
    {"0000 0000 0111 00", 0, 19}, {"0000 0000 0110 11", 0, 20}, {"0000 0000 0110 10", 0, 21},        \
    {"0000 0000 0110 01", 0, 22}, {"0000 0000 0110 00", 0, 23}, {"0000 0000 0101 11", 0, 24},        \
    {"0000 0000 0101 10", 0, 25}, {"0000 0000 0101 01", 0, 26}, {"0000 0000 0101 00", 0, 27},        \
    {"0000 0000 0100 11", 0, 28}, {"0000 0000 0100 10", 0, 29}, {"0000 0000 0100 01", 0, 30},        \
    {"0000 0000 0100 00", 0, 31}, {"0000 0000 0011 000", 0, 32}, {"0000 0000 0010 111", 0, 33},      \
    {"0000 0000 0010 110", 0, 34}, {"0000 0000 0010 101", 0, 35}, {"0000 0000 0010 100", 0, 36},     \
    {"0000 0000 0010 011", 0, 37}, {"0000 0000 0010 010", 0, 38}, {"0000 0000 0010 001", 0, 39},     \
    {"0000 0000 0010 000", 0, 40}, {"0000 0000 0011 111", 1, 8}, {"0000 0000 0011 110", 1, 9},       \
    {"0000 0000 0011 101", 1, 10}, {"0000 0000 0011 100", 1, 11}, {"0000 0000 0011 011", 1, 12},     \
    {"0000 0000 0011 010", 1, 13}, {"0000 0000 0011 001", 1, 14}, {"0000 0000 0001 0011", 1, 15},    \
    {"0000 0000 0001 0010", 1, 16}, {"0000 0000 0001 0001", 1, 17}, {"0000 0000 0001 0000", 1, 18},  \
    {"0000 0000 0001 0100", 6, 3}, {"0000 0000 0001 1010", 11, 2}, {"0000 0000 0001 1001", 12, 2},   \
    {"0000 0000 0001 1000", 13, 2}, {"0000 0000 0001 0111", 14, 2}, {"0000 0000 0001 0110", 15, 2},  \
    {"0000 0000 0001 0101", 16, 2}, {"0000 0000 0001 1111", 27, 1}, {"0000 0000 0001 1110", 28, 1},  \
    {"0000 0000 0001 1101", 29, 1}, {"0000 0000 0001 1100", 30, 1}, {"0000 0000 0001 1011", 31, 1}

// B.14 DCT coefficients table zero (sign bit excluded; (0,1) listed in its "11" form).
constexpr TextDct text_b14[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"0010 1", 0, 3}, {"0011 1", 3, 1},
    {"0011 0", 4, 1}, {"0001 10", 1, 2}, {"0001 11", 5, 1}, {"0001 01", 6, 1}, {"0001 00", 7, 1},
    {"0000 110", 0, 4}, {"0000 100", 2, 2}, {"0000 111", 8, 1}, {"0000 101", 9, 1},
    {"0010 0110", 0, 5}, {"0010 0001", 0, 6}, {"0010 0101", 1, 3}, {"0010 0100", 3, 2},
    {"0010 0111", 10, 1}, {"0010 0011", 11, 1}, {"0010 0010", 12, 1}, {"0010 0000", 13, 1},
    {"0000 0010 10", 0, 7}, {"0000 0011 00", 1, 4}, {"0000 0010 11", 2, 3}, {"0000 0011 11", 4, 2},
    {"0000 0010 01", 5, 2}, {"0000 0011 10", 14, 1}, {"0000 0011 01", 15, 1}, {"0000 0010 00", 16, 1},
    {"0000 0001 1101", 0, 8}, {"0000 0001 1000", 0, 9}, {"0000 0001 0011", 0, 10},
    {"0000 0001 0000", 0, 11}, {"0000 0001 1011", 1, 5}, {"0000 0001 0100", 2, 4},
    {"0000 0001 1100", 3, 3}, {"0000 0001 0010", 4, 3}, {"0000 0001 1110", 6, 2},
    {"0000 0001 0101", 7, 2}, {"0000 0001 0001", 8, 2}, {"0000 0001 1111", 17, 1},
    {"0000 0001 1010", 18, 1}, {"0000 0001 1001", 19, 1}, {"0000 0001 0111", 20, 1},
    {"0000 0001 0110", 21, 1}, {"0000 0000 1101 0", 0, 12}, {"0000 0000 1100 1", 0, 13},
    {"0000 0000 1100 0", 0, 14}, {"0000 0000 1011 1", 0, 15}, {"0000 0000 1011 0", 1, 6},
    {"0000 0000 1010 1", 1, 7}, {"0000 0000 1010 0", 2, 5}, {"0000 0000 1001 1", 3, 4},
    {"0000 0000 1001 0", 5, 3}, {"0000 0000 1000 1", 9, 2}, {"0000 0000 1000 0", 10, 2},
    {"0000 0000 1111 1", 22, 1}, {"0000 0000 1111 0", 23, 1}, {"0000 0000 1110 1", 24, 1},
    {"0000 0000 1110 0", 25, 1}, {"0000 0000 1101 1", 26, 1}, OMEGA_DCT_LONG_CODES};

// B.15 DCT coefficients table one.
constexpr TextDct text_b15[] = {
    {"10", 0, 1}, {"010", 1, 1}, {"110", 0, 2}, {"0010 1", 2, 1}, {"0111", 0, 3}, {"0011 1", 3, 1},
    {"0001 10", 4, 1}, {"0011 0", 1, 2}, {"0001 11", 5, 1}, {"0000 110", 6, 1}, {"0000 100", 7, 1},
    {"1110 0", 0, 4}, {"0000 111", 2, 2}, {"0000 101", 8, 1}, {"1111 000", 9, 1},
    {"1110 1", 0, 5}, {"0001 01", 0, 6}, {"1111 001", 1, 3}, {"0010 0110", 3, 2},
    {"1111 010", 10, 1}, {"0010 0001", 11, 1}, {"0010 0101", 12, 1}, {"0010 0100", 13, 1},
    {"0001 00", 0, 7}, {"0010 0111", 1, 4}, {"1111 1100", 2, 3}, {"1111 1101", 4, 2},
    {"0000 0010 0", 5, 2}, {"0000 0010 1", 14, 1}, {"0000 0011 1", 15, 1}, {"0000 0011 01", 16, 1},
    {"1111 011", 0, 8}, {"1111 100", 0, 9}, {"0010 0011", 0, 10}, {"0010 0010", 0, 11},
    {"0010 0000", 1, 5}, {"0000 0011 00", 2, 4}, {"0000 0001 1100", 3, 3}, {"0000 0001 0010", 4, 3},
    {"0000 0001 1110", 6, 2}, {"0000 0001 0101", 7, 2}, {"0000 0001 0001", 8, 2},
    {"0000 0001 1111", 17, 1}, {"0000 0001 1010", 18, 1}, {"0000 0001 1001", 19, 1},
    {"0000 0001 0111", 20, 1}, {"0000 0001 0110", 21, 1}, {"1111 1010", 0, 12}, {"1111 1011", 0, 13},
    {"1111 1110", 0, 14}, {"1111 1111", 0, 15}, {"0000 0000 1011 0", 1, 6},
    {"0000 0000 1010 1", 1, 7}, {"0000 0000 1010 0", 2, 5}, {"0000 0000 1001 1", 3, 4},
    {"0000 0000 1001 0", 5, 3}, {"0000 0000 1000 1", 9, 2}, {"0000 0000 1000 0", 10, 2},
    {"0000 0000 1111 1", 22, 1}, {"0000 0000 1111 0", 23, 1}, {"0000 0000 1110 1", 24, 1},
    {"0000 0000 1110 0", 25, 1}, {"0000 0000 1101 1", 26, 1}, OMEGA_DCT_LONG_CODES};
#undef OMEGA_DCT_LONG_CODES

std::pair<std::uint32_t, std::uint8_t> parse_bits(const char* text) {
    std::uint32_t code = 0;
    std::uint8_t length = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') continue;
        if (*p != '0' && *p != '1') throw std::logic_error("MPEG-2 table: bad code text");
        code = (code << 1) | std::uint32_t(*p - '0');
        ++length;
    }
    return {code, length};
}

template <std::size_t N>
std::vector<VlcEntry> make_vlc(const TextVlc (&text)[N]) {
    std::vector<VlcEntry> out;
    for (const auto& t : text) {
        const auto [code, length] = parse_bits(t.bits);
        out.push_back({code, length, static_cast<std::int16_t>(t.value)});
    }
    return out;
}

template <std::size_t N>
std::vector<DctEntry> make_dct(const TextDct (&text)[N]) {
    std::vector<DctEntry> out;
    for (const auto& t : text) {
        const auto [code, length] = parse_bits(t.bits);
        out.push_back({code, length, static_cast<std::uint8_t>(t.run), static_cast<std::uint8_t>(t.level)});
    }
    return out;
}

const std::vector<VlcEntry>& b1() { static const auto t = make_vlc(text_b1); return t; }
const std::vector<VlcEntry>& b2() { static const auto t = make_vlc(text_b2); return t; }
const std::vector<VlcEntry>& b3() { static const auto t = make_vlc(text_b3); return t; }
const std::vector<VlcEntry>& b4() { static const auto t = make_vlc(text_b4); return t; }
const std::vector<VlcEntry>& b9() { static const auto t = make_vlc(text_b9); return t; }
const std::vector<VlcEntry>& b10() { static const auto t = make_vlc(text_b10); return t; }
const std::vector<VlcEntry>& b12() { static const auto t = make_vlc(text_b12); return t; }
const std::vector<VlcEntry>& b13() { static const auto t = make_vlc(text_b13); return t; }
const std::vector<DctEntry>& b14() { static const auto t = make_dct(text_b14); return t; }
const std::vector<DctEntry>& b15() { static const auto t = make_dct(text_b15); return t; }

// Lookup tables indexed by the next `bits` bits: (value << 8) | length, 0 = invalid code.
struct Lut {
    int bits{};
    std::vector<std::uint32_t> entries;
};

Lut build_lut(const std::vector<VlcEntry>& table, int bits) {
    Lut lut{bits, std::vector<std::uint32_t>(std::size_t(1) << bits, 0)};
    for (const auto& e : table) {
        if (e.length == 0 || e.length > bits) throw std::logic_error("MPEG-2 table: code too long");
        const std::uint32_t base = e.code << (bits - e.length);
        for (std::uint32_t k = 0; k < (1u << (bits - e.length)); ++k) {
            if (lut.entries[base + k]) throw std::logic_error("MPEG-2 table: codes are not prefix-free");
            lut.entries[base + k] = (std::uint32_t(std::uint16_t(e.value)) << 8) | e.length;
        }
    }
    return lut;
}

// DCT lookup: run | level << 8 | kind << 16 | length << 24; kind 1 = coefficient, 2 = escape, 3 = EOB.
constexpr std::uint32_t kind_coefficient = 1, kind_escape = 2, kind_eob = 3;
struct DctLut {
    std::vector<std::uint32_t> entries;
};

DctLut build_dct_lut(const std::vector<DctEntry>& table, DctSpecialCodes special) {
    constexpr int bits = 16;
    DctLut lut{std::vector<std::uint32_t>(std::size_t(1) << bits, 0)};
    const auto put = [&](std::uint32_t code, int length, std::uint32_t value) {
        if (length == 0 || length > bits) throw std::logic_error("MPEG-2 DCT table: code too long");
        const std::uint32_t base = code << (bits - length);
        for (std::uint32_t k = 0; k < (1u << (bits - length)); ++k) {
            if (lut.entries[base + k]) throw std::logic_error("MPEG-2 DCT table: codes are not prefix-free");
            lut.entries[base + k] = value | (std::uint32_t(length) << 24);
        }
    };
    for (const auto& e : table) put(e.code, e.length, e.run | (std::uint32_t(e.level) << 8) | (kind_coefficient << 16));
    put(special.escape_code, special.escape_length, kind_escape << 16);
    put(special.eob_code, special.eob_length, kind_eob << 16);
    return lut;
}

const Lut& lut_b1() { static const auto l = build_lut(b1(), 11); return l; }
const Lut& lut_b2() { static const auto l = build_lut(b2(), 2); return l; }
const Lut& lut_b3() { static const auto l = build_lut(b3(), 6); return l; }
const Lut& lut_b4() { static const auto l = build_lut(b4(), 6); return l; }
const Lut& lut_b9() { static const auto l = build_lut(b9(), 9); return l; }
const Lut& lut_b10() { static const auto l = build_lut(b10(), 10); return l; }
const Lut& lut_b12() { static const auto l = build_lut(b12(), 10); return l; }
const Lut& lut_b13() { static const auto l = build_lut(b13(), 10); return l; }
const DctLut& lut_b14() { static const auto l = build_dct_lut(b14(), table_b14_special()); return l; }
const DctLut& lut_b15() { static const auto l = build_dct_lut(b15(), table_b15_special()); return l; }

constexpr int mb_quant = 16, mb_forward = 8, mb_backward = 4, mb_pattern = 2, mb_intra = 1;

inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Half-sample prediction of a w x h block (7.6.4) from a plane of pw x ph,
// rows y, y+ystep, ...; reads outside the picture clamp to the edge (field
// reads clamp within their own parity), as mpeg2_ref.py's _hp.
void half_pel(const std::uint8_t* plane, int pw, int ph, int x, int y, int w, int h, int hx, int hy,
              int ystep, std::uint8_t* out, int out_stride) {
    const bool inside = x >= 0 && y >= 0 && x + w + 1 <= pw && y + (h + 1) * ystep <= ph + ystep - 1;
#if defined(__vita__)
    if (inside) {   // port-only (132vt2a): cols[i] = x + i, read in place
#if defined(__ARM_NEON)
        if (w == 16 || w == 8) {   // port-only (132vt2a2): the same averages, 8 or 16 at a time
            const uint16x8_t two = vdupq_n_u16(2);
            for (int j = 0; j < h; ++j) {
                const std::uint8_t* a = plane + std::size_t(y + j * ystep) * pw + x;
                const std::uint8_t* c = a + std::size_t(ystep) * pw;
                std::uint8_t* o = out + j * out_stride;
                if (w == 16) {
                    if (!hx && !hy) {
                        vst1q_u8(o, vld1q_u8(a));
                    } else if (hx && !hy) {
                        vst1q_u8(o, vrhaddq_u8(vld1q_u8(a), vld1q_u8(a + 1)));
                    } else if (!hx && hy) {
                        vst1q_u8(o, vrhaddq_u8(vld1q_u8(a), vld1q_u8(c)));
                    } else {
                        const uint8x16_t a0 = vld1q_u8(a), a1 = vld1q_u8(a + 1), c0 = vld1q_u8(c), c1 = vld1q_u8(c + 1);
                        const uint16x8_t lo = vaddq_u16(vaddq_u16(vaddl_u8(vget_low_u8(a0), vget_low_u8(a1)),
                                                                  vaddl_u8(vget_low_u8(c0), vget_low_u8(c1))), two);
                        const uint16x8_t hi = vaddq_u16(vaddq_u16(vaddl_u8(vget_high_u8(a0), vget_high_u8(a1)),
                                                                  vaddl_u8(vget_high_u8(c0), vget_high_u8(c1))), two);
                        vst1q_u8(o, vcombine_u8(vshrn_n_u16(lo, 2), vshrn_n_u16(hi, 2)));
                    }
                } else {
                    if (!hx && !hy) {
                        vst1_u8(o, vld1_u8(a));
                    } else if (hx && !hy) {
                        vst1_u8(o, vrhadd_u8(vld1_u8(a), vld1_u8(a + 1)));
                    } else if (!hx && hy) {
                        vst1_u8(o, vrhadd_u8(vld1_u8(a), vld1_u8(c)));
                    } else {
                        const uint16x8_t sum = vaddq_u16(vaddq_u16(vaddl_u8(vld1_u8(a), vld1_u8(a + 1)),
                                                                   vaddl_u8(vld1_u8(c), vld1_u8(c + 1))), two);
                        vst1_u8(o, vshrn_n_u16(sum, 2));
                    }
                }
            }
            return;
        }
#endif
        for (int j = 0; j < h; ++j) {
            const std::uint8_t* a = plane + std::size_t(y + j * ystep) * pw + x;
            const std::uint8_t* c = a + std::size_t(ystep) * pw;
            std::uint8_t* o = out + j * out_stride;
            if (!hx && !hy) {
                std::memcpy(o, a, std::size_t(w));
            } else if (hx && !hy) {
                for (int i = 0; i < w; ++i) o[i] = static_cast<std::uint8_t>((a[i] + a[i + 1] + 1) >> 1);
            } else if (!hx && hy) {
                for (int i = 0; i < w; ++i) o[i] = static_cast<std::uint8_t>((a[i] + c[i] + 1) >> 1);
            } else {
                for (int i = 0; i < w; ++i)
                    o[i] = static_cast<std::uint8_t>((a[i] + a[i + 1] + c[i] + c[i + 1] + 2) >> 2);
            }
        }
        return;
    }
#endif
    const int parity = ystep == 2 ? (y & 1) : 0;
    const int row_lo = parity, row_hi = ph - ystep + parity;
    int cols[17 + 1];
    for (int i = 0; i <= w; ++i) cols[i] = inside ? x + i : clamp_int(x + i, 0, pw - 1);
    for (int j = 0; j < h; ++j) {
        int r0 = y + j * ystep, r1 = r0 + ystep;
        if (!inside) {
            r0 = clamp_int(r0, row_lo, row_hi);
            r1 = clamp_int(r1, row_lo, row_hi);
        }
        const std::uint8_t* a = plane + std::size_t(r0) * pw;
        const std::uint8_t* c = plane + std::size_t(r1) * pw;
        std::uint8_t* o = out + j * out_stride;
        if (!hx && !hy) {
            for (int i = 0; i < w; ++i) o[i] = a[cols[i]];
        } else if (hx && !hy) {
            for (int i = 0; i < w; ++i) o[i] = static_cast<std::uint8_t>((a[cols[i]] + a[cols[i + 1]] + 1) >> 1);
        } else if (!hx && hy) {
            for (int i = 0; i < w; ++i) o[i] = static_cast<std::uint8_t>((a[cols[i]] + c[cols[i]] + 1) >> 1);
        } else {
            for (int i = 0; i < w; ++i)
                o[i] = static_cast<std::uint8_t>((a[cols[i]] + a[cols[i + 1]] + c[cols[i]] + c[cols[i + 1]] + 2) >> 2);
        }
    }
}

}  // namespace

std::span<const VlcEntry> table_b1_address_increment() { return b1(); }
std::span<const VlcEntry> table_b9_coded_block_pattern() { return b9(); }
std::span<const VlcEntry> table_b10_motion_code() { return b10(); }
std::span<const VlcEntry> table_b12_dc_size_luma() { return b12(); }
std::span<const VlcEntry> table_b13_dc_size_chroma() { return b13(); }
std::span<const DctEntry> table_b14_dct_zero() { return b14(); }
std::span<const DctEntry> table_b15_dct_one() { return b15(); }
DctSpecialCodes table_b14_special() { return {0x2, 2, 0x1, 6}; }   // EOB "10", escape "0000 01"
DctSpecialCodes table_b15_special() { return {0x6, 4, 0x1, 6}; }   // EOB "0110", escape "0000 01"

// ----------------------------------------------------------------- bit reader
// Reads a unit body followed by at least 16 zero bytes (the prototype's padding).
class Decoder::Bits {
public:
    Bits(const std::uint8_t* data, std::size_t size) : d_(data), size_(size) {}
    std::uint32_t peek(int n) const {
        const std::size_t byte = pos_ >> 3;
        if (byte + 8 > size_ + 16) throw std::runtime_error("MPEG-2: read past the end of a unit");
#if defined(__vita__)
        std::uint64_t v;   // port-only (132vt2a): one unaligned load, byte-swapped (little-endian ARM)
        std::memcpy(&v, d_ + byte, 8);
        v = __builtin_bswap64(v);
#else
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | d_[byte + i];
#endif
        return static_cast<std::uint32_t>((v << (pos_ & 7)) >> (64 - n));
    }
    std::uint32_t get(int n) {
        const auto v = peek(n);
        pos_ += n;
        return v;
    }
    int bit() { return static_cast<int>(get(1)); }
    int sget(int n) {
        const int v = static_cast<int>(get(n));
        return (v >> (n - 1)) ? v - (1 << n) : v;
    }
    int vlc(const Lut& lut) {
        const std::uint32_t e = lut.entries[peek(lut.bits)];
        if (!e) throw std::runtime_error("MPEG-2: invalid VLC at bit " + std::to_string(pos_));
        pos_ += e & 0xff;
        return static_cast<std::int16_t>(e >> 8);
    }
    void skip(int n) { pos_ += n; }
    std::size_t pos() const { return pos_; }
    std::size_t end() const { return size_ * 8; }

private:
    const std::uint8_t* d_;
    std::size_t size_;
    std::size_t pos_{};
};

namespace {
Decoder::SliceRunner& slice_runner() {
    static Decoder::SliceRunner runner;
    return runner;
}

void add_stats(PictureStats& t, const PictureStats& s) {
    t.macroblocks += s.macroblocks; t.skipped += s.skipped;
    t.intra += s.intra; t.field_mc += s.field_mc; t.frame_mc += s.frame_mc;
    t.field_dct += s.field_dct; t.frame_dct += s.frame_dct; t.quant += s.quant;
    t.escapes += s.escapes; t.p_no_mc += s.p_no_mc;
    t.mismatch_toggles += s.mismatch_toggles; t.slices += s.slices;
}
}  // namespace

// ----------------------------------------------------------------- decoder
Decoder::Decoder() {
    std::copy(default_intra_matrix.begin(), default_intra_matrix.end(), intra_matrix_.begin());
    non_intra_matrix_.fill(16);
    for (auto& s : f_code_)
        for (auto& t : s) t = 15;
    (void)lut_b1(); (void)lut_b14(); (void)lut_b15();
}

void Decoder::push(std::span<const std::uint8_t> bytes) {
    if (finished_) throw std::runtime_error("MPEG-2: push after finish");
    constexpr std::size_t npos = static_cast<std::size_t>(-1);
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    const std::size_t n = buffer_.size();
    std::size_t i = scan_;
    while (i + 3 <= n) {
        const auto* p = static_cast<const std::uint8_t*>(
            std::memchr(buffer_.data() + i + 2, 1, n - i - 2));
        if (!p) {
            i = n - 2;
            break;
        }
        const std::size_t one = static_cast<std::size_t>(p - buffer_.data());
        if (buffer_[one - 1] != 0 || buffer_[one - 2] != 0) {
            i = one - 1;
            continue;
        }
        const std::size_t j = one - 2;
        if (unit_start_ != npos) {
            const std::size_t a = unit_start_;
            if (a + 3 >= j) throw std::runtime_error("MPEG-2: empty start code");
            const std::size_t body = j - (a + 4);
            scratch_.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(a + 4),
                            buffer_.begin() + static_cast<std::ptrdiff_t>(j));
            scratch_.resize(body + 16, 0);
            unit(buffer_[a + 3], scratch_.data(), body);
        }
        unit_start_ = j;
        i = j + 3;
    }
    scan_ = i;
    const std::size_t keep = unit_start_ != npos ? unit_start_ : scan_;
    if (keep > (1u << 20)) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(keep));
        scan_ -= keep;
        if (unit_start_ != npos) unit_start_ -= keep;
    }
}

void Decoder::finish() {
    if (finished_) return;
    constexpr std::size_t npos = static_cast<std::size_t>(-1);
    if (unit_start_ != npos) {
        const std::size_t a = unit_start_;
        if (a + 3 >= buffer_.size()) throw std::runtime_error("MPEG-2: truncated start code");
        const std::size_t body = buffer_.size() - (a + 4);
        scratch_.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(a + 4), buffer_.end());
        scratch_.resize(body + 16, 0);
        unit(buffer_[a + 3], scratch_.data(), body);
        unit_start_ = npos;
    }
    if (in_picture_ && !skip_picture_) finish_picture();
    in_picture_ = false;
    if (have_pending_) {
        output_.push_back(std::move(pending_));
        have_pending_ = false;
    }
    buffer_.clear();
    finished_ = true;
}

bool Decoder::pop(Frame& frame) {
    if (output_.empty()) return false;
    frame = std::move(output_.front());
    output_.pop_front();
    return true;
}

void Decoder::unit(std::uint8_t code, const std::uint8_t* body, std::size_t size) {
    if (code == 0xB3) {
        if (!active_ && sequence_index_ == start_sequence_) active_ = true;
        ++sequence_index_;
    }
    if (!active_) return;
    const bool is_slice = code >= 1 && code <= 0xAF;
    if (in_picture_ && !is_slice && skip_picture_) in_picture_ = false;
    if (in_picture_ && !is_slice) {
        finish_picture();
        in_picture_ = false;
    }
    Bits b(body, size);
    if (code == 0xB3) {
        sequence_header(b);
    } else if (code == 0xB5) {
        extension(b);
    } else if (code == 0x00) {
        picture_header(b);
        skip_picture_ = picture_type_ == 3 && !have_forward_ && !closed_gop_;
        if (skip_picture_) ++stats_.skipped_leading_b;
    } else if (code == 0xB8) {
        b.get(25);
        closed_gop_ = b.get(1) != 0;
        ++stats_.gops;
    } else if (is_slice && skip_picture_) {
        in_picture_ = true;
    } else if (is_slice) {
        if (!in_picture_) {
            start_picture();
            in_picture_ = true;
        }
        if (slice_runner()) {
            slices_.push_back({slice_bytes_.size(), size, code - 1});
            slice_bytes_.insert(slice_bytes_.end(), body, body + size);
            slice_bytes_.resize(slice_bytes_.size() + 16, 0);
        } else {
            Slice s;
            slice(b, code - 1, s);
            add_stats(picture_stats_, s.stats);
        }
    }
}

void Decoder::sequence_header(Bits& b) {
    width_ = static_cast<int>(b.get(12));
    height_ = static_cast<int>(b.get(12));
    if (width_ <= 0 || height_ <= 0 || width_ > 1920 || height_ > 1152)
        throw std::runtime_error("MPEG-2: unsupported picture size");
    b.get(4); b.get(4); b.get(18); b.get(1); b.get(10); b.get(1);
    std::copy(default_intra_matrix.begin(), default_intra_matrix.end(), intra_matrix_.begin());
    non_intra_matrix_.fill(16);
    if (b.get(1))
        for (int i = 0; i < 64; ++i) intra_matrix_[zigzag_scan[i]] = static_cast<std::uint8_t>(b.get(8));
    if (b.get(1))
        for (int i = 0; i < 64; ++i) non_intra_matrix_[zigzag_scan[i]] = static_cast<std::uint8_t>(b.get(8));
    mb_width_ = (width_ + 15) / 16;
    mb_height_ = (height_ + 15) / 16;
    have_sequence_ = true;
    ++stats_.sequence_headers;
}

void Decoder::extension(Bits& b) {
    const auto id = b.get(4);
    if (id == 8) {
        f_code_[0][0] = static_cast<int>(b.get(4));
        f_code_[0][1] = static_cast<int>(b.get(4));
        f_code_[1][0] = static_cast<int>(b.get(4));
        f_code_[1][1] = static_cast<int>(b.get(4));
        dc_precision_ = static_cast<int>(b.get(2));
        picture_structure_ = static_cast<int>(b.get(2));
        b.get(1);   // top_field_first
        frame_pred_frame_dct_ = static_cast<int>(b.get(1));
        concealment_ = static_cast<int>(b.get(1));
        q_scale_type_ = static_cast<int>(b.get(1));
        intra_vlc_ = static_cast<int>(b.get(1));
        alternate_ = static_cast<int>(b.get(1));
        if (picture_structure_ != 3) throw std::runtime_error("MPEG-2: field pictures are not used by PSS movies");
        if (concealment_) throw std::runtime_error("MPEG-2: concealment motion vectors are not used by PSS movies");
        have_picture_extension_ = true;
    } else if (id == 3) {
        if (b.get(1))
            for (int i = 0; i < 64; ++i) intra_matrix_[zigzag_scan[i]] = static_cast<std::uint8_t>(b.get(8));
        if (b.get(1))
            for (int i = 0; i < 64; ++i) non_intra_matrix_[zigzag_scan[i]] = static_cast<std::uint8_t>(b.get(8));
        ++stats_.quant_matrix_extensions;
    }
}

void Decoder::picture_header(Bits& b) {
    temporal_reference_ = static_cast<int>(b.get(10));
    picture_type_ = static_cast<int>(b.get(3));
    if (picture_type_ < 1 || picture_type_ > 3) throw std::runtime_error("MPEG-2: unsupported picture type");
    b.get(16);
    if (picture_type_ == 2 || picture_type_ == 3) { b.get(1); b.get(3); }
    if (picture_type_ == 3) { b.get(1); b.get(3); }
    for (auto& s : f_code_)
        for (auto& t : s) t = 15;
    have_picture_extension_ = false;
}

void Decoder::start_picture() {
    if (!have_sequence_) throw std::runtime_error("MPEG-2: slice before a sequence header");
    const std::size_t luma = std::size_t(mb_width_) * 16 * mb_height_ * 16;
    current_.y.assign(luma, 0);
    current_.u.assign(luma / 4, 0);
    current_.v.assign(luma / 4, 0);
    picture_stats_ = {};
    ++decode_index_;
    if (picture_type_ == 1) ++stats_.pictures_i;
    else if (picture_type_ == 2) ++stats_.pictures_p;
    else ++stats_.pictures_b;
}

void Decoder::finish_picture() {
    run_slices();
    Frame f;
    f.width = width_;
    f.height = height_;
    const int pw = mb_width_ * 16;
    f.y.resize(std::size_t(width_) * height_);
    for (int r = 0; r < height_; ++r)
        std::memcpy(f.y.data() + std::size_t(r) * width_, current_.y.data() + std::size_t(r) * pw, std::size_t(width_));
    const int cw = width_ / 2, ch = height_ / 2;
    f.u.resize(std::size_t(cw) * ch);
    f.v.resize(std::size_t(cw) * ch);
    for (int r = 0; r < ch; ++r) {
        std::memcpy(f.u.data() + std::size_t(r) * cw, current_.u.data() + std::size_t(r) * (pw / 2), std::size_t(cw));
        std::memcpy(f.v.data() + std::size_t(r) * cw, current_.v.data() + std::size_t(r) * (pw / 2), std::size_t(cw));
    }
    f.type = "XIPB"[picture_type_];
    f.temporal_reference = temporal_reference_;
    f.decode_index = decode_index_ - 1;
    f.stats = picture_stats_;
    auto& t = stats_.totals;
    t.macroblocks += picture_stats_.macroblocks; t.skipped += picture_stats_.skipped;
    t.intra += picture_stats_.intra; t.field_mc += picture_stats_.field_mc; t.frame_mc += picture_stats_.frame_mc;
    t.field_dct += picture_stats_.field_dct; t.frame_dct += picture_stats_.frame_dct; t.quant += picture_stats_.quant;
    t.escapes += picture_stats_.escapes; t.p_no_mc += picture_stats_.p_no_mc;
    t.mismatch_toggles += picture_stats_.mismatch_toggles; t.slices += picture_stats_.slices;
    if (picture_type_ == 3) {
        output_.push_back(std::move(f));
    } else {
        if (have_pending_) output_.push_back(std::move(pending_));
        pending_ = std::move(f);
        have_pending_ = true;
        std::swap(forward_, backward_);
        have_forward_ = have_backward_;
        std::swap(backward_, current_);
        have_backward_ = true;
    }
}

int Decoder::quantiser_scale(int code) const {
    return q_scale_type_ ? non_linear_quantiser_scale[static_cast<std::size_t>(code)] : code * 2;
}

void Decoder::reset_pmv(Slice& s) { std::memset(s.pmv, 0, sizeof s.pmv); }

void Decoder::set_slice_runner(SliceRunner runner) {
    (void)lut_b2(); (void)lut_b3(); (void)lut_b4(); (void)lut_b9(); (void)lut_b10(); (void)lut_b12(); (void)lut_b13();
    slice_runner() = std::move(runner);
}

// The collected slices through the runner; the first failure in slice order is the one a sequential decode meets.
void Decoder::run_slices() {
    if (slices_.empty()) return;
    const int count = static_cast<int>(slices_.size());
    std::vector<Slice> states(slices_.size());
    std::vector<std::exception_ptr> errors(slices_.size());
    slice_runner()(count, [&](int i) {
        const auto& p = slices_[std::size_t(i)];
        try {
            Bits b(slice_bytes_.data() + p.offset, p.size);
            slice(b, p.row, states[std::size_t(i)]);
        } catch (...) {
            errors[std::size_t(i)] = std::current_exception();
        }
    });
    slices_.clear();
    slice_bytes_.clear();
    for (int i = 0; i < count; ++i) {
        if (errors[std::size_t(i)]) std::rethrow_exception(errors[std::size_t(i)]);
        add_stats(picture_stats_, states[std::size_t(i)].stats);
    }
}

void Decoder::slice(Bits& b, int row, Slice& s) {
    if (!have_picture_extension_) throw std::runtime_error("MPEG-2: slice without a picture coding extension");
    const int qcode = static_cast<int>(b.get(5));
    if (b.peek(1)) throw std::runtime_error("MPEG-2: extra slice information is not used by PSS movies");
    b.get(1);
    s.quantiser = quantiser_scale(qcode);
    const int reset = 128 << dc_precision_;
    s.dc_pred[0] = s.dc_pred[1] = s.dc_pred[2] = reset;
    reset_pmv(s);
    s.prev.valid = false;
    ++s.stats.slices;
    int address = row * mb_width_ - 1;
    bool first = true;
    const std::size_t end = b.end();
    while (true) {
        int increment = 0;
        while (true) {
            const int v = b.vlc(lut_b1());
            if (v == 34) increment += 33;
            else if (v == 35) continue;
            else {
                increment += v;
                break;
            }
        }
        if (first) {
            address += increment;
            first = false;
        } else {
            for (int k = 0; k < increment - 1; ++k) skipped_macroblock(s, ++address);
            ++address;
        }
        macroblock(b, s, address);
        if (b.pos() >= end || b.peek(23) == 0) break;
    }
}

void Decoder::skipped_macroblock(Slice& s, int address) {
    if (address < 0 || address >= mb_width_ * mb_height_) throw std::runtime_error("MPEG-2: macroblock address out of range");
    ++s.stats.skipped;
    const int reset = 128 << dc_precision_;
    s.dc_pred[0] = s.dc_pred[1] = s.dc_pred[2] = reset;
    if (picture_type_ == 2) {
        reset_pmv(s);
        const bool dirs[2]{true, false};
        const int mv[2][2][2]{};
        const int fs[2][2]{};
        predict(address, dirs, 2, mv, fs);
    } else {
        if (!s.prev.valid) throw std::runtime_error("MPEG-2: skipped macroblock without a previous macroblock");
        predict(address, s.prev.dirs, s.prev.motion_type, s.prev.mv, s.prev.field_select);
    }
}

void Decoder::macroblock(Bits& b, Slice& s, int address) {
    if (address < 0 || address >= mb_width_ * mb_height_) throw std::runtime_error("MPEG-2: macroblock address out of range");
    const Lut& types = picture_type_ == 1 ? lut_b2() : (picture_type_ == 2 ? lut_b3() : lut_b4());
    const int type = b.vlc(types);
    const bool quant = type & mb_quant, intra = type & mb_intra, pattern = type & mb_pattern;
    bool forward = type & mb_forward;
    const bool backward = type & mb_backward;
    auto& st = s.stats;
    ++st.macroblocks;
    int motion_type = 2;
    if (forward || backward) {
        if (!frame_pred_frame_dct_) motion_type = static_cast<int>(b.get(2));
        if (motion_type == 0) throw std::runtime_error("MPEG-2: reserved frame_motion_type");
        if (motion_type == 1) ++st.field_mc;
        else if (motion_type == 2) ++st.frame_mc;
    }
    int dct_type = 0;
    if (!frame_pred_frame_dct_ && (intra || pattern)) {
        dct_type = b.bit();
        if (dct_type) ++st.field_dct;
        else ++st.frame_dct;
    }
    if (quant) {
        s.quantiser = quantiser_scale(static_cast<int>(b.get(5)));
        ++st.quant;
    }
    int mv[2][2][2]{};
    int fs[2][2]{};
    if (forward) motion_vectors(b, s, 0, motion_type, mv[0], fs);
    if (backward) motion_vectors(b, s, 1, motion_type, mv[1], fs);
    int cbp;
    if (intra) {
        ++st.intra;
        cbp = 63;
        reset_pmv(s);
        s.prev.valid = false;
    } else {
        const int reset = 128 << dc_precision_;
        s.dc_pred[0] = s.dc_pred[1] = s.dc_pred[2] = reset;
        cbp = pattern ? b.vlc(lut_b9()) : 0;
        if (picture_type_ == 2 && !forward) {
            reset_pmv(s);
            motion_type = 2;
            std::memset(mv, 0, sizeof mv);
            forward = true;
            ++st.p_no_mc;
        }
        s.prev.valid = true;
        s.prev.dirs[0] = forward;
        s.prev.dirs[1] = backward;
        s.prev.motion_type = motion_type;
        std::memcpy(s.prev.mv, mv, sizeof mv);
        std::memcpy(s.prev.field_select, fs, sizeof fs);
        predict(address, s.prev.dirs, motion_type, mv, fs);
    }
    const int mbx = address % mb_width_, mby = address / mb_width_;
    const int pw = mb_width_ * 16;
#if !defined(__ARM_NEON)
    std::int32_t blk[64];
#endif
    for (int i = 0; i < 6; ++i) {
        if (!((cbp >> (5 - i)) & 1)) continue;
        std::uint8_t* dst;
        int stride;
        if (i < 4) {
            const int x = mbx * 16 + (i & 1) * 8;
            if (dct_type) {
                dst = current_.y.data() + std::size_t(mby * 16 + (i >> 1)) * pw + x;
                stride = pw * 2;
            } else {
                dst = current_.y.data() + std::size_t(mby * 16 + (i >> 1) * 8) * pw + x;
                stride = pw;
            }
        } else {
            auto& plane = i == 4 ? current_.u : current_.v;
            dst = plane.data() + std::size_t(mby * 8) * (pw / 2) + mbx * 8;
            stride = pw / 2;
        }
#if defined(__ARM_NEON)
        block(b, s, i, intra, dst, stride);
#else
        block(b, s, i, intra, blk);
        for (int r = 0; r < 8; ++r) {
            std::uint8_t* o = dst + std::size_t(r) * stride;
            const std::int32_t* p = blk + r * 8;
            if (intra)
                for (int c = 0; c < 8; ++c) o[c] = static_cast<std::uint8_t>(clamp_int(p[c], 0, 255));
            else
                for (int c = 0; c < 8; ++c) o[c] = static_cast<std::uint8_t>(clamp_int(o[c] + p[c], 0, 255));
        }
#endif
    }
}

void Decoder::motion_vectors(Bits& b, Slice& sl, int s, int motion_type, int (&mv)[2][2], int (&fs)[2][2]) {
    if (motion_type == 1) {
        for (int r = 0; r < 2; ++r) {
            fs[r][s] = b.bit();
            motion_vector(b, sl, r, s, true, mv[r]);
        }
        return;
    }
    if (motion_type == 3) throw std::runtime_error("MPEG-2: dual-prime prediction is not used by PSS movies");
    motion_vector(b, sl, 0, s, false, mv[0]);
    sl.pmv[1][s][0] = sl.pmv[0][s][0];
    sl.pmv[1][s][1] = sl.pmv[0][s][1];
}

void Decoder::motion_vector(Bits& b, Slice& sl, int r, int s, bool field, int (&vec)[2]) {
    for (int t = 0; t < 2; ++t) {
        int code = b.vlc(lut_b10());
        if (code && b.bit()) code = -code;
        const int fc = f_code_[s][t];
        if (fc < 1 || fc > 9) throw std::runtime_error("MPEG-2: invalid f_code");
        const int rsize = fc - 1;
        const int f = 1 << rsize;
        int delta;
        if (f == 1 || code == 0) {
            delta = code;
        } else {
            const int residual = static_cast<int>(b.get(rsize));
            delta = ((code < 0 ? -code : code) - 1) * f + residual + 1;
            if (code < 0) delta = -delta;
        }
        int pred = sl.pmv[r][s][t];
        if (field && t == 1) pred >>= 1;
        int v = pred + delta;
        const int low = -16 * f, high = 16 * f - 1;
        if (v < low) v += 32 * f;
        else if (v > high) v -= 32 * f;
        sl.pmv[r][s][t] = (field && t == 1) ? v * 2 : v;
        vec[t] = v;
    }
}

// The coefficients of one block (7.2.2, 7.4) and its IDCT, put (intra) or added (non-intra) at dst.
#if defined(__ARM_NEON)
// Port-only (132vt2a2, research/VITA.md "Movies"): the levels dequantised as they are read (the same formulas,
// sums and mismatch control as the generic path, which collects them first), int16 coefficients, the NEON IDCT
// over the rows that hold any, and the clamp to 0..255 by saturating narrows. The same bytes.
void Decoder::block(Bits& b, Slice& s, int index, bool intra, std::uint8_t* dst, int stride) {
    const auto& scan = alternate_ ? alternate_scan : zigzag_scan;
    alignas(16) std::int16_t f[64];
    std::memset(f, 0, sizeof f);
    unsigned rows = 0;
    bool ac = false;
    int total = 0;
    const int q = s.quantiser;
    const DctLut* lut = intra ? (intra_vlc_ ? &lut_b15() : &lut_b14()) : &lut_b14();
    const std::uint8_t* w = intra ? intra_matrix_.data() : non_intra_matrix_.data();
    const auto coefficient = [&](int k, int level) {
        if (!level) return;
        const int pos = scan[static_cast<std::size_t>(k)];
        int value;
        if (intra && k == 0) {
            value = level * (8 >> dc_precision_);
        } else {
            const int sign = intra ? 0 : (level > 0 ? 1 : -1);
            value = (2 * level + sign) * w[pos] * q / 32;
        }
        value = clamp_int(value, -2048, 2047);
        f[pos] = static_cast<std::int16_t>(value);
        total += value;
        rows |= 1u << (pos >> 3);
        ac |= pos != 0;
    };
    int n = 0;
    if (intra) {
        const int cc = index < 4 ? 0 : index - 3;
        const int size = b.vlc(cc == 0 ? lut_b12() : lut_b13());
        int diff = 0;
        if (size) {
            const int d = static_cast<int>(b.get(size));
            diff = (d >> (size - 1)) ? d : d - (1 << size) + 1;
        }
        s.dc_pred[cc] += diff;
        coefficient(0, s.dc_pred[cc]);
        n = 1;
    } else if (b.peek(1)) {
        b.skip(1);
        coefficient(0, b.bit() ? -1 : 1);
        n = 1;
    }
    while (true) {
        const std::uint32_t e = lut->entries[b.peek(16)];
        if (!e) throw std::runtime_error("MPEG-2: invalid DCT coefficient VLC at bit " + std::to_string(b.pos()));
        b.skip(static_cast<int>(e >> 24));
        const std::uint32_t kind = (e >> 16) & 0xff;
        if (kind == kind_eob) break;
        int run, level;
        if (kind == kind_escape) {
            run = static_cast<int>(b.get(6));
            level = b.sget(12);
            ++s.stats.escapes;
        } else {
            run = static_cast<int>(e & 0xff);
            level = static_cast<int>((e >> 8) & 0xff);
            if (b.bit()) level = -level;
        }
        n += run;
        if (n > 63) throw std::runtime_error("MPEG-2: DCT coefficient index overflow");
        coefficient(n, level);
        ++n;
    }
    if (!(total & 1)) {
        f[63] = static_cast<std::int16_t>(f[63] + ((f[63] & 1) ? -1 : 1));
        ++s.stats.mismatch_toggles;
        rows |= 0x80;
        ac = true;
    }
    if (!ac) {   // idct_8x8_fast's DC-only case: one value
        const std::int64_t sum = std::int64_t(idct_basis[0][0]) * (f[0] * idct_basis[0][0]);
        const int v = static_cast<int>(std::clamp<std::int64_t>((sum + (std::int64_t(1) << 31)) >> 32, -256, 255));
        if (intra) {
            const uint8x8_t p = vdup_n_u8(static_cast<std::uint8_t>(clamp_int(v, 0, 255)));
            for (int r = 0; r < 8; ++r) vst1_u8(dst + std::size_t(r) * stride, p);
        } else if (v) {
            const int16x8_t d = vdupq_n_s16(static_cast<std::int16_t>(v));
            for (int r = 0; r < 8; ++r) {
                std::uint8_t* o = dst + std::size_t(r) * stride;
                vst1_u8(o, vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(o))), d)));
            }
        }
        return;
    }
    int16x8_t out[8];
    idct_neon(f, rows, out);
    for (int r = 0; r < 8; ++r) {
        std::uint8_t* o = dst + std::size_t(r) * stride;
        if (intra) vst1_u8(o, vqmovun_s16(out[r]));
        else vst1_u8(o, vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(o))), out[r])));
    }
}
#else
void Decoder::block(Bits& b, Slice& s, int index, bool intra, std::int32_t* out) {
    const auto& scan = alternate_ ? alternate_scan : zigzag_scan;
    int qf[64]{};
    int n = 0;
    const DctLut* lut;
    const std::uint8_t* w;
    if (intra) {
        const int cc = index < 4 ? 0 : index - 3;
        const int size = b.vlc(cc == 0 ? lut_b12() : lut_b13());
        int diff = 0;
        if (size) {
            const int d = static_cast<int>(b.get(size));
            diff = (d >> (size - 1)) ? d : d - (1 << size) + 1;
        }
        s.dc_pred[cc] += diff;
        qf[0] = s.dc_pred[cc];
        n = 1;
        lut = intra_vlc_ ? &lut_b15() : &lut_b14();
        w = intra_matrix_.data();
    } else {
        lut = &lut_b14();
        w = non_intra_matrix_.data();
        if (b.peek(1)) {
            b.skip(1);
            qf[0] = b.bit() ? -1 : 1;
            n = 1;
        }
    }
    while (true) {
        const std::uint32_t e = lut->entries[b.peek(16)];
        if (!e) throw std::runtime_error("MPEG-2: invalid DCT coefficient VLC at bit " + std::to_string(b.pos()));
        b.skip(static_cast<int>(e >> 24));
        const std::uint32_t kind = (e >> 16) & 0xff;
        if (kind == kind_eob) break;
        int run, level;
        if (kind == kind_escape) {
            run = static_cast<int>(b.get(6));
            level = b.sget(12);
            ++s.stats.escapes;
        } else {
            run = static_cast<int>(e & 0xff);
            level = static_cast<int>((e >> 8) & 0xff);
            if (b.bit()) level = -level;
        }
        n += run;
        if (n > 63) throw std::runtime_error("MPEG-2: DCT coefficient index overflow");
        qf[n] = level;
        ++n;
    }
    std::int32_t f[64]{};
    const int q = s.quantiser;
    int total = 0;
    for (int k = 0; k < 64; ++k) {
        const int level = qf[k];
        if (!level) continue;
        const int pos = scan[static_cast<std::size_t>(k)];
        int value;
        if (intra && k == 0) {
            value = level * (8 >> dc_precision_);
        } else {
            const int sign = intra ? 0 : (level > 0 ? 1 : -1);
            value = (2 * level + sign) * w[pos] * q / 32;
        }
        value = clamp_int(value, -2048, 2047);
        f[pos] = value;
        total += value;
    }
    if (!(total & 1)) {
        f[63] += (f[63] & 1) ? -1 : 1;
        ++s.stats.mismatch_toggles;
    }
    idct_8x8(f, out);
}
#endif

void Decoder::predict(int address, const bool (&dirs)[2], int motion_type, const int (&mv)[2][2][2],
                      const int (&fs)[2][2]) {
    const int mbx = address % mb_width_, mby = address / mb_width_;
    const int pw = mb_width_ * 16, ph = mb_height_ * 16;
    const int cw = pw / 2, chh = ph / 2;
    std::uint8_t pred[2][384];
    int count = 0;
    for (int s = 0; s < 2; ++s) {
        if (!dirs[s]) continue;
        const Planes* ref;
        bool have;
        if (s == 0 && picture_type_ == 2) { ref = &backward_; have = have_backward_; }
        else if (s == 0) { ref = &forward_; have = have_forward_; }
        else { ref = &backward_; have = have_backward_; }
        if (!have) throw std::runtime_error("MPEG-2: missing reference picture");
        std::uint8_t* py = pred[count];
        std::uint8_t* pu = py + 256;
        std::uint8_t* pv = pu + 64;
        if (motion_type == 2) {
            const int mx = mv[s][0][0], my = mv[s][0][1];
            half_pel(ref->y.data(), pw, ph, mbx * 16 + (mx >> 1), mby * 16 + (my >> 1), 16, 16, mx & 1, my & 1, 1, py, 16);
            const int cx = mx / 2, cy = my / 2;
            half_pel(ref->u.data(), cw, chh, mbx * 8 + (cx >> 1), mby * 8 + (cy >> 1), 8, 8, cx & 1, cy & 1, 1, pu, 8);
            half_pel(ref->v.data(), cw, chh, mbx * 8 + (cx >> 1), mby * 8 + (cy >> 1), 8, 8, cx & 1, cy & 1, 1, pv, 8);
        } else {
            for (int r = 0; r < 2; ++r) {   // r = 0: top-field lines of the macroblock, 1: bottom
                const int mx = mv[s][r][0], my = mv[s][r][1];
                const int sel = fs[r][s];
                const int fy = mby * 8 + (my >> 1);
                half_pel(ref->y.data(), pw, ph, mbx * 16 + (mx >> 1), 2 * fy + sel, 16, 8, mx & 1, my & 1, 2,
                         py + r * 16, 32);
                const int cx = mx / 2, cy = my / 2;
                const int fyc = mby * 4 + (cy >> 1);
                half_pel(ref->u.data(), cw, chh, mbx * 8 + (cx >> 1), 2 * fyc + sel, 8, 4, cx & 1, cy & 1, 2,
                         pu + r * 8, 16);
                half_pel(ref->v.data(), cw, chh, mbx * 8 + (cx >> 1), 2 * fyc + sel, 8, 4, cx & 1, cy & 1, 2,
                         pv + r * 8, 16);
            }
        }
        ++count;
    }
    if (count == 2) {
#if defined(__ARM_NEON)
        for (int i = 0; i < 384; i += 16)   // port-only (132vt2a2): the same rounded average
            vst1q_u8(pred[0] + i, vrhaddq_u8(vld1q_u8(pred[0] + i), vld1q_u8(pred[1] + i)));
#else
        for (int i = 0; i < 384; ++i) pred[0][i] = static_cast<std::uint8_t>((pred[0][i] + pred[1][i] + 1) >> 1);
#endif
    }
    const std::uint8_t* p = pred[0];
    for (int r = 0; r < 16; ++r)
        std::memcpy(current_.y.data() + std::size_t(mby * 16 + r) * pw + mbx * 16, p + r * 16, 16);
    for (int r = 0; r < 8; ++r) {
        std::memcpy(current_.u.data() + std::size_t(mby * 8 + r) * cw + mbx * 8, p + 256 + r * 8, 8);
        std::memcpy(current_.v.data() + std::size_t(mby * 8 + r) * cw + mbx * 8, p + 320 + r * 8, 8);
    }
}

}  // namespace omega::mpeg2
