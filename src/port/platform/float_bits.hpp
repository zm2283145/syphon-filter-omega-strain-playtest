#pragma once
// Port-side speed only (checkpoint 132win10, not a game behaviour): the floating-point class tests on the bits.
// MSVC's std::isfinite / std::fpclassify are out-of-line ucrtbase fdclass calls, which the per-actor arithmetic
// guards (the interpreter-style checked add / sub / mul helpers that throw on a nonfinite result) make millions of
// times a second; these give the same answers inline (IEEE-754 binary32 / binary64; -0 and +0 are zero).
#include <bit>
#include <cstdint>

namespace omega::float_bits {

inline constexpr bool finite(float x) { return (std::bit_cast<std::uint32_t>(x) & 0x7f800000U) != 0x7f800000U; }
inline constexpr bool finite(double x) {
    return (std::bit_cast<std::uint64_t>(x) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}
inline constexpr bool subnormal(float x) {
    const std::uint32_t b = std::bit_cast<std::uint32_t>(x);
    return (b & 0x7f800000U) == 0 && (b & 0x007fffffU) != 0;
}
inline constexpr bool subnormal(double x) {
    const std::uint64_t b = std::bit_cast<std::uint64_t>(x);
    return (b & 0x7ff0000000000000ULL) == 0 && (b & 0x000fffffffffffffULL) != 0;
}
// !std::isfinite(x) || std::fpclassify(x) == FP_SUBNORMAL
inline constexpr bool nonfinite_or_subnormal(float x) { return !finite(x) || subnormal(x); }
inline constexpr bool nonfinite_or_subnormal(double x) { return !finite(x) || subnormal(x); }

}  // namespace omega::float_bits
