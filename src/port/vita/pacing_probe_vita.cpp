// Port-only (checkpoint 132vt2): the pacing probe (--pacing-probe, pacing_probe.cpp) measures a Windows swap chain against
// the DWM's vertical blanks; the PS Vita build has neither (its display paces on sceDisplayWaitVblankStart).

#include "pacing_probe.hpp"

#include <stdexcept>

namespace omega {

int run_pacing_probe(const PacingProbeOptions&) {
    throw std::runtime_error("--pacing-probe measures the Windows swap chain against the DWM (pacing_probe.cpp); not on "
                             "the PS Vita");
}

}  // namespace omega
