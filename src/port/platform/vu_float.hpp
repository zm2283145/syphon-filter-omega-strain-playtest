#pragma once
#include <cmath>

namespace omega::vu {
// Reference forms of the PS2 divide/square-root instructions as the project's
// EE interpreter models them (research note VU_ROUNDING.md). Each is one
// binary32 square root of the magnitude and/or one binary32 division, in that
// order; no wider intermediate is kept. A reciprocal formed as a double and
// rounded once, or a direct division by the length in place of multiplying by
// the reciprocal, differs from these by an ulp on some inputs.
//
// Callers validate their own domain: these helpers do not model the zero
// divisor or denormal cases (the interpreter returns the largest float there
// and flushes denormal results to zero).

// VU SQRT Q,ft / FPU sqrt.s: binary32 square root of |x|.
inline float sqrt(float x) { return std::sqrt(std::fabs(x)); }

// VU DIV Q,fs,ft / FPU div.s: one binary32 division.
inline float div(float numerator, float denominator) { return numerator / denominator; }

// VU RSQRT Q,fs,ft (and FPU rsqrt.s, modelled identically): binary32 square
// root of |radicand|, then binary32 division of the numerator by it. The usual
// normalization uses vf0.w = 1 as the numerator.
inline float rsqrt(float numerator, float radicand) { return numerator / std::sqrt(std::fabs(radicand)); }
inline float rsqrt(float radicand) { return rsqrt(1.0F, radicand); }
}
