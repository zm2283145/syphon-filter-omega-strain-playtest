#pragma once
// Port-only (checkpoint 132vt1): the PS Vita GXM backend of the renderer abstraction (gfx_device.hpp). research/VITA.md
// "Renderer" and research/RENDERER.md "The GXM backend" describe the design: sceGxm directly (no GL layer), explicit
// CDRAM / LPDDR placement, the 11 programs as Cg compiled at first use by SceShaccCg (libshacccg.suprx) with the
// GXP binaries cached on ux0:data.
#include "gfx_device.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace omega::gfx::gxm {

struct Options {
    unsigned display_buffers = 3;              // 2 or 3 (960x544, CDRAM)
    std::size_t cdram_pool_bytes = 96u << 20;  // textures and render targets (less when CDRAM is short)
    std::size_t lpddr_pool_bytes = 48u << 20;  // vertex / index buffers, staging, per-frame transient copies
    std::string shader_cache = "ux0:data/omega_strain/shader_cache";
    std::string compiler = "ur0:data/libshacccg.suprx";
    std::function<void(const std::string&)> log;  // diagnostics (program parameters, the first draws)
    // Port-only test switch (research/VITA.md "Widescreen"): vitaGL's whole-display copies read back on the CPU (the
    // 132vt1 path) instead of drawing on the GPU.
    bool display_copy_readback = false;
    // Port-only test switch (the same section): the GPU copy's row order given (1 rows as read, 2 reversed) instead of
    // checked against the read-back on its first use; 0 checks.
    int display_copy_order = 0;
};

// The memory the backend holds (research/VITA.md "Memory").
struct MemoryStats {
    std::size_t cdram_pool = 0, cdram_used = 0, cdram_peak = 0;
    std::size_t lpddr_pool = 0, lpddr_used = 0, lpddr_peak = 0;
    std::size_t transient_bytes = 0;  // the per-frame copy arenas (inside the LPDDR pool)
    std::size_t fixed_bytes = 0;      // rings, patcher memory, display buffers
    unsigned scenes_last_frame = 0, draws_last_frame = 0;
    unsigned programs_compiled = 0, programs_cached = 0;
};

// The one GXM device (sceGxmInitialize once per process) and its display swap chain. Throws std::runtime_error
// naming the failed sce call; a missing libshacccg.suprx throws ShaderCompilerMissing at the first program.
Display create_display(const Options& options = {});
MemoryStats memory_stats();
// A system message dialog (SceMsgDialog drawn by SceCommonDialog over the display) shown until the user closes it:
// the fatal-error path (e.g. ShaderCompilerMissing), which needs no shader. Needs create_display first.
void show_message(const std::string& text);

struct ShaderCompilerMissing : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace omega::gfx::gxm
