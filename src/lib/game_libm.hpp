#pragma once
// The executable's single-precision libm routines used by gameplay code
// (newlib fdlibm as linked; constants are the executable's): atan2f
// 00121ab8 -> 0011d7a0, atanf 001212e0, tanf 00121a10 (kernel 001208f0),
// sinf 00121928 and cosf 00121688 (kernels 001207e8 / 0011fe30), with the
// argument reduction 0011e950 (general path 0011ec28 -> __kernel_rem_pio2f
// 0011ff80, scalbnf 00121860), acosf 00121a88 -> 0011cfd8, asinf 00121aa0 -> 0011d400, powf 00121b30 -> 0011e170. Bit-exact with the EE interpreter (IEEE
// binary32, round to nearest, results that would be denormal flushed to zero
// like the EE FPU, division by zero giving +-FLT_MAX). Nonfinite arguments
// throw, as do acosf / asinf arguments outside [-1, 1].
namespace omega {
float game_acosf(float x);
float game_asinf(float x);
float game_atan2f(float y,float x);
float game_atanf(float x);
float game_tanf(float x);
float game_sinf(float x);
float game_cosf(float x);
// powf: overflow and subnormal x throw (the EE clamps, the interpreter does not).
float game_powf(float x,float y);
}
