#pragma once
// Port-only (checkpoint 132vt1): the PS Vita GXM backend of the renderer abstraction (gfx_device.hpp). research/VITA.md
// "Renderer" and research/RENDERER.md "The GXM backend" describe the design: sceGxm directly (no GL layer), explicit
// CDRAM / LPDDR placement, the 11 programs as Cg compiled at first use by SceShaccCg (libshacccg.suprx) with the
// GXP binaries cached on ux0:data.
#include "gfx_device.hpp"

#include <atomic>
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
    // Port-only (checkpoint 132vt2): run on the presenting thread after each swap (the frame-rate log); the vitaGL
    // backend calls it, the GXM backend does not yet.
    std::function<void()> presented;
    // Port-only (132vt2e, research/VITA.md "Draw batching"): the vitaGL backend sets only the draw state that changed.
    // False (test switch ux0:data/omega_strain/vita_test_gl_state_cache_off.txt) sets all of it every draw.
    bool gl_state_cache = true;
    // Port-only (132vt2e, research/VITA.md "Depth across scenes"): the display's and the framebuffer objects' depth /
    // stencil surfaces are loaded at each scene's start and stored at its end, so a scene break inside a frame (a copy
    // out of the display, a target switch) keeps the depth drawn before it. False (test switch
    // ux0:data/omega_strain/vita_test_depth_keep_off.txt) leaves vitaGL's default (depth cleared at every scene).
    bool keep_depth = true;
    // Port-only (132vt4, research/VITA.md "Shadow filter cache"): the vitaGL backend's shadow filter reads each
    // receiver draw's triangles from a copy in cached RAM made at the range's first use (dropped when the bytes are
    // written). False (test switch ux0:data/omega_strain/vita_test_shadow_cache_off.txt) reads the buffers every draw.
    bool shadow_filter_cache = true;
    // Port-only (132vt4): test switch ux0:data/omega_strain/vita_test_shadow_cache_check.txt compares each cache hit
    // with the buffers' bytes (MemoryStats::shadow_checked / shadow_mismatched).
    bool shadow_filter_check = false;
    // Port-only (132vg2, research/VITA.md "Vita render path (132vg2)"), GXM: each scene ends with a fragment
    // notification of its own and a waiter thread timestamps its completion (MemoryStats::GxmTimers' GPU fields,
    // scene_dump()). False (test switch ux0:data/omega_strain/vita_test_gpu_timing_off.txt) leaves only the present's.
    bool gpu_timing = true;
    // Port-only (132vg2, the same section), GXM: sampled-only textures are stored swizzled once a start-up self test
    // confirms the layout. True (test switch ux0:data/omega_strain/vita_test_linear_textures.txt) keeps every
    // texture linear (the A/B).
    bool linear_textures = false;
    // Port-only (132vg2), GXM: a partial update of a large buffer the GPU may still read goes to a patch (no GPU wait).
    // False (test switch ux0:data/omega_strain/vita_test_patches_off.txt) waits for the GPU and writes in place, as
    // before 132vg2 (the A/B).
    bool large_buffer_patches = true;
    // Port-only (132vg2), GXM: scene draws without an alpha test use scene_ps compiled without it (no discard in the
    // program, so the GPU's hidden-surface removal is not defeated). False (test switch
    // ux0:data/omega_strain/vita_test_alpha_variant_off.txt) draws every scene draw with the full program (the A/B).
    bool alpha_test_variant = true;
    // Port-only (132vg2 step 3d), GXM: a scene draw whose alpha test cannot fail (its vertices' colour alpha range
    // times its texture's alpha range clears the test's reference) also uses that variant: the same pixels, no
    // discard. False (test switch ux0:data/omega_strain/vita_test_alpha_proof_off.txt) keeps the full program for
    // every draw with an alpha test (the A/B).
    bool alpha_proof = true;
    // Port-only (132vg2 step 3e), GXM: a patched shadow receiver's triangle filter tests each vertex once and each
    // triangle by its vertices' flags (the same tests). False (test switch
    // ux0:data/omega_strain/vita_test_shadow_flags_off.txt) gathers 12 floats a triangle as before (the A/B).
    bool shadow_vertex_flags = true;
    // step 3e: also run the per-triangle gather on each flagged draw and log mismatches (test switch
    // ux0:data/omega_strain/vita_test_shadow_flags_check.txt).
    bool shadow_flags_check = false;
    // Port-only (132vg2 step 3f), GXM: a scene draw whose SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend cannot change a pixel
    // (its output alpha is provably 1.0) is drawn without blending. False (test switch
    // ux0:data/omega_strain/vita_test_blend_proof_off.txt) keeps every blend (the A/B).
    bool blend_proof = true;
    // step 3g (diagnostic): count why scene draws stay blended (test switch ux0:data/omega_strain/vita_test_census.txt).
    bool census = false;
    // Port-only (132vg2 step 3g), GXM: the alpha and blend proofs also cover patched draws (the moving level
    // objects), their vertex alpha read from the patch's cached-RAM copy each draw. False (test switch
    // ux0:data/omega_strain/vita_test_patched_proofs_off.txt) leaves patched draws unproven (the A/B).
    bool patched_proofs = true;
};

