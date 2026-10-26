// Port-only (132ps5c): the pacing probe (--pacing-probe, pacing_probe.cpp) measures a Windows swap chain against the
// DWM's vertical blanks; the PS5 build has neither (its presents pace on eglSwapBuffers' vertical blank).

#include "pacing_probe.hpp"

#include <stdexcept>

namespace omega {

int run_pacing_probe(const PacingProbeOptions&) {
    throw std::runtime_error("--pacing-probe measures the Windows swap chain against the DWM (pacing_probe.cpp); not on "
                             "the PS5");
}

}  // namespace omega