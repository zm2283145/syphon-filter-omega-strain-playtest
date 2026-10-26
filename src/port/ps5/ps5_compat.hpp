#pragma once
// Port-only (132ps5c, research/PS5.md "Build"): force-included into every PS5 translation unit by
// cmake/ps5/OmegaPs5.cmake; never part of the Windows or Vita builds. It only supplies the MSVC CRT names the shared
// code uses that the PS5 SDK's C library lacks, with the same results for the uses it has.

// OMEGA_PS5_NO_COMPAT: a file that must see no C library declarations (ps5_imports.cpp).
#if defined(OMEGA_PS5) && defined(__cplusplus) && !defined(OMEGA_PS5_NO_COMPAT)

#include <cstdio>

// The shared code calls sscanf_s with numeric conversions only (%f, %d: gui_runtime.cpp's subtitle tags), for which
// the secure variant takes the same arguments and gives the same results as sscanf (no %s / %c / %[ buffer sizes).
#ifndef sscanf_s
#define sscanf_s std::sscanf
#endif

#endif