// Port-only (132vg2, research/VITA.md "Vita render path (132vg2)"), GXM backend: its timers since the start
// (microseconds, the render thread's clock); the play log prints the deltas per window. The vitaGL backend leaves
// them zero.
struct GxmTimers {
    std::uint64_t submits = 0, submit_us = 0;          // Device::submit (a whole command list)
    std::uint64_t scenes = 0, begin_scene_us = 0, end_scene_us = 0;  // sceGxmBeginScene + scene state; EndScene
    std::uint64_t clears = 0, clear_us = 0, copies = 0, copy_us = 0;  // the quads of clears and copies
    std::uint64_t updates = 0, update_us = 0, update_bytes = 0;  // vertex / index buffer and texture writes
    std::uint64_t constant_updates = 0;                // constant buffer writes (a CPU copy, untimed)
    std::uint64_t display_queue_us = 0, present_wait_us = 0;  // sceGxmDisplayQueueAddEntry; the frames-in-flight wait
    std::uint64_t drains = 0, drain_us = 0;            // sceGxmFinish waits (large in-place updates, read-backs)
    std::uint64_t shadow_draws = 0, shadow_filter_us = 0;  // shadow_receiver's CPU triangle filter
    // One draw in 8 (draw calls of the API, not the clear / copy quads), split: the whole draw, then inside it the
    // programs (lookups and sceGxmSet*Program), the state (viewport, clip, cull, depth), the 4 textures, the
    // uniforms, sceGxmSetVertexStream + sceGxmDraw.
    std::uint64_t sampled = 0, sample_total_us = 0, sample_program_us = 0, sample_state_us = 0, sample_texture_us = 0,
                  sample_uniform_us = 0, sample_draw_us = 0;
    // 132vg2 per-draw cuts: the GXM calls skipped because the state they set was unchanged.
    std::uint64_t state_calls = 0, state_skipped = 0, texture_slots_skipped = 0, program_cache_hits = 0;
    // 132vg2: large-buffer patches (gfx_gxm.cpp GxmBuffer::Patch): the partial updates made patches, their bytes and
    // the bytes filled from older patches / the buffer, the draws that read a patch, and the folds back into the buffer
    // (each a GPU wait).
    std::uint64_t patch_updates = 0, patch_bytes = 0, patch_fill_bytes = 0, patch_draws = 0, patch_folds = 0;
    std::uint64_t patch_widened = 0;  // patches widened over a draw that read a patch and bytes beside it
    std::uint64_t rebased_lists = 0;  // patched draws' index lists rebased (made once per range and patch start)
    std::uint64_t no_alpha_test_draws = 0;  // scene draws with scene_ps's no-alpha-test variant
    // step 3d: draws with an alpha test proven never to discard (drawn with the variant too; not in the count above),
    // and the vertex ranges read to prove it (once per range)
    std::uint64_t alpha_proven_draws = 0, alpha_scans = 0;
    std::uint64_t shadow_flagged_draws = 0;  // step 3e: receiver draws filtered by per-vertex flags
    // step 3f: scene_ps draws executed, those blended, those with discard (the full program), those into the display
    std::uint64_t scene_draws = 0, scene_blended = 0, scene_discard = 0, scene_display = 0;
    std::uint64_t blend_proven_draws = 0;  // step 3f: scene draws whose blend was proven a no-op (drawn unblended)
    std::uint64_t census[10]{};            // step 3g: gfx_gxm.cpp census_blended
    std::uint64_t alpha_patched_reads = 0;  // step 3g: patched draws' vertex alpha ranges read (each draw)
    std::uint64_t alpha_skipped_draws = 0;  // step 3g: scene draws skipped: their alpha test discards every pixel
    // Night-vision ghosts use the man draw's GEQUAL / RGB_ONLY pair. When the uniform ghost alpha is below AREF,
    // every pixel fails: GXM skips the empty depth-writing pass and draws the RGB_ONLY pass with a no-discard shader
    // that retains the GS alpha quantisation. These counters are the hardware A/B evidence.
    std::uint64_t man_alpha_fail_skipped = 0, man_alpha_fail_draws = 0;
    std::uint64_t patches_live = 0;   // a gauge: the patches of the last buffer patched
    // 132vg2 step 3b: the updates split: the patch path (all), patches written in place, the time allocating new
    // patches and writing GPU memory, new patches that landed in CDRAM; the other buffers' fresh copies (count, bytes
    // copied from the old block, time) and the plain writes' time.
    std::uint64_t upd_patch_us = 0, upd_patch_in_place = 0, upd_patch_alloc_us = 0, upd_patch_gpu_write_us = 0,
                  upd_patch_in_cdram = 0, upd_fresh = 0, upd_fresh_copied = 0, upd_fresh_us = 0, upd_write_us = 0,
                  upd_patch_rotated = 0;  // step 3b: patches given their next GPU copy (a spare or a new one)
    // GPU: the scenes whose completion was timed, and the GPU's busy time (each scene from its kick or the previous
    // scene's end, whichever is later, to its fragment notification) by the kind of target.
    enum GpuKind : unsigned { gpu_display, gpu_colour_depth, gpu_colour, gpu_float, gpu_depth_only, gpu_kinds };
    std::uint64_t gpu_scenes = 0, gpu_busy_us = 0, gpu_resyncs = 0;
    std::uint64_t gpu_kind_scenes[gpu_kinds]{}, gpu_kind_us[gpu_kinds]{};
};
inline const char* gpu_kind_name(unsigned kind) {
    static const char* const names[]{"display", "colour+depth", "colour", "float", "depth only"};
    return kind < GxmTimers::gpu_kinds ? names[kind] : "?";
}

