#pragma once
namespace omega {
// The PS2 EE FPU and VU have no denormals: results that would be denormal
// become signed zero and denormal operands are treated as zero. Executables
// that run long gameplay sequences call this once on their (single) thread so
// the host's binary32 arithmetic flushes the same way (x86 MXCSR FTZ|DAZ,
// ARM FPSCR/FPCR FZ). Unit tests keep the default environment so their
// explicit subnormal-domain checks stay observable.
void enable_ps2_denormal_flush();
}
