#pragma once
// Port-only (132ps5c, research/PS5.md "Renderer"): the PS5 backend of the renderer abstraction (gfx_device.hpp) on the
// PS5 OpenGL core driver (Mesa over AGC; since 132ps5d cybervisi0n/ps5-gl-runtime: a 4.6 context, else 3.3) with an
// EGL window surface (1920 x 1080 by default). The
// programs are the shared Cg sources of gfx_gxm_shaders.cpp converted to GLSL (4.60 / 3.30) when the device starts. The PS5
// app (src/port/ps5/ps5_main.cpp) makes the display; the play host draws into it through host_window_ps5.hpp.
#include "gfx_device.hpp"

#include <functional>
#include <string>

namespace omega::gfx::gl46 {

struct Options {
    // Run on the presenting thread after each swap (the frame-rate log).
    std::function<void()> presented;
    // The driver's compiled-shader cache (PS5_SHADER_CACHE_DIR).
    std::string shader_cache = "/download0/shadercache";
    // GL_KHR_debug messages into the log (the first 200). Off by default (132ps5c hw2: not yet run on the PS5 driver;
    // the PS5 app turns it on with <working folder>/gl_debug.txt).
    bool debug_output = false;
};

// The one EGL display, context and window surface (made once per process on the backend's GL thread) and its swap
// chain. Throws std::runtime_error naming the failed EGL / GL step.
Display create_display(const Options& options = {});
// One line of the backend's state for the logs: objects and bytes held, draws and presents, the program failures.
std::string stats_line();

}  // namespace omega::gfx::gl46