// The memory the backend holds (research/VITA.md "Memory").
struct MemoryStats {
    std::size_t cdram_pool = 0, cdram_used = 0, cdram_peak = 0;
    std::size_t lpddr_pool = 0, lpddr_used = 0, lpddr_peak = 0;
    std::size_t transient_bytes = 0;  // the per-frame copy arenas (inside the LPDDR pool)
    std::size_t fixed_bytes = 0;      // rings, patcher memory, display buffers
    unsigned scenes_last_frame = 0, draws_last_frame = 0;
    std::uint64_t scenes_total = 0, draws_total = 0;   // 132vt2e: since the start (the play log's draws per frame)
    std::uint64_t gl_state_set = 0, gl_state_skipped = 0;  // 132vt2e, vitaGL: draw state groups set / found unchanged
    // 132vt2e, vitaGL: triangles drawn; the shadow receivers' triangles tested on the CPU and those kept.
    std::uint64_t triangles_total = 0, shadow_tested = 0, shadow_kept = 0;
    // 132vt4, vitaGL: the tested triangles read from the shadow filter cache, and the cache's bytes; with
    // Options::shadow_filter_check, the cache hits found equal to the buffers and those not.
    std::uint64_t shadow_cached = 0, shadow_checked = 0, shadow_mismatched = 0;
    std::size_t shadow_cache_bytes = 0;
    // 132vt2e: buffer updates: those that gave the buffer new GL memory (glBufferData), the others, and the bytes.
    std::uint64_t buffer_reallocs = 0, buffer_writes = 0, buffer_bytes = 0;
    unsigned programs_compiled = 0, programs_cached = 0;
    // 132vt3 (research/VITA.md "GXM scenes"), vitaGL: the GXM scenes begun since the start, by why the one before
    // ended: the work went to another framebuffer (counted under the new framebuffer's kind, scene_kind_name), or a
    // GPU wait (glFinish, or glReadPixels of the framebuffer being drawn) ended it. The present ends each frame's last.
    enum SceneKind : unsigned {
        scene_display, scene_shadow, scene_frame_alpha, scene_display_copy, scene_screen_fx, scene_corona_reduce,
        scene_gui, scene_other, scene_wait, scene_kinds
    };
    std::uint64_t gxm_scenes[scene_kinds]{};
    std::uint64_t gxm_scenes_total = 0;
    unsigned gxm_scenes_last_frame = 0;
    GxmTimers gxm;  // 132vg2: the GXM backend's timers
};
inline const char* scene_kind_name(unsigned kind) {
    static const char* const names[]{"display", "shadow buffers", "frame alpha", "display copies", "ScreenFxBuf",
                                     "corona reduce", "GUI target", "other targets", "GPU waits"};
    return kind < MemoryStats::scene_kinds ? names[kind] : "?";
}

