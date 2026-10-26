#pragma once
#include <cstdint>
namespace omega {
// The PS2 EE FPU and VU have no denormals: results that would be denormal
// become signed zero and denormal operands are treated as zero. Executables
// that run long gameplay sequences call this once on their (single) thread so
// the host's binary32 arithmetic flushes the same way (x86 MXCSR FTZ|DAZ,
// ARM FPSCR/FPCR FZ). Unit tests keep the default environment so their
// explicit subnormal-domain checks stay observable.
void enable_ps2_denormal_flush();

// Port-only (132vs3, research/VITA.md "In-level sim speed (132vs3)"): the calling thread's floating-point control
// mode (rounding, the denormal flush; not the sticky exception flags), and a scope that gives the calling thread
// another thread's mode until it ends. WorkerPool runs every job under its caller's mode, so a computation moved
// onto the workers flushes denormals as it did on the simulation thread.
std::uint64_t float_controls();
class FloatControlsScope {
public:
    explicit FloatControlsScope(std::uint64_t controls);
    ~FloatControlsScope();
    FloatControlsScope(const FloatControlsScope&)=delete;
    FloatControlsScope& operator=(const FloatControlsScope&)=delete;
private:
    std::uint64_t saved_;
    bool changed_;
};
}
