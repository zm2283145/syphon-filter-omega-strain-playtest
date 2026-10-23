#pragma once
// Port-only (checkpoint 132vt1, research/VITA.md "Toolchain"): force-included into every PS Vita (VitaSDK,
// arm-vita-eabi-gcc + newlib) translation unit by cmake/vita/OmegaVita.cmake; never part of the Windows build.
// It only supplies the MSVC CRT names the shared code uses, with the same results for the uses it has.

#if defined(__vita__) && defined(__cplusplus)

#include <cstdio>

// The shared code calls sscanf_s with numeric conversions only (%f, %d: gui_runtime.cpp's subtitle tags), for which
// the secure variant takes the same arguments and gives the same results as sscanf (no %s / %c / %[ buffer sizes).
#ifndef sscanf_s
#define sscanf_s std::sscanf
#endif

// newlib's <sys/socket.h> has no SOMAXCONN (online_tcp.cpp's listen backlog, 132on2h). 128 is the BSD default
// (port-only; the listen backlog is only a hint).
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

#endif