// The one GXM device (sceGxmInitialize once per process) and its display swap chain. Throws std::runtime_error
// naming the failed sce call; a missing libshacccg.suprx throws ShaderCompilerMissing at the first program.
Display create_display(const Options& options = {});
MemoryStats memory_stats();
// 132vt2c (VITA.md "Memory", vitaGL backend): the backend's buffers and textures by the memory vitaGL placed them in.
std::string placement_line();
// Port-only (132vg2, VITA.md "Vita render path (132vg2)"), GXM: the scenes of the frame presented two frames ago, in
// order, one per line (target, depth load / clear, draws, clears, copies, CPU time open, GPU time); empty for vitaGL.
std::string scene_dump();
// Test switch vita_test_gl_trace.txt (VITA.md "Vita3K"): the backend's call trace starts here (the play's window).
void arm_trace();
// A system message dialog (SceMsgDialog drawn by SceCommonDialog over the display) shown until the user closes it:
// the fatal-error path (e.g. ShaderCompilerMissing), which needs no shader. Needs create_display first.
void show_message(const std::string& text);

struct ShaderCompilerMissing : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Port-only diagnostic (132vg2 step 3f, research/VITA.md "Vita render path (132vg2)"): the GPU probe. Nonzero changes
// how the scene_ps draws are drawn, to split the main scene's GPU time on the handheld (the picture is wrong while it
// runs): 1 no texture sampling, 2 blending off, 3 every draw without discard, 4 blended draws skipped, 5 every scene
// draw skipped, 6 every scene draw squeezed into a 1x1 viewport (vertex and per-draw work only). The play log cycles
// it per window when ux0:data/omega_strain/vita_test_gpu_probe.txt is present.
inline std::atomic<int> g_gpu_probe_mode{0};

}  // namespace omega::gfx::gxm
