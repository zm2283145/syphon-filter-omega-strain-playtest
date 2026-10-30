// Port-only (checkpoint 132vt1, research/VITA.md "The Vita app"): the PS Vita app (SFOS00001). Stage 1 of the Vita
// port: it brings up the platform layer (src/port/vita) and the GXM backend (gfx_gxm.hpp), proves the backend
// on a read-back self test, then runs the front end's title screen (TITLESCR, the GuiRuntime / GuiDecor / GuiRenderer
// path of omega_frontend_formats --gui-capture, live with the handheld's pad) and the TORONTO1 loading screen
// (build_loading_screen / loading_frame) from the user's disc image, measuring frame times and memory.
//
// Everything is logged to ux0:data/omega_strain/omega_vita.log. The disc image is ux0:data/omega_strain/omega_strain.iso
// (verified once, as on the PC: size, then SHA-256 of research/REQUIRED_DISC.md; iso_verify_vita.cpp caches it).
// Controls: SELECT goes from the title screen to the loading screen; START + SELECT quits.
// The front end proper (FrontendApp: menus, mission select, the level hand-off) and the level setup / play loop are
// still the Windows hosts' (frontend_app.cpp, play_main.cpp): research/VITA.md "Handoff".

#include "audio_device.hpp"
#include "crash_report.hpp"
#include "crash_report_vita.hpp"
#include "derived_data.hpp"
#include "draw_prepare.hpp"
#include "particle_vu1.hpp"
#include "skinned_mesh.hpp"
#include "small_alloc.hpp"
#include "particle_tail.hpp"
#include "host_window.hpp"
#include "launcher.hpp"
#include "disc.hpp"
#include "display_settings.hpp"
#include "audio_output.hpp"
#include "frontend_disc.hpp"
#include "frontend_formats.hpp"
#include "frame_fit.hpp"
#include "frame_overlay.hpp"
#include "frame_profile.hpp"
#include "frontend_session.hpp"
#include "zone_profile.hpp"
#include "game_sound.hpp"
#include "gamepad_device.hpp"
#include "gamepad_reports.hpp"
#include "gfx_device.hpp"
#include "gfx_gxm.hpp"
#include "gfx_gxm_shaders.hpp"
#include "gfx_render_thread.hpp"
#include "gui_decor.hpp"
#include "gui_draw.hpp"
#include "gui_renderer.hpp"
#include "gui_runtime.hpp"
#include "ie_draw.hpp"
#include "iso_verify_vita.hpp"
#include "loading_screen_host.hpp"
#include "port_settings.hpp"
#include "population.hpp"
#include "localization.hpp"
#include "string_pool.hpp"
#include "sound_iop_host.hpp"
#include "thread_roles.hpp"
#include "vita_file.hpp"
#include "windows_hash.hpp"
#include "worker_pool.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// play_main.cpp's entry on the handheld (the PC's wmain without the launcher).
int omega_play_entry(int argc, wchar_t** argv);

// The C++ heap profile (defined with operator new at the end of this file).
namespace heap_profile {
void report(const char* when, int top);
extern std::atomic<bool> enabled;
}

extern "C" {
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
// The newlib heap (the game's allocations); the GPU pools are separate memory blocks (gfx_gxm.hpp Options).
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;
}

namespace {

using namespace omega;

FILE* g_log = nullptr;
std::string g_log_path;
double g_log_reopen_seconds = 2.0;   // 0 with vita_test_log_lines.txt (every line reaches Vita3K's host file)

// Writes `lines` (whole lines) and flushes; Vita3K holds a guest file's writes in a host buffer until it is closed, so
// the file is reopened at most every g_log_reopen_seconds so a killed emulator keeps the log.
void write_log_lines(const char* lines, std::size_t size, double t) {
    if (!g_log) return;
    std::fwrite(lines, 1, size, g_log);
    std::fflush(g_log);
    static double reopened = 0.0;
    if (t - reopened >= g_log_reopen_seconds && !g_log_path.empty()) {
        reopened = t;
        std::fclose(g_log);
        g_log = std::fopen(g_log_path.c_str(), "a");
    }
}

// Port-only (132vs1, research/VITA.md "Log writer"): each flushed log line costs the handheld about 5 ms (the
// vt2e log's consecutive lines are 4-7 ms apart), 2 lines a frame in-level and about 1600 a session. log() queues the
// line and this thread writes the queue (the lines a write found waiting in one write), so the simulation, render
// and loading threads no longer wait for the memory card. With vita_test_log_lines.txt (Vita3K: every line reaches
// the host file before an emulator abort) log() writes the line itself, as before.
struct LogWriter {
    std::mutex mutex;  // the queue, `running` and the file
    std::condition_variable wake;
    std::string queue;
    bool running = false, stopping = false;
    std::thread thread;

    void start() {
        std::lock_guard<std::mutex> lock(mutex);
        if (running || g_log_reopen_seconds <= 0.0) return;
        running = true;
        thread = std::thread([this] {
            enter_thread_role(ThreadRole::worker, "log writer");
            std::string batch;
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) break;
                batch.swap(queue);
                lock.unlock();
                write_log_lines(batch.data(), batch.size(), double(sceKernelGetProcessTimeWide()) * 1e-6);
                batch.clear();
                lock.lock();
            }
        });
    }
    // Writes what is queued and ends the thread (the exit path, before the file closes).
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running) return;
            stopping = true;
        }
        wake.notify_one();
        thread.join();
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }
};
LogWriter g_log_writer;

void log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    const double t = double(sceKernelGetProcessTimeWide()) * 1e-6;
    char line[1100];
    const int n = std::snprintf(line, sizeof line, "[%9.3f] %s\n", t, text);
    const std::size_t size = n < 0 ? 0 : std::min<std::size_t>(std::size_t(n), sizeof line - 1);
    std::unique_lock<std::mutex> lock(g_log_writer.mutex);
    if (g_log_writer.running) {
        g_log_writer.queue.append(line, size);
        lock.unlock();
        g_log_writer.wake.notify_one();
        return;
    }
    write_log_lines(line, size, t);
}

// std::cout / std::cerr (the play host's and the shared code's log) into the log file, a line at a time.
class LogBuf : public std::streambuf {
protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        std::lock_guard<std::mutex> lock(mutex_);
        if (c == '\n') flush_line();
        else line_ += char(c);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::streamsize i = 0; i < n; ++i) {
            if (s[i] == '\n') flush_line();
            else line_ += s[i];
        }
        return n;
    }

private:
    void flush_line() {
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        log("%s", line_.c_str());
        line_.clear();
    }
    std::mutex mutex_;
    std::string line_;
};

[[gnu::noinline]] double now_seconds() { return double(sceKernelGetProcessTimeWide()) * 1e-6; }

// Port-only diagnostic (research/VITA.md "Front-end speed"): the C++ heap allocations (the replaced operator new below),
// for the per-frame allocation numbers.
std::uint32_t g_mallocs = 0;
thread_local std::uint32_t t_mallocs = 0;   // this thread's (the zones' allocation counts, 132vs1)

// Port-only diagnostic: the simulation thread's frame split into parts (lap(i) charges the time since the last lap to
// part i), logged every 120 frames as mean / max ms, with the allocations and disc reads per frame.
// The shared code's OMEGA_ZONE slots (zone_profile.hpp), heaviest first, then reset: ms, calls and (132vs1) heap
// allocations per frame, on as many lines as they take (a log line holds 1023 characters).
template <class Log> void zone_report(Log&& out, int frames) {
    std::vector<omega::zone::Slot*> slots;
    for (auto* s = omega::zone::Slot::head().load(); s; s = s->next)
        if (s->calls) slots.push_back(s);
    std::sort(slots.begin(), slots.end(), [](auto* a, auto* b) { return a->ns > b->ns; });
    char text[1000];
    const char* head = "zones (ms/frame, calls/frame, allocations/frame):";
    int n = std::snprintf(text, sizeof text, "%s", head);
    for (auto* s : slots) {
        if (n > int(sizeof text) - 100) {
            out(text);
            n = std::snprintf(text, sizeof text, "zones (more):");
        }
        n += std::snprintf(text + n, sizeof text - n, " %s %.2f x%.1f a%.0f;", s->name, s->ns / 1e6 / frames,
                           double(s->calls) / frames, double(s->allocs) / frames);
    }
    for (auto* s = omega::zone::Slot::head().load(); s; s = s->next) s->ns = 0, s->calls = 0, s->allocs = 0;
    if (!slots.empty()) out(text);
    // Port-only (132vf1, research/VITA.md "Front-end speed (132vf1)"): once, what one zone costs (its two clock reads
    // and the slot update), to read the zone lines net of their own overhead.
    static bool overhead_logged = false;
    if (!overhead_logged && !slots.empty()) {
        overhead_logged = true;
        static omega::zone::Slot probe{"zone.overhead"};
        constexpr int n_probe = 2000;
        const double t0 = now_seconds();
        for (int i = 0; i < n_probe; ++i) {
            omega::zone::Scope scope{probe};
        }
        const double t1 = now_seconds();
        probe.ns = 0, probe.calls = 0;
        std::snprintf(text, sizeof text, "zones: one zone costs %.2f us (%d timed)", (t1 - t0) * 1e6 / n_probe, n_probe);
        out(text);
    }
}

struct FrameParts {
    static constexpr int count = 10;
    const char* names[count] = {"pad",           "session.frame", "sound mix",     "images",   "movie picture",
                                "canvas record", "audio submit",  "renderer draw", "present",  "other"};
    double sum[count]{}, peak[count]{};
    double mark = 0.0;
    int frames = 0;
    std::uint64_t mallocs = g_mallocs, reads = 0;
    std::string overlay;  // the last report's main parts, for the frame overlay
    void begin() { mark = now_seconds(); }
    void lap(int part) {
        const double t = now_seconds(), d = t - mark;
        sum[part] += d;
        peak[part] = std::max(peak[part], d);
        mark = t;
    }
    template <class Log> void end(Log&& out, std::uint64_t disc_reads) {
        if (++frames < 120) return;
        char text[900];
        int n = std::snprintf(text, sizeof text, "frame parts (%d frames, ms mean/max):", frames);
        double total = 0.0;
        for (int i = 0; i < count; ++i) {
            total += sum[i];
            n += std::snprintf(text + n, sizeof text - n, " %s %.2f/%.1f;", names[i], 1000.0 * sum[i] / frames,
                               1000.0 * peak[i]);
        }
        const std::uint64_t m = g_mallocs;
        std::snprintf(text + n, sizeof text - n, " total %.2f; %.0f allocations, %.2f disc reads per frame",
                      1000.0 * total / frames, double(m - mallocs) / frames, double(disc_reads - reads) / frames);
        out(text);
        zone_report(out, frames);
        char line[160];
        std::snprintf(line, sizeof line, "STEP %.1f  RECORD %.1f  DRAW %.1f  MIX %.1f  ALLOC %.0f",
                      1000.0 * sum[1] / frames, 1000.0 * (sum[3] + sum[4] + sum[5]) / frames, 1000.0 * sum[7] / frames,
                      1000.0 * sum[2] / frames, double(m - mallocs) / frames);
        *this = FrameParts{};
        overlay = line;
        reads = disc_reads;
    }
};

// Port-only (132vg2, research/VITA.md "Vita render path (132vg2)"): per play-frames window, the render thread's
// execute / present time (RenderThread::totals), each thread's busy and kernel run time and the cores' idle time, the
// GXM backend's timers and GPU time (MemoryStats::gxm), one frame's scenes (gxm::scene_dump) and the particle
// prepare's split (g_particle_prepare_times).
bool g_gpu_probe_cycle = false;  // 132vg2 step 3f: vita_test_gpu_probe.txt
struct RenderWindow {
    gfx::RenderThread::Totals rt{};
    gfx::gxm::GxmTimers t{};
    std::vector<ThreadUsage> threads;
    std::uint64_t idle[3]{};
    bool idle_ok = false;
    std::uint64_t pp[6]{};
    std::uint64_t cull[7]{};  // g_scene_cull_stats
    double start = 0.0;
    static void cull_now(std::uint64_t (&out)[7]) {
        const auto& c = g_scene_cull_stats;
        const std::uint64_t v[7]{c.frames.load(), c.models.load(), c.culled.load(), c.surfaces.load(),
                                 c.culled_surfaces.load(), c.indices.load(), c.culled_indices.load()};
        std::copy(std::begin(v), std::end(v), std::begin(out));
    }

    void begin(const gfx::gxm::MemoryStats& g) {
        rt = gfx::RenderThread::totals();
        t = g.gxm;
        threads = thread_usage();
        idle_ok = core_idle_us(idle);
        const auto& p = g_particle_prepare_times;
        const std::uint64_t now[6]{p.calls.load(), p.batches.load(), p.programs.load(), p.program_us.load(),
                                   p.sprite_us.load(), p.wall_us.load()};
        std::copy(std::begin(now), std::end(now), std::begin(pp));
        cull_now(cull);
        start = now_seconds();
    }
    void report(const gfx::gxm::MemoryStats& g, unsigned frames) {
        const double wall = std::max(now_seconds() - start, 1e-3);
        const double f = std::max(frames, 1u);
        const auto r = gfx::RenderThread::totals();
        const double rf = std::max<double>(double(r.frames - rt.frames), 1.0);
        const double ex = double(r.execute_us - rt.execute_us) / 1000.0, pr = double(r.present_us - rt.present_us) / 1000.0;
        log("play render thread: %llu frames executed; ms/frame execute %.2f, present %.2f; executing %.1f%% and "
            "presenting %.1f%% of the %.1f s",
            static_cast<unsigned long long>(r.frames - rt.frames), ex / rf, pr / rf, ex / 10.0 / wall, pr / 10.0 / wall,
            wall);

        const auto after = thread_usage();
        std::string list;
        for (std::size_t i = 0; i < after.size(); ++i) {
            const ThreadUsage& u = after[i];
            const std::uint64_t busy0 = i < threads.size() && threads[i].busy_us <= u.busy_us ? threads[i].busy_us : 0;
            const std::uint64_t run0 = i < threads.size() && threads[i].run_us <= u.run_us ? threads[i].run_us : 0;
            if (u.busy_us == busy0 && u.run_us == run0) continue;
            char item[96];
            std::snprintf(item, sizeof item, "%s%s (core %u) %.0f/%.0f", list.empty() ? "" : ", ", u.name.c_str(),
                          thread_role_core(u.role), double(u.busy_us - busy0) / 1e4 / wall,
                          double(u.run_us - run0) / 1e4 / wall);
            list += item;
        }
        std::uint64_t idle_now[3]{};
        char cores[96] = "no kernel idle clock";
        if (idle_ok && core_idle_us(idle_now))
            std::snprintf(cores, sizeof cores, "core idle %.0f%% / %.0f%% / %.0f%%", double(idle_now[0] - idle[0]) / 1e4 / wall,
                          double(idle_now[1] - idle[1]) / 1e4 / wall, double(idle_now[2] - idle[2]) / 1e4 / wall);
        log("play threads (busy %% / kernel run %%): %s; %s", list.c_str(), cores);

        const auto& n = g.gxm;
        if (n.submits != t.submits) {
            const auto ms = [&](std::uint64_t a, std::uint64_t b) { return double(a - b) / 1000.0 / f; };
            const auto per = [&](std::uint64_t a, std::uint64_t b) { return double(a - b) / f; };
            const double s = std::max<double>(double(n.sampled - t.sampled), 1.0);
            const auto us = [&](std::uint64_t a, std::uint64_t b) { return double(a - b) / s; };
            const double whole = us(n.sample_total_us, t.sample_total_us);
            const double parts = us(n.sample_program_us, t.sample_program_us) + us(n.sample_state_us, t.sample_state_us) +
                                 us(n.sample_texture_us, t.sample_texture_us) + us(n.sample_uniform_us, t.sample_uniform_us) +
                                 us(n.sample_draw_us, t.sample_draw_us);
            log("play gxm (ms/frame): submit %.2f (x%.1f), begin scene %.2f (x%.1f), end scene %.2f, clears %.2f (x%.1f), "
                "copies %.2f (x%.1f), updates %.2f (x%.1f, %.0f KB; constants x%.0f), display queue %.2f, present wait "
                "%.2f, drains %.2f (x%.2f), shadow filter %.2f (x%.1f); 1 draw in 8 (us a draw, %.0f sampled): whole "
                "%.1f = programs %.1f + state %.1f + textures %.1f + uniforms %.1f + draw %.1f + rest %.1f; per frame: "
                "state calls skipped %.0f of %.0f, texture slots skipped %.0f, fragment program cache hits %.0f; "
                "patches %.1f (%.0f KB, filled %.0f KB, live %llu), patched draws %.0f, rebased lists %.1f, widened %.2f, folds %.2f; "
                "no-alpha-test draws %.0f, alpha test proven to pass %.0f (vertex ranges read %.1f); man alpha fail "
                "skipped %.0f + RGB-only %.0f; shadow receivers by vertex flags %.1f",
                ms(n.submit_us, t.submit_us), per(n.submits, t.submits), ms(n.begin_scene_us, t.begin_scene_us),
                per(n.scenes, t.scenes), ms(n.end_scene_us, t.end_scene_us), ms(n.clear_us, t.clear_us),
                per(n.clears, t.clears), ms(n.copy_us, t.copy_us), per(n.copies, t.copies), ms(n.update_us, t.update_us),
                per(n.updates, t.updates), double(n.update_bytes - t.update_bytes) / 1024.0 / f,
                per(n.constant_updates, t.constant_updates), ms(n.display_queue_us, t.display_queue_us),
                ms(n.present_wait_us, t.present_wait_us), ms(n.drain_us, t.drain_us), per(n.drains, t.drains),
                ms(n.shadow_filter_us, t.shadow_filter_us), per(n.shadow_draws, t.shadow_draws),
                double(n.sampled - t.sampled), whole, us(n.sample_program_us, t.sample_program_us),
                us(n.sample_state_us, t.sample_state_us), us(n.sample_texture_us, t.sample_texture_us),
                us(n.sample_uniform_us, t.sample_uniform_us), us(n.sample_draw_us, t.sample_draw_us), whole - parts,
                per(n.state_skipped, t.state_skipped), per(n.state_calls, t.state_calls),
                per(n.texture_slots_skipped, t.texture_slots_skipped), per(n.program_cache_hits, t.program_cache_hits),
                per(n.patch_updates, t.patch_updates), double(n.patch_bytes - t.patch_bytes) / 1024.0 / f,
                double(n.patch_fill_bytes - t.patch_fill_bytes) / 1024.0 / f,
                static_cast<unsigned long long>(n.patches_live), per(n.patch_draws, t.patch_draws),
                per(n.rebased_lists, t.rebased_lists), per(n.patch_widened, t.patch_widened),
                per(n.patch_folds, t.patch_folds), per(n.no_alpha_test_draws, t.no_alpha_test_draws),
                per(n.alpha_proven_draws, t.alpha_proven_draws), per(n.alpha_scans, t.alpha_scans),
                per(n.man_alpha_fail_skipped, t.man_alpha_fail_skipped),
                per(n.man_alpha_fail_draws, t.man_alpha_fail_draws),
                per(n.shadow_flagged_draws, t.shadow_flagged_draws));
            {
                // 132vg2 step 3f: the scene draws by kind, and the GPU probe's mode this window (gfx_gxm.hpp)
                static const char* const probe_names[]{"off",          "no texture",          "blending off",
                                                       "no discard",   "blended draws skipped", "scene draws skipped",
                                                       "1x1 viewport"};
                const int mode = gfx::gxm::g_gpu_probe_mode.load();
                log("play scene draws (per frame): %.1f, blended %.1f (blend proven a no-op and dropped %.1f), with "
                    "discard %.1f, into the display %.1f; patched draws' alpha read %.1f; skipped (every pixel "
                    "discarded) %.1f; GPU probe mode %d (%s)",
                    per(n.scene_draws, t.scene_draws), per(n.scene_blended, t.scene_blended),
                    per(n.blend_proven_draws, t.blend_proven_draws),
                    per(n.scene_discard, t.scene_discard), per(n.scene_display, t.scene_display),
                    per(n.alpha_patched_reads, t.alpha_patched_reads),
                    per(n.alpha_skipped_draws, t.alpha_skipped_draws), mode,
                    probe_names[mode >= 0 && mode < 7 ? mode : 0]);
                if (g_gpu_probe_cycle) gfx::gxm::g_gpu_probe_mode.store((mode + 1) % 7);
                if (n.census[0] + n.census[1] + n.census[2] + n.census[3] + n.census[4] !=
                    t.census[0] + t.census[1] + t.census[2] + t.census[3] + t.census[4])
                    log("play blend census (per frame, scene draws still blended): other blend factors %.1f, FIX over "
                        "%.1f, alpha range unknown %.1f, cut-out (texture alpha 0 or full) %.1f, translucent %.1f; of "
                        "them with discard %.1f, writing depth %.1f, linear filter %.1f, full vertex alpha %.1f, "
                        "untextured %.1f",
                        per(n.census[0], t.census[0]), per(n.census[1], t.census[1]), per(n.census[2], t.census[2]),
                        per(n.census[3], t.census[3]), per(n.census[4], t.census[4]), per(n.census[5], t.census[5]),
                        per(n.census[6], t.census[6]), per(n.census[7], t.census[7]), per(n.census[8], t.census[8]),
                        per(n.census[9], t.census[9]));
            }
            log("play gxm updates (ms/frame): patch path %.2f (x%.1f, in place x%.1f, rotated x%.1f; allocate %.2f, GPU "
                "writes %.2f, %.1f new in CDRAM); fresh copies %.2f (x%.1f, %.0f KB copied from the old block); plain "
                "writes %.2f",
                ms(n.upd_patch_us, t.upd_patch_us), per(n.patch_updates, t.patch_updates),
                per(n.upd_patch_in_place, t.upd_patch_in_place), per(n.upd_patch_rotated, t.upd_patch_rotated),
                ms(n.upd_patch_alloc_us, t.upd_patch_alloc_us),
                ms(n.upd_patch_gpu_write_us, t.upd_patch_gpu_write_us), per(n.upd_patch_in_cdram, t.upd_patch_in_cdram),
                ms(n.upd_fresh_us, t.upd_fresh_us), per(n.upd_fresh, t.upd_fresh),
                double(n.upd_fresh_copied - t.upd_fresh_copied) / 1024.0 / f, ms(n.upd_write_us, t.upd_write_us));
            if (n.gpu_scenes != t.gpu_scenes) {
                std::string kinds;
                for (unsigned k = 0; k < gfx::gxm::GxmTimers::gpu_kinds; ++k) {
                    if (n.gpu_kind_scenes[k] == t.gpu_kind_scenes[k]) continue;
                    char item[80];
                    std::snprintf(item, sizeof item, "%s%s %.2f (x%.1f)", kinds.empty() ? "" : ", ", gfx::gxm::gpu_kind_name(k),
                                  ms(n.gpu_kind_us[k], t.gpu_kind_us[k]), per(n.gpu_kind_scenes[k], t.gpu_kind_scenes[k]));
                    kinds += item;
                }
                log("play gpu: busy %.2f ms/frame over %.1f scenes/frame (%.0f%% of the %.1f s); by target ms/frame: %s; "
                    "resyncs %llu",
                    ms(n.gpu_busy_us, t.gpu_busy_us), per(n.gpu_scenes, t.gpu_scenes),
                    double(n.gpu_busy_us - t.gpu_busy_us) / 1e4 / wall, wall, kinds.c_str(),
                    static_cast<unsigned long long>(n.gpu_resyncs - t.gpu_resyncs));
            }
            const std::string dump = gfx::gxm::scene_dump();
            std::size_t at = 0;
            bool first = true;
            while (at < dump.size()) {
                // up to about 900 characters a line, broken between scenes
                std::size_t end = dump.size();
                if (end - at > 900) {
                    end = dump.rfind('\n', at + 900);
                    if (end == std::string::npos || end <= at) end = std::min(dump.size(), at + 900);
                }
                std::string part = dump.substr(at, end - at);
                for (char& c : part)
                    if (c == '\n') c = ';';
                log("%s%s", first ? "play scene sequence: " : "play scene sequence (more): ", part.c_str());
                first = false;
                at = end;
            }
        }

        const auto& p = g_particle_prepare_times;
        const std::uint64_t now[6]{p.calls.load(), p.batches.load(), p.programs.load(), p.program_us.load(),
                                   p.sprite_us.load(), p.wall_us.load()};
        {
            std::uint64_t c[7];
            cull_now(c);
            const double cf = std::max<double>(double(c[0] - cull[0]), 1.0);
            if (c[0] != cull[0])
                log("play scene cull (%s): per scene frame %.0f models filed, %.0f outside the view (%.0f of %.0f surfaces, "
                    "%.0f of %.0f indices)", g_scene_cull ? "outside models not drawn" : "off: all drawn",
                    double(c[1] - cull[1]) / cf, double(c[2] - cull[2]) / cf, double(c[4] - cull[4]) / cf,
                    double(c[3] - cull[3]) / cf, double(c[6] - cull[6]) / cf, double(c[5] - cull[5]) / cf);
        }
        if (now[0] != pp[0])
            log("play particle prepare: per frame %.1f calls, %.1f batches, %.1f VU1 programs; ms/frame wall %.2f, worker "
                "time in the programs %.2f, in the sprite programs %.2f",
                double(now[0] - pp[0]) / f, double(now[1] - pp[1]) / f, double(now[2] - pp[2]) / f,
                double(now[5] - pp[5]) / 1000.0 / f, double(now[3] - pp[3]) / 1000.0 / f, double(now[4] - pp[4]) / 1000.0 / f);
        begin(g);
    }
};

void log_memory(const char* when) {
    const struct mallinfo m = mallinfo();
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof info;
    sceKernelGetFreeMemorySize(&info);
    const auto gpu = gfx::gxm::memory_stats();
    log("memory (%s): heap arena %.1f MB, in use %.1f MB; kernel free: user %.1f MB, CDRAM %.1f MB, phycont %.1f MB; "
        "GPU pools: CDRAM %.1f / %.1f MB (peak %.1f), LPDDR %.1f / %.1f MB (peak %.1f)",
        when, m.arena / 1048576.0, m.uordblks / 1048576.0, info.size_user / 1048576.0, info.size_cdram / 1048576.0,
        info.size_phycont / 1048576.0, gpu.cdram_used / 1048576.0, gpu.cdram_pool / 1048576.0,
        gpu.cdram_peak / 1048576.0, gpu.lpddr_used / 1048576.0, gpu.lpddr_pool / 1048576.0, gpu.lpddr_peak / 1048576.0);
}

// Per-thread and per-core busy time over a phase (thread_roles.hpp; research/VITA.md "Threads and cores").
struct ThreadReport {
    double start = now_seconds();
    std::vector<ThreadUsage> before = thread_usage();
    std::uint64_t idle_before[3]{};
    bool idle_ok = core_idle_us(idle_before);
    void report(const char* phase) {
        const double wall = now_seconds() - start;
        const auto after = thread_usage();
        double core_busy[3]{};
        log("threads (%s, %.2f s):", phase, wall);
        // The registry is append-only and names repeat (one "audio out" per device): match by index.
        for (std::size_t i = 0; i < after.size(); ++i) {
            const ThreadUsage& t = after[i];
            // A reused thread id restarts its kernel run clock: never subtract a larger baseline.
            const std::uint64_t busy0 = i < before.size() && before[i].busy_us <= t.busy_us ? before[i].busy_us : 0;
            const std::uint64_t run0 = i < before.size() && before[i].run_us <= t.run_us ? before[i].run_us : 0;
            if (t.busy_us == busy0 && t.run_us == run0) continue;  // ended, or idle all phase
            const double busy = double(t.busy_us - busy0) * 1e-6, run = double(t.run_us - run0) * 1e-6;
            const unsigned core = thread_role_core(t.role);
            core_busy[core] += busy;
            if (t.run_us != 0)
                log("  %-14s %-10s core %u (affinity 0x%05X, priority %d): busy %.3f s = %.1f%%, kernel run clock %.3f s",
                    t.name.c_str(), thread_role_name(t.role), core, t.affinity, t.priority, busy, 100.0 * busy / wall,
                    run);
            else  // Vita3K's sceKernelGetThreadInfo leaves runClocks zero
                log("  %-14s %-10s core %u (affinity 0x%05X, priority %d): busy %.3f s = %.1f%% (no kernel run clock)",
                    t.name.c_str(), thread_role_name(t.role), core, t.affinity, t.priority, busy, 100.0 * busy / wall);
        }
        std::uint64_t idle_after[3]{};
        const bool idle = idle_ok && core_idle_us(idle_after);
        for (unsigned c = 0; c < 3; ++c) {
            if (idle)
                log("  core %u: busy %.1f%% by the pinned threads' own count, %.1f%% by the kernel's idle clock", c,
                    100.0 * core_busy[c] / wall, 100.0 - 100.0 * double(idle_after[c] - idle_before[c]) * 1e-6 / wall);
            else
                log("  core %u: busy %.1f%% (the pinned threads' own count; no kernel idle clock here)", c,
                    100.0 * core_busy[c] / wall);
        }
        start = now_seconds();
        before = after;
        idle_ok = core_idle_us(idle_before);
    }
};

// 132vc1 (research/VITA.md "Render pass on the Vita's cores (132vc1)"): with each "play frames" window, how the
// threads and cores were used (busy time from the threads' own scopes, the kernel's run clock, the cores' idle clocks)
// and how the worker pool's calls and the particle preparation spread over them, all per frame of the window.
void log_play_threads(unsigned frames) {
    static std::vector<ThreadUsage> before = thread_usage();
    static std::uint64_t idle_before[3]{};
    static bool idle_ok = core_idle_us(idle_before);
    static WorkerPool::Profile pool_before = WorkerPool::profile();
    static ParticlePrepareProfile prep_before = particle_prepare_profile();
    static std::uint64_t rollbacks_before = particle::particle_neon_rollbacks();
    static std::uint64_t skin_neon_before = 0, skin_scalar_before = 0;
    static ActorPiecesProfile actors_before = actor_pieces_profile();
    if (frames == 0) skin_neon_counts(skin_neon_before, skin_scalar_before);
    static double started = now_seconds();
    // The A/B switch vita_test_ab_speculation.txt: the particle programs' speculative NEON iterations (132vc1,
    // particle_vu1.cpp) on and off in turn, one window each, so one run compares the two at the same scene load.
    static const bool ab = vita_file_size(std::string(vita_data_root) + "/vita_test_ab_speculation.txt") >= 0;
    // vita_test_ab_pool_spin.txt: the worker pool's spins (132vc1, worker_pool.hpp) on and off in turn by window.
    static const bool ab_spin = vita_file_size(std::string(vita_data_root) + "/vita_test_ab_pool_spin.txt") >= 0;
    // vita_test_ab_vc1.txt (132vc1): four modes in turn, one window each: 0 the exact particle path, 1 the speculative
    // iterations through memory, 2 the register-resident ones, 3 those with the pool's spins off (0-2 spin on).
    static const bool ab_modes = vita_file_size(std::string(vita_data_root) + "/vita_test_ab_vc1.txt") >= 0;
    static int mode = 2;
    const auto apply_mode = [](int m) {
        particle::set_particle_neon_speculation(m != 0);
        particle::set_particle_neon_registers(m >= 2);
        WorkerPool::set_spin(m != 3);
    };
    if (frames == 0) {
        if (ab) log("play particle prepare: vita_test_ab_speculation.txt present: the speculative NEON iterations "
                    "alternate on / off by window (test switch)");
        if (ab_spin) log("play pool: vita_test_ab_pool_spin.txt present: the pool's spins alternate on / off by window "
                         "(test switch)");
        if (ab_modes) {
            mode = 0;
            apply_mode(mode);
            log("play modes: vita_test_ab_vc1.txt present: modes 0 (exact particles), 1 (speculative), 2 (registers), "
                "3 (registers, pool spin off) in turn by window (test switch)");
        }
        return;
    }
    const double wall = now_seconds() - started;
    const double f = double(frames);
    const auto after = thread_usage();
    std::string line;
    for (std::size_t i = 0; i < after.size(); ++i) {
        const ThreadUsage& t = after[i];
        const std::uint64_t busy0 = i < before.size() && before[i].busy_us <= t.busy_us ? before[i].busy_us : 0;
        const std::uint64_t run0 = i < before.size() && before[i].run_us <= t.run_us ? before[i].run_us : 0;
        if (t.busy_us == busy0 && t.run_us == run0) continue;
        char item[128];
        std::snprintf(item, sizeof item, "%s%s %.1f/%.1f", line.empty() ? "" : ", ", t.name.c_str(),
                      double(t.busy_us - busy0) / 1000.0 / f, double(t.run_us - run0) / 1000.0 / f);
        line += item;
    }
    std::uint64_t idle_after[3]{};
    const bool idle = idle_ok && core_idle_us(idle_after);
    char cores[160] = "no kernel idle clock";
    if (idle && wall > 0.0)
        std::snprintf(cores, sizeof cores, "core 0 %.0f%%, core 1 %.0f%%, core 2 %.0f%%",
                      100.0 - 100.0 * double(idle_after[0] - idle_before[0]) * 1e-6 / wall,
                      100.0 - 100.0 * double(idle_after[1] - idle_before[1]) * 1e-6 / wall,
                      100.0 - 100.0 * double(idle_after[2] - idle_before[2]) * 1e-6 / wall);
    log("play threads (ms/frame busy/kernel run): %s; cores busy by the idle clocks: %s; clocks ARM %d MHz, bus %d, GPU %d",
        line.c_str(), cores, scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
        scePowerGetGpuClockFrequency());
    const WorkerPool::Profile p = WorkerPool::profile();
    const std::uint64_t calls = p.calls - pool_before.calls, items = p.items - pool_before.items;
    const std::uint64_t joins = p.joins - pool_before.joins;
    log("play pool (spin %s): %.1f parallel calls/frame, %.1f indices/frame (%.0f%% on the calling thread); caller %.2f ms/frame "
        "in the calls, %.2f of it waiting after its last index; %.2f worker joins per call, wake-up %.0f us mean; "
        "workers %.2f ms/frame in the calls",
        WorkerPool::spin() ? "on" : "off", double(calls) / f, double(items) / f,
        items ? 100.0 * double(p.caller_items - pool_before.caller_items) / double(items) : 0.0,
        double(p.call_us - pool_before.call_us) / 1000.0 / f, double(p.tail_us - pool_before.tail_us) / 1000.0 / f,
        calls ? double(joins) / double(calls) : 0.0, joins ? double(p.wake_us - pool_before.wake_us) / double(joins) : 0.0,
        double(p.worker_us - pool_before.worker_us) / 1000.0 / f);
    const ParticlePrepareProfile q = particle_prepare_profile();
    const double work = double(q.program_us - prep_before.program_us + q.sprite_us - prep_before.sprite_us);
    const double prep_wall = double(q.wall_us - prep_before.wall_us);
    const std::uint64_t rollbacks = particle::particle_neon_rollbacks();
    if (ab_modes) log("play mode %d this window (132vc1 test switch)", mode);
    log("play particle prepare (speculation %s): %.1f calls/frame (%.1f with one batch), %.1f batches/frame (%.1f "
        "programs, %.1f drawn), %.0f particles/frame; work ms/frame: programs %.2f, sprite programs %.2f; wall %.2f "
        "ms/frame (x%.2f spread); %.1f speculative iterations rolled back/frame",
        particle::particle_neon_speculation() ? "on" : "off",
        double(q.calls - prep_before.calls) / f, double(q.single_calls - prep_before.single_calls) / f,
        double(q.batches - prep_before.batches) / f, double(q.programs - prep_before.programs) / f,
        double(q.drawn - prep_before.drawn) / f, double(q.particles - prep_before.particles) / f,
        double(q.program_us - prep_before.program_us) / 1000.0 / f,
        double(q.sprite_us - prep_before.sprite_us) / 1000.0 / f, prep_wall / 1000.0 / f,
        prep_wall > 0 ? work / prep_wall : 0.0, double(rollbacks - rollbacks_before) / f);
    rollbacks_before = rollbacks;
    std::uint64_t skin_neon = 0, skin_scalar = 0;
    skin_neon_counts(skin_neon, skin_scalar);
    log("play skinning: %.0f chunks/frame on NEON, %.0f on the scalar path (132vc1)", double(skin_neon - skin_neon_before) / f,
        double(skin_scalar - skin_scalar_before) / f);
    skin_neon_before = skin_neon;
    skin_scalar_before = skin_scalar;
    const ActorPiecesProfile a = actor_pieces_profile();
    log("play actors: %.1f pool tasks/frame (LOD0 %.1f, far LOD %.1f, hidden %.1f), %.0f overlay vertices/frame; work "
        "ms/frame: LOD0 skinning %.2f, the rest of the tasks %.2f (LOD0 %.2f, far LOD %.2f, hidden %.2f) (132vc1)",
        double(a.tasks - actors_before.tasks) / f, double(a.lod0 - actors_before.lod0) / f,
        double(a.lod1 - actors_before.lod1) / f, double(a.hidden - actors_before.hidden) / f,
        double(a.vertices - actors_before.vertices) / f, double(a.skin_us - actors_before.skin_us) / 1000.0 / f,
        double(a.task_us - actors_before.task_us) / 1000.0 / f, double(a.lod0_us - actors_before.lod0_us) / 1000.0 / f,
        double(a.lod1_us - actors_before.lod1_us) / 1000.0 / f, double(a.hidden_us - actors_before.hidden_us) / 1000.0 / f);
    actors_before = a;
    // 132vc1: one recorded particle batch run on this thread (no other work at the same time), every 6th window.
    static unsigned bench_window = 0;
    if (!ab_modes && ++bench_window % 6 == 2) {
        double us[3]{}, one[3]{};
        unsigned particles = 0;
        if (particle_bench(40, us, one, particles))
            log("play particle bench (this thread alone, %u particles x 40 runs): us/particle exact %.2f, speculative %.2f, "
                "register-resident %.2f; the batch cut to 1 particle, us/batch: %.1f, %.1f, %.1f (132vc1)", particles, us[0],
                us[1], us[2], one[0], one[1], one[2]);
    }
    // 132vc1 hw5: one far-LOD actor's overlay work alone (build_actor_mesh runs it on the next frame it has one).
    if (!ab_modes && bench_window % 6 == 4) request_actor_bench();
    ActorBench actor_bench;
    if (actor_bench_take(actor_bench))
        log("play actor bench (one far-LOD actor, this thread alone, %u runs; %u parts, %u chunks, %u vertices): us first run / "
            "mean of the rest: palette %.0f / %.0f, skin_parts (palette included) %.0f / %.0f, character_mesh %.0f / %.0f (132vc1)",
            actor_bench.runs, actor_bench.parts, actor_bench.chunks, actor_bench.vertices, actor_bench.palette_first,
            actor_bench.palette_rest, actor_bench.skin_first, actor_bench.skin_rest, actor_bench.mesh_first,
            actor_bench.mesh_rest);
    if (actor_bench.lod0_vertices)   // hw6: the same bench's LOD0 actor
        log("play actor bench LOD0 (one actor's geometry() skinning, this thread alone, %u runs; %u parts, %u vertices): us "
            "first run / mean of the rest: palette %.0f / %.0f, plain parts %.0f / %.0f, face and hand parts %.0f / %.0f "
            "(132vc1)", actor_bench.runs, actor_bench.lod0_parts, actor_bench.lod0_vertices, actor_bench.lod0_first[0],
            actor_bench.lod0_rest[0], actor_bench.lod0_first[1], actor_bench.lod0_rest[1], actor_bench.lod0_first[2],
            actor_bench.lod0_rest[2]);
    if (ab) particle::set_particle_neon_speculation(!particle::particle_neon_speculation());
    if (ab_spin) WorkerPool::set_spin(!WorkerPool::spin());
    if (ab_modes) {
        mode = (mode + 1) % 4;
        apply_mode(mode);
    }
    before = after;
    std::copy(std::begin(idle_after), std::end(idle_after), std::begin(idle_before));
    idle_ok = idle;
    pool_before = p;
    prep_before = q;
    started = now_seconds();
}

// Frame time statistics over a phase.
struct FrameStats {
    std::vector<double> cpu, interval;
    double last_present = 0.0;
    void add(double cpu_seconds, double present_time) {
        cpu.push_back(cpu_seconds);
        if (last_present > 0.0) interval.push_back(present_time - last_present);
        last_present = present_time;
    }
    static void summary(const char* what, std::vector<double> v) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (double x : v) sum += x;
        log("  %s: %u frames, mean %.2f ms, median %.2f ms, p95 %.2f ms, max %.2f ms", what, unsigned(v.size()),
            1e3 * sum / double(v.size()), 1e3 * v[v.size() / 2], 1e3 * v[v.size() * 95 / 100], 1e3 * v.back());
    }
    void report(const char* phase) {
        log("frame times (%s):", phase);
        summary("CPU (update + record + submit)", cpu);
        summary("present interval", interval);
        cpu.clear();
        interval.clear();
        last_present = 0.0;
    }
};

// The GUI program's vertex (VertexLayout::gui).
struct GuiVertexHost {
    float x, y, u, v, r, g, b, a, z, w;
};

void quad(std::vector<GuiVertexHost>& out, float x0, float y0, float x1, float y1, const float c[4]) {
    const GuiVertexHost p[4] = {{x0, y0, 0, 0, c[0], c[1], c[2], c[3], 0, 1},
                                {x1, y0, 1, 0, c[0], c[1], c[2], c[3], 0, 1},
                                {x0, y1, 0, 1, c[0], c[1], c[2], c[3], 0, 1},
                                {x1, y1, 1, 1, c[0], c[1], c[2], c[3], 0, 1}};
    for (int k : {0, 1, 2, 2, 1, 3}) out.push_back(p[k]);
}

// The backend's self test: clears, a GUI-program draw, a region copy and a read-back, checked per pixel.
bool self_test(gfx::Device& device) {
    constexpr unsigned w = 64, h = 32;
    gfx::TextureDesc td;
    td.width = w;
    td.height = h;
    td.bind = gfx::bind_target | gfx::bind_sampled;
    const auto target = device.create_texture(td, nullptr, {});
    td.bind = 0;
    td.usage = gfx::Usage::staging;
    const auto staging = device.create_texture(td, nullptr, {});
    gfx::TextureDesc wd;
    wd.width = wd.height = 1;
    wd.usage = gfx::Usage::immutable;
    wd.bind = gfx::bind_sampled;
    const std::uint32_t white = 0xffffffffU;
    const gfx::Level level{&white, 4};
    const auto image = device.create_texture(wd, &level, {});
    const auto sampler = device.create_sampler({}, "self test sampler");
    const auto raster = device.create_raster({}, "self test raster");
    std::vector<GuiVertexHost> v;
    const float green[4]{0, 1, 0, 1};
    quad(v, -320, -224, 0, 224, green);  // the left half (GUI space x -320..320 over the target)
    gfx::BufferDesc bd;
    bd.bytes = v.size() * sizeof v[0];
    bd.usage = gfx::Usage::immutable;
    bd.bind = gfx::bind_vertices;
    const auto vertices = device.create_buffer(bd, v.data(), "self test vertices");
    device.prepare(gfx::Program::gui);

    gfx::CommandList list;
    const float red[4]{1, 0, 0, 1};
    list.clear_target({target.get(), 0}, red);
    list.set_target(target.get());
    list.set_viewport({0, 0, float(w), float(h), 0, 1});
    list.set_raster(raster.get());
    list.set_depth(nullptr);
    list.set_blend(nullptr);
    list.set_program(gfx::Program::gui);
    list.set_vertex_buffer(vertices.get(), sizeof(GuiVertexHost));
    list.set_texture(0, image.get());
    list.set_sampler(0, sampler.get());
    list.draw(6, 0);
    const float blue[4]{0, 0, 1, 1};
    const gfx::Rect rect{56, 0, 64, 8};
    list.clear_rects(target.get(), blue, &rect, 1);
    list.copy(staging.get(), target.get());
    device.submit(list);
    int bad = 0;
    std::uint32_t seen[3]{};
    device.read(*staging, 0, [&](const std::uint8_t* data, unsigned pitch) {
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const std::uint8_t* p = data + y * pitch + x * 4;
                const bool corner = x >= 56 && y < 8;
                const std::uint8_t want[4] = {std::uint8_t(corner ? 0 : x < w / 2 ? 0 : 255),
                                              std::uint8_t(!corner && x < w / 2 ? 255 : 0),
                                              std::uint8_t(corner ? 255 : 0), 255};
                if (std::memcmp(p, want, 4) != 0) {
                    if (bad < 8) log("  self test: pixel (%u, %u) = %02x %02x %02x %02x, expected %02x %02x %02x %02x",
                                     x, y, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
                    ++bad;
                }
                ++seen[corner ? 2 : x < w / 2 ? 1 : 0];
            }
    }, "self test read");
    log("GXM self test: %s (%d of %u pixels differ; red %u, green %u, blue %u)", bad ? "FAILED" : "passed", bad,
        w * h, seen[0], seen[1], seen[2]);
    if (bad) {
        sceKernelDelayThread(1000000);
        device.read(*staging, 0, [&](const std::uint8_t* data, unsigned) {
            log("  self test: 1 s later pixel (0, 0) = %02x %02x %02x %02x, pixel (40, 0) = %02x %02x %02x %02x", data[0],
                data[1], data[2], data[3], data[160], data[161], data[162], data[163]);
        }, "self test reread");
    }
    return bad == 0;
}

// The display copy test (132vt1b, research/VITA.md "Widescreen"; a test switch: vita_test_display_copy.txt): the back
// buffer cleared to four quadrant colours is copied whole into a texture of its size, as the wide side fill copies it
// every frame (the vitaGL backend's GPU copy, checked by the backend against the read-back on its first use), then on
// into a staging texture checked per pixel. Run twice: the first copy includes the backend's check, the second is the
// per-frame cost (with the read-back wait). Off by default: on Vita3K the next disc read after it never returns.
bool display_copy_test(gfx::Device& device, const gfx::Texture* back) {
    const gfx::TextureDesc& bd = back->desc();
    const unsigned w = bd.width, h = bd.height;
    gfx::TextureDesc cd;
    cd.width = w;
    cd.height = h;
    cd.format = bd.format;
    cd.bind = gfx::bind_sampled;
    const auto copy = device.create_texture(cd, nullptr, {});
    gfx::TextureDesc sd;
    sd.width = w;
    sd.height = h;
    sd.usage = gfx::Usage::staging;
    const auto staging = device.create_texture(sd, nullptr, {});
    const float colours[4][4]{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
    int bad = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const double start = now_seconds();
        gfx::CommandList list;
        for (unsigned q = 0; q < 4; ++q) {
            const int x = int((q & 1) ? w / 2 : 0), y = int((q & 2) ? h / 2 : 0);
            const gfx::Rect r{x, y, x + int(w / 2), y + int(h / 2)};
            list.clear_rects(back, colours[(q + pass) % 4], &r, 1);
        }
        list.copy(copy.get(), back);
        list.copy(staging.get(), copy.get());
        device.submit(list);
        bad = 0;
        device.read(*staging, 0, [&](const std::uint8_t* data, unsigned pitch) {
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const float* c = colours[((x >= w / 2 ? 1 : 0) + (y >= h / 2 ? 2 : 0) + pass) % 4];
                    const std::uint8_t* p = data + y * pitch + x * 4;
                    const std::uint8_t want[4]{std::uint8_t(c[0] * 255), std::uint8_t(c[1] * 255),
                                               std::uint8_t(c[2] * 255), 255};
                    if (std::memcmp(p, want, 4) != 0) {
                        if (bad < 4) log("  display copy test: pixel (%u, %u) = %02x %02x %02x %02x, expected %02x %02x %02x %02x",
                                         x, y, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
                        ++bad;
                    }
                }
        }, "display copy test read");
        log("display copy test, pass %d: %s (%d of %u pixels differ), %.2f ms with the read-back", pass + 1,
            bad ? "FAILED" : "passed", bad, w * h, 1000.0 * (now_seconds() - start));
    }
    return bad == 0;
}
// A 440 Hz tone for one second through SceAudioOut (the audio::Device the game's mixer feeds).
void audio_test() {
    audio::Device device(0.1);
    std::vector<std::int16_t> samples;
    constexpr int rate = 48000;
    for (int i = 0; i < rate; ++i) {
        const auto s = std::int16_t(2000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
        samples.push_back(s);
        samples.push_back(s);
    }
    const double start = now_seconds();
    device.submit(samples);
    const auto stats = device.stats();
    log("audio: submitted %llu frames in %.3f s (queued %u, underruns %u)",
        static_cast<unsigned long long>(stats.submitted_frames), now_seconds() - start, unsigned(stats.queued_frames),
        unsigned(stats.underruns));
    for (const auto& line : device.take_log()) log("audio: %s", line.c_str());
}

struct PadState {
    GamepadDevice device;
    std::uint16_t held = 0, previous = 0;
    std::string source;
    void read() {
        previous = held;
        if (const auto r = device.read()) {
            held = r->pad.held;
            source = r->source;
        }
        if (held != previous) log("pad: held %04x (%s)", held, source.c_str());
    }
    bool pressed(std::uint16_t button) const { return (held & button) && !(previous & button); }
};

class App {
public:
    // The display's device and swap chain are wrapped by the render thread (gfx_render_thread.hpp): the frames are
    // recorded here on the simulation thread (core 0) and submitted to GXM on the render host thread (core 1).
    App(gfx::Display display) : inner_(std::move(display)), display_(render_thread_.wrap(inner_)) {}
    void run();

private:
    gfx::Display inner_;
    gfx::RenderThread render_thread_;
    gfx::Display display_;
    WorkerPool workers_{WorkerPool::default_workers()};
    std::shared_ptr<gfx::Texture> back_;
    std::uint64_t frame_ = 0;
    PadState pad_;
    FrameStats stats_;
    ThreadReport threads_;
    bool quit_ = false;
    // Port-only (research/VITA.md "Frame overlay"): frame times over the frame, toggled by SELECT with R held.
    FrameOverlay overlay_;
    bool overlay_on_ = true;
    std::vector<std::string> overlay_lines_{"FRAME TIMES (MEASURING)"};
    std::string overlay_detail_;
    // Port-only (research/VITA.md "Boot progress"): a boot step's screen. Only the measured steps (fraction >= 0: the
    // first start's shader compile, the opt-in full SHA-256 verify) show their text and a bar; the others
    // (fraction < 0) are a plain black screen (132vt2b, the user's request).
    FrameOverlay progress_text_;
    double progress_drawn_ = -1.0;
    void progress(const std::string& text, double fraction, bool force = true) {
        const double now = now_seconds();
        if (!force && now - progress_drawn_ < 0.1) return;
        progress_drawn_ = now;
        auto back = begin_frame();
        if (fraction < 0.0) {
            log("boot step (black screen): %s", text.c_str());
            display_.swap_chain->present(1, "present");
            render_thread_.end_frame(++frame_);
            return;
        }
        gfx::CommandList list;
        const float frame[4]{0.25F, 0.27F, 0.32F, 1.0F}, fill[4]{0.85F, 0.55F, 0.12F, 1.0F};
        const gfx::Rect outer{180, 300, 780, 324};
        list.clear_rects(back.get(), frame, &outer, 1);
        const int left = 182, right = 182 + int(596.0 * std::min(fraction, 1.0));
        if (right > left) {
            const gfx::Rect inner{left, 302, right, 322};
            list.clear_rects(back.get(), fill, &inner, 1);
        }
        display_.device->submit(list);
        progress_text_.draw(*display_.device, back.get(), 960, 544, {"SYPHON FILTER: THE OMEGA STRAIN (NATIVE PORT)", text},
                            180, 230, 2);
        display_.swap_chain->present(1, "present");
        render_thread_.end_frame(++frame_);
    }
    double overlay_cpu_ = 0.0, overlay_interval_ = 0.0, overlay_max_ = 0.0, overlay_last_ = 0.0;
    int overlay_frames_ = 0;

    std::shared_ptr<gfx::Texture> begin_frame() {
        // One texture for every frame (the backend resolves it to the current back buffer when the list executes).
        if (!back_) back_ = display_.swap_chain->buffer(true, "back buffer", "back buffer view");
        const float black[4]{0, 0, 0, 1};
        gfx::clear_target(*display_.device, back_.get(), black);
        return back_;
    }
    void end_frame(double started) {
        if (overlay_on_ && back_) overlay_.draw(*display_.device, back_.get(), 960, 544, overlay_lines_, 8, 8, 2);
        display_.swap_chain->present(1, "present");
        const double recorded = now_seconds();
        count_busy_time(static_cast<std::uint64_t>((recorded - started) * 1e6));
        render_thread_.end_frame(++frame_);
        const double presented = now_seconds();
        stats_.add(recorded - started, presented);
        overlay_update(recorded - started, presented);
    }
    void overlay_update(double cpu, double presented) {
        if (overlay_last_ > 0.0) {
            const double interval = presented - overlay_last_;
            overlay_interval_ += interval;
            overlay_max_ = std::max(overlay_max_, interval);
            overlay_cpu_ += cpu;
            ++overlay_frames_;
        }
        overlay_last_ = presented;
        if (overlay_frames_ < 30) return;
        char a[120], b[120];
        std::snprintf(a, sizeof a, "FRAME %.1f MS  %.1f FPS  MAX %.1f", 1000.0 * overlay_interval_ / overlay_frames_,
                      overlay_frames_ / overlay_interval_, 1000.0 * overlay_max_);
        std::snprintf(b, sizeof b, "CPU %.1f MS  R1 AND SELECT HIDES", 1000.0 * overlay_cpu_ / overlay_frames_);
        overlay_lines_ = {a, b};
        if (!overlay_detail_.empty()) overlay_lines_.push_back(overlay_detail_);
        overlay_cpu_ = overlay_interval_ = overlay_max_ = 0.0;
        overlay_frames_ = 0;
    }
    void report(const char* phase) {
        stats_.report(phase);
        log("  render thread: %s", render_thread_.summary().c_str());
        threads_.report(phase);
    }
    void core_check();
    // Port-only evidence: the back buffer as drawn so far to ux0:data/omega_strain/<name>.bmp (24-bit, bottom-up).
    void capture(const gfx::Texture* back, const std::string& name) {
        auto& device = *display_.device;
        if (!capture_staging_) {
            gfx::TextureDesc td;
            td.width = 960;
            td.height = 544;
            td.usage = gfx::Usage::staging;
            capture_staging_ = device.create_texture(td, nullptr, {});
        }
        gfx::CommandList list;
        list.copy(capture_staging_.get(), back);
        device.submit(list);
        constexpr unsigned w = 960, h = 544, row = w * 3;
        std::vector<std::uint8_t> bmp(54 + std::size_t(row) * h);
        auto put32 = [&](std::size_t at, std::uint32_t v) {
            for (int k = 0; k < 4; ++k) bmp[at + k] = std::uint8_t(v >> (8 * k));
        };
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(2, std::uint32_t(bmp.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        bmp[26] = 1;
        bmp[28] = 24;
        put32(34, row * h);
        device.read(*capture_staging_, 0, [&](const std::uint8_t* data, unsigned pitch) {
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const std::uint8_t* p = data + y * pitch + x * 4;
                    std::uint8_t* q = bmp.data() + 54 + std::size_t(h - 1 - y) * row + x * 3;
                    q[0] = p[2];
                    q[1] = p[1];
                    q[2] = p[0];
                }
        }, "capture read");
        const std::string path = std::string(vita_data_root) + "/" + name + ".bmp";
        sceIoRemove(path.c_str());
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bmp.data(), 1, bmp.size(), f);
            std::fclose(f);
            log("capture: %s", path.c_str());
        }
    }
    std::shared_ptr<gfx::Texture> capture_staging_;
    void poll() {
        pad_.read();
        if ((pad_.held & ds2::start) && (pad_.held & ds2::select)) quit_ = true;
        if ((pad_.held & ds2::r1) && pad_.pressed(ds2::select)) {
            overlay_on_ = !overlay_on_;
            log("frame overlay %s (R + SELECT)", overlay_on_ ? "on" : "off");
        }
    }
    std::string front_end(DiscImage& disc, const std::string& iso, int frames);
    void loading_screen(DiscImage& disc, const std::string& level, int frames);
    void no_disc(int frames);
    void notice(const std::string& title, const std::string& text);
    int play(DiscImage& disc, const std::string& iso);
    std::string play_error_;
    std::string front_end_error_;
};

// Port-only (research/VITA.md "Errors on screen"): a stop the handheld cannot recover from in place (an unported path
// naming its address, a disc read failure) as a screen, instead of closing: the title, the message wrapped to the
// overlay font (unknown characters as spaces), until CROSS (or START + SELECT, which quits).
void App::notice(const std::string& title, const std::string& text) {
    log("notice: %s: %s", title.c_str(), text.c_str());
    std::vector<std::string> lines{title, ""};
    constexpr std::size_t width = 70;
    std::string line, word;
    auto flush_word = [&] {
        while (word.size() > width) {
            if (!line.empty()) lines.push_back(line), line.clear();
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > width) lines.push_back(line), line.clear();
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flush_word();
            lines.push_back(line);
            line.clear();
            continue;
        }
        if (c == ',' || c == ';') c = '.';
        if (c == ' ' || !overlay_font_has(c)) flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    lines.push_back("");
    lines.push_back("PRESS CROSS TO GO BACK TO THE FRONT END");
    FrameOverlay text_overlay;
    for (int frame = 0; !quit_; ++frame) {
        const double started = now_seconds();
        poll();
        if (frame > 30 && pad_.pressed(ds2::cross)) break;
        auto back = begin_frame();
        text_overlay.draw(*display_.device, back.get(), 960, 544, lines, 40, 120, 2);
        if (frame == 60) capture(back.get(), "capture_notice");
        end_frame(started);
    }
}

void install_play_movie_hooks(bool on);
std::thread start_subtitle_setup();

// Checkpoint 132vt2 (research/VITA.md "The play host"): the PC play host (play_main.cpp) on the handheld, as
// OmegaStrain.exe runs it (launcher.cpp's run): its front end, the in-process hand-off, the loading screen, the level
// (TORONTO1 first), pause / loadout and the level end, all the shared code. The working folder is
// ux0:data/omega_strain (the play host's relative paths: config/disc_us.json, local/..., mods), the derived files
// (launcher.cpp's derive step) in local/derived, and the display handed over unwrapped (the play makes its own render
// thread). ux0:data/omega_strain/play_args.txt (one argument per line) adds play host arguments.
int App::play(DiscImage& disc, const std::string& iso) {
    namespace fs = std::filesystem;
    static LogBuf log_buf;
    std::cout.rdbuf(&log_buf);
    std::cerr.rdbuf(&log_buf);
    if (chdir(vita_data_root) != 0) throw std::runtime_error(std::string("cannot enter ") + vita_data_root);
    char cwd[512]{};
    log("play host: working folder %s", getcwd(cwd, sizeof cwd) ? cwd : "(getcwd failed)");
    std::error_code ec;
    fs::create_directories("config", ec);
    fs::create_directories("local/derived", ec);
    const std::string profile = std::string(vita_data_root) + "/config/disc_us.json";
    if (vita_file_size(profile) != vita_file_size("app0:config/disc_us.json")) {
        std::ifstream in("app0:config/disc_us.json", std::ios::binary);
        std::ofstream out(profile, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
        log("play host: %s copied from the package", profile.c_str());
    }
    // vt1's settings file (ux0:data/omega_strain/port_settings.txt) becomes the play host's local/port_settings.txt.
    const std::string settings = std::string(vita_data_root) + "/local/port_settings.txt";
    const std::string old_settings = std::string(vita_data_root) + "/port_settings.txt";
    if (vita_file_size(settings) < 0 && vita_file_size(old_settings) >= 0) {
        fs::copy_file(old_settings, settings, ec);
        log("play host: %s copied to %s", old_settings.c_str(), settings.c_str());
    }
    bool has_aspect = false;
    if (std::ifstream in{settings, std::ios::binary}) {
        std::stringstream text;
        text << in.rdbuf();
        try {
            for (const auto& [key, value] : parse_port_settings(text.str()).other) has_aspect = has_aspect || key == "aspect";
        } catch (const std::exception& e) {
            log("play host: %s: %s", settings.c_str(), e.what());
        }
    }

    const derived::DiscFingerprint fingerprint{OMEGA_ISO_SHA256, OMEGA_BOOT_EXECUTABLE, OMEGA_EXE_SHA256, OMEGA_EXE_SIZE};
    const fs::path derived_folder = "local/derived";
    const auto files = derived::derived_files(derived_folder);
    if (!derived::derived_data_current(files, fingerprint)) {
        progress("PREPARING THE GAME DATA (THE FIRST START READS THE BOOT EXECUTABLE AND THE MOTION FILES)", -1.0);
        const double start = now_seconds();
        derived::derive_game_data(disc, fingerprint, derived_folder);
        log("play host: derived the motion graphs and tunings into local/derived in %.1f s", now_seconds() - start);
    } else {
        log("play host: local/derived is current");
    }

    launcher::Options options;
    options.dialogs = false;
    if (!has_aspect) options.extra = {L"--aspect", L"wide"};   // the 960 x 544 screen's default (132ws)
    options.extra.push_back(L"--frame-overlay");
    // 132vt2d: the play's render thread (gfx_render_thread.hpp, core 1) is on, as on the PC: the vitaGL backend takes
    // its mutex around every vitaGL call (gfx_vitagl.cpp), so the level's GPU submission overlaps the next step on
    // core 0 (Vita3K TRAINING: no abort, 3.1 fps against 2.7). "--render-thread 0" in play_args.txt turns it off
    // (research/VITA.md "The play host").
    options.extra.push_back(L"--render-thread");
    options.extra.push_back(L"1");
    // 132vt2: the level steps at the original's world dt (two NTSC fields, 0.033367 s: frame_pacer.hpp), half the
    // PC's 1/60 steps; the presents wait two vertical blanks (30 fps). "--dt 0.0166667" in play_args.txt overrides it.
    options.extra.push_back(L"--dt");
    options.extra.push_back(L"0.033367");
    // 132vt2d: one world update per loop at the loop's measured interval, clamped to [0.001, 0.05] (the original's
    // world dt, 00133340 -> 005383d0), instead of up to four fixed steps a loop: a slow frame no longer makes the
    // next one slower ("The play host"). "!--measured-dt" in play_args.txt drops it.
    options.extra.push_back(L"--measured-dt");
    const std::string args_file = std::string(vita_data_root) + "/play_args.txt";
    std::vector<std::wstring> drops;
    if (std::ifstream in{args_file}) {
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '!') {
                // "!--frontend": drops that argument from the launcher's list (a direct level start on Vita3K, whose
                // front-end audio aborts the emulator: "Vita3K").
                const std::wstring drop(line.begin() + 1, line.end());
                drops.push_back(drop);
                log("play host: argument %s dropped (play_args.txt)", line.c_str() + 1);
                continue;
            }
            options.extra.emplace_back(line.begin(), line.end());
            log("play host: argument %s (play_args.txt)", line.c_str());
        }
    }
    std::vector<std::wstring> storage{L"omega_vita"};
    // The whole path: open_verified_iso's per-process key (the path, size, mtime and digest) is the one the boot check
    // above remembered, so the play host does not hash the 3.46 GB image again.
    for (auto& a : launcher::play_arguments(fs::path(iso), files, options))
        if (std::find(drops.begin(), drops.end(), a) == drops.end()) storage.push_back(std::move(a));
    std::vector<wchar_t*> pointers;
    for (auto& a : storage) pointers.push_back(a.data());
    pointers.push_back(nullptr);
    {
        std::string line;
        for (const auto& a : storage) line += std::string(a.begin(), a.end()) + " ";
        log("play host: %s", line.c_str());
    }

    // The app's own frames are done: its render thread idles (vitaGL's state is global) before the play's starts.
    render_thread_.drain();
    back_.reset();
    capture_staging_.reset();
    set_vita_window_display({inner_, 960, 544, {}});
    crash::install({fs::absolute("local") / "crashes", false});
    log_memory("play host start");
    {   // 132vc1 test switch: vita_test_worker_priority.txt holds the workers' priority (64..191; the default 144)
        std::ifstream in(std::string(vita_data_root) + "/vita_test_worker_priority.txt");
        int priority = 0;
        if (in >> priority && priority >= 64 && priority <= 191) {
            set_worker_priority(priority);
            log("play host: vita_test_worker_priority.txt: worker threads at priority %d (test switch)", priority);
        }
    }
    threads_ = ThreadReport{};
    // The play loop's split (frame_profile.hpp, the --frame-overlay profile) and the OMEGA_ZONE slots, logged about
    // every 10 s with the heap and the allocations per frame: the in-level hot spots (research/VITA.md "Profiling").
    // 132vt2e: every 5 s, with the backend's draws and render-target scenes per frame and the draws merged into them
    // (gfx::CommandList::merge_contiguous_draws). 132vt3: then the GXM scenes per frame by why each began (MemoryStats::
    // SceneKind; research/VITA.md "GXM scenes").
    FrameProfile::frame_profile_sink() = [](const FrameProfile::Averages& a) {
        static double sum[FrameProfile::phase_count]{}, peak = 0.0, started = 0.0;
        static unsigned frames = 0, windows = 0;
        static std::uint32_t mallocs = g_mallocs;
        static std::uint64_t gxm[gfx::gxm::MemoryStats::scene_kinds]{}, gxm_total = 0;
        static std::uint64_t draws = 0, scenes = 0, merged = 0, state_set = 0, state_skipped = 0, triangles = 0, tested = 0, kept = 0, cached = 0, reallocs = 0, writes = 0, bytes = 0;
        static RenderWindow render_window;  // 132vg2
        if (started == 0.0) {
            started = now_seconds();
            log_play_threads(0);   // 132vc1: the baselines
            const auto g = gfx::gxm::memory_stats();
            render_window.begin(g);
            draws = g.draws_total;
            scenes = g.scenes_total;
            std::copy(std::begin(g.gxm_scenes), std::end(g.gxm_scenes), std::begin(gxm));
            gxm_total = g.gxm_scenes_total;
            merged = gfx::CommandList::merged_draws.load();
            state_set = g.gl_state_set;
            state_skipped = g.gl_state_skipped;
            triangles = g.triangles_total;
            tested = g.shadow_tested;
            kept = g.shadow_kept;
            cached = g.shadow_cached;
            reallocs = g.buffer_reallocs;
            writes = g.buffer_writes;
            bytes = g.buffer_bytes;
        }
        for (int p = 0; p < FrameProfile::phase_count; ++p) sum[p] += a.ms[std::size_t(p)] * a.frames;
        peak = std::max(peak, a.max_total);
        frames += a.frames;
        ++windows;
        const double span = now_seconds() - started;
        if (span < 5.0 || frames == 0) return;
        const auto g = gfx::gxm::memory_stats();
        const std::uint64_t merged_now = gfx::CommandList::merged_draws.load();
        log("play frames: %u in %.1f s = %.1f fps; ms/frame sim %.2f, script %.2f, render %.2f, present %.2f "
            "(longest frame %.1f); draws/frame %.0f (scenes %.1f, merged %.0f; GL state set %.0f, unchanged %.0f; "
            "triangles %.0f; shadow filter tested %.0f, kept %.0f, cached %.0f (%.0f KB); buffer updates %.0f new memory, %.0f in place, %.0f KB); "
            "%.0f allocations per frame; %s",
            frames, span, frames / span, sum[0] / frames, sum[1] / frames, sum[2] / frames, sum[3] / frames, peak,
            double(g.draws_total - draws) / frames, double(g.scenes_total - scenes) / frames,
            double(merged_now - merged) / frames, double(g.gl_state_set - state_set) / frames,
            double(g.gl_state_skipped - state_skipped) / frames, double(g.triangles_total - triangles) / frames,
            double(g.shadow_tested - tested) / frames, double(g.shadow_kept - kept) / frames,
            double(g.shadow_cached - cached) / frames, g.shadow_cache_bytes / 1024.0,
            double(g.buffer_reallocs - reallocs) / frames, double(g.buffer_writes - writes) / frames,
            double(g.buffer_bytes - bytes) / 1024.0 / frames, double(g_mallocs - mallocs) / frames,
            process_memory_line().c_str());
        {   // 132vc2: the small-object allocator (small_alloc.hpp), totals since the start
            static omega::small_alloc::Stats sa_last{};
            const auto sa = omega::small_alloc::stats();
            log("play small alloc (132vc2): %.0f KB of chunks held; per frame %.1f refills, %.1f flushes, %.1f refused "
                "(over the limit: malloc)",
                sa.chunk_bytes_held / 1024.0, double(sa.refills - sa_last.refills) / frames,
                double(sa.flushes - sa_last.flushes) / frames, double(sa.refused - sa_last.refused) / frames);
            sa_last = sa;
            static omega::ParticleTailProfile pt_last{};
            const auto pt = omega::particle_tail_profile();
            log("play particle tail (132vc2, %s): per frame %.1f tails, %.1f batches; ms/frame: work on the pool %.2f, "
                "the simulation thread's wait %.2f, its emit %.2f",
                omega::g_particle_tail.load() ? "on" : "off", double(pt.tails - pt_last.tails) / frames,
                double(pt.batches - pt_last.batches) / frames, double(pt.work_us - pt_last.work_us) / 1000.0 / frames,
                double(pt.wait_us - pt_last.wait_us) / 1000.0 / frames, double(pt.emit_us - pt_last.emit_us) / 1000.0 / frames);
            pt_last = pt;
        }
        if (g.shadow_checked || g.shadow_mismatched)
            log("play shadow cache check: %llu hits equal to the buffers, %llu not (all so far)",
                static_cast<unsigned long long>(g.shadow_checked), static_cast<unsigned long long>(g.shadow_mismatched));
        if (g.gxm_scenes_total != gxm_total) {
            std::string kinds;
            for (unsigned k = 0; k < gfx::gxm::MemoryStats::scene_kinds; ++k) {
                char item[64];
                std::snprintf(item, sizeof item, "%s%s %.1f", k ? ", " : "", gfx::gxm::scene_kind_name(k),
                              double(g.gxm_scenes[k] - gxm[k]) / frames);
                kinds += item;
            }
            log("play scenes: GXM scenes/frame %.1f (begun for: %s); target binds/frame %.1f",
                double(g.gxm_scenes_total - gxm_total) / frames, kinds.c_str(), double(g.scenes_total - scenes) / frames);
        }
        render_window.report(g, frames);  // 132vg2
        zone_report([](const char* text) { log("%s", text); }, int(frames));
        log_play_threads(frames);
        // 132vp1 (VITA.md "Playability, memory and coverage"): once a level is in (heap in use past 96 MB), one longer
        // report of the heap's holders, for the per-mission memory table.
        static bool level_report = false;
        if (!level_report && mallinfo().uordblks > (96 << 20)) {
            level_report = true;
            heap_profile::report("in the level", 32);
        }
        std::fill(std::begin(sum), std::end(sum), 0.0);
        peak = 0.0;
        started = now_seconds();
        frames = windows = 0;
        mallocs = g_mallocs;
        draws = g.draws_total;
        scenes = g.scenes_total;
        std::copy(std::begin(g.gxm_scenes), std::end(g.gxm_scenes), std::begin(gxm));
        gxm_total = g.gxm_scenes_total;
        merged = merged_now;
        state_set = g.gl_state_set;
        state_skipped = g.gl_state_skipped;
        triangles = g.triangles_total;
        tested = g.shadow_tested;
        kept = g.shadow_kept;
        cached = g.shadow_cached;
        reallocs = g.buffer_reallocs;
        writes = g.buffer_writes;
        bytes = g.buffer_bytes;
    };
    int code = 0;
    install_play_movie_hooks(true);
    std::thread subtitle_setup = start_subtitle_setup();
    heap_profile::enabled = true;
    try {
        code = omega_play_entry(int(storage.size()), pointers.data());
    } catch (const std::exception& e) {
        log("play host: stopped: %s", e.what());
        code = -1;
        play_error_ = e.what();
    }
    heap_profile::enabled = false;
    omega::terrain_reserve_flush();   // a failed load's terrain size, for the next one (research/VITA.md "Memory")
    if (subtitle_setup.joinable()) subtitle_setup.join();
    install_play_movie_hooks(false);
    std::cout.flush();
    std::cerr.flush();
    log("play host: ended (exit %d)", code);
    log_memory("play host end");
    threads_.report("play host");
    return code;
}

void App::no_disc(int frames) {
    std::vector<GuiVertexHost> v;
    gfx::BufferDesc bd;
    bd.bytes = 64 * 6 * sizeof(GuiVertexHost);
    bd.usage = gfx::Usage::dynamic;
    bd.bind = gfx::bind_vertices;
    auto& device = *display_.device;
    const auto vertices = device.create_buffer(bd, nullptr, "no-disc vertices");
    gfx::TextureDesc wd;
    wd.width = wd.height = 1;
    wd.usage = gfx::Usage::immutable;
    wd.bind = gfx::bind_sampled;
    const std::uint32_t white = 0xffffffffU;
    const gfx::Level level{&white, 4};
    const auto image = device.create_texture(wd, &level, {});
    const auto sampler = device.create_sampler({}, "sampler");
    for (int frame = 0; frame < frames && !quit_; ++frame) {
        const double started = now_seconds();
        poll();
        auto back = begin_frame();
        v.clear();
        const float t = float(frame) / 60.0F;
        const float bar[4]{0.8F, 0.2F, 0.1F, 1.0F};
        quad(v, -300, -20, -300 + 600 * std::fmod(t / 4.0F, 1.0F), 20, bar);
        for (int b = 0; b < 16; ++b) {
            const float on[4]{0.9F, 0.9F, 0.9F, 1.0F}, off[4]{0.2F, 0.25F, 0.3F, 1.0F};
            quad(v, -300 + b * 37.5F, 60, -270 + b * 37.5F, 90, (pad_.held >> b & 1) ? on : off);
        }
        device.write(*vertices, v.data(), v.size() * sizeof v[0], "no-disc vertices");
        gfx::CommandList list;
        list.set_target(back.get());
        list.set_viewport({0, 0, 960, 544, 0, 1});
        list.set_program(gfx::Program::gui);
        list.set_vertex_buffer(vertices.get(), sizeof(GuiVertexHost));
        list.set_texture(0, image.get());
        list.set_sampler(0, sampler.get());
        list.draw(static_cast<std::uint32_t>(v.size()), 0);
        device.submit(list);
        if (frame == 120) capture(back.get(), "capture_no_disc");
        end_frame(started);
    }
    report("no disc: pad test");
}

// Port-only (132vt2a, research/VITA.md "Movies"): the movie path's ARM-only code checked at boot against the
// reference on this CPU: idct_8x8_fast vs idct_8x8 on pseudo-random and extreme blocks, frame_to_rgba (NEON) vs
// frame_to_rgb on pseudo-random frames of two sizes; then their speeds (logged).
void movie_check() {
    std::uint32_t seed = 132;
    const auto rnd = [&] {
        seed = seed * 1103515245u + 12345u;
        return static_cast<int>((seed >> 16) & 0x7fff);
    };
    std::array<std::int32_t, 64> f{}, a{}, b{};
    int idct_bad = 0;
    for (int trial = 0; trial < 4000; ++trial) {
        const int range = trial % 3 == 0 ? 256 : trial % 3 == 1 ? 2049 : 16000;
        for (auto& v : f) v = rnd() % (1 + trial % 6) == 0 ? rnd() % (2 * range + 1) - range : 0;
        if (trial % 50 == 0)
            for (int i = 0; i < 64; ++i) f[i] = (mpeg2::idct_basis[i & 7][0] < 0) != (mpeg2::idct_basis[i >> 3][0] < 0) ? -2049 : 2049;
        mpeg2::idct_8x8(f.data(), a.data());
        mpeg2::idct_8x8_fast(f.data(), b.data());
        idct_bad += a != b;
    }
    // 132vt2a2: the decoder's NEON IDCT (16-bit, the dequantiser's -2048..2047) against idct_8x8.
    int neon_bad = 0;
    std::array<std::int16_t, 64> f16{}, o16{};
    for (int trial = 0; trial < 4000; ++trial) {
        const int range = trial % 3 == 0 ? 256 : 2048;
        for (auto& v : f) v = rnd() % (1 + trial % 6) == 0 ? std::clamp(rnd() % (2 * range + 1) - range, -2048, 2047) : 0;
        if (trial % 50 == 0)
            for (int i = 0; i < 64; ++i)
                f[i] = (mpeg2::idct_basis[i & 7][0] < 0) != (mpeg2::idct_basis[i >> 3][0] < 0) ? -2048 : 2047;
        if (trial % 50 == 25)
            for (int i = 0; i < 64; ++i) f[i] = (trial / 50) % 2 ? 2047 : -2048;
        for (int i = 0; i < 64; ++i) f16[std::size_t(i)] = static_cast<std::int16_t>(f[std::size_t(i)]);
        mpeg2::idct_8x8(f.data(), a.data());
        mpeg2::idct_8x8_neon(f16.data(), o16.data());
        bool same = true;
        for (int i = 0; i < 64; ++i) same = same && a[std::size_t(i)] == o16[std::size_t(i)];
        neon_bad += !same;
    }
    int rgba_bad = 0;
    for (const auto [w, h] : {std::pair{96, 64}, std::pair{50, 30}}) {
        mpeg2::Frame frame;
        frame.width = w;
        frame.height = h;
        frame.y.resize(std::size_t(w) * h);
        frame.u.resize(std::size_t((w + 1) / 2) * ((h + 1) / 2));
        frame.v.resize(frame.u.size());
        for (auto& v : frame.y) v = std::uint8_t(rnd());
        for (auto& v : frame.u) v = std::uint8_t(rnd());
        for (auto& v : frame.v) v = std::uint8_t(rnd());
        const auto rgb = movie::frame_to_rgb(frame);
        std::vector<std::uint8_t> rgba;
        movie::frame_to_rgba(frame, rgba);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i)
            rgba_bad += rgba[4 * i] != rgb[3 * i] || rgba[4 * i + 1] != rgb[3 * i + 1] || rgba[4 * i + 2] != rgb[3 * i + 2] ||
                        rgba[4 * i + 3] != 255;
    }
    // Speeds: 6720 coded blocks (one 640 x 448 picture's worth) of a typical mix, and one 640 x 448 conversion.
    std::vector<std::array<std::int32_t, 64>> blocks(6720);
    for (std::size_t k = 0; k < blocks.size(); ++k) {
        blocks[k].fill(0);
        const int n = k % 4 == 0 ? 1 : 2 + int(k % 12);
        for (int i = 0; i < n; ++i) blocks[k][rnd() % (i < 3 ? 10 : 64)] = rnd() % 401 - 200;
        if (k % 2) blocks[k][63] ^= 1;
    }
    double t = now_seconds();
    for (const auto& blk : blocks) mpeg2::idct_8x8(blk.data(), a.data());
    const double ref_ms = (now_seconds() - t) * 1000.0;
    t = now_seconds();
    for (const auto& blk : blocks) mpeg2::idct_8x8_fast(blk.data(), b.data());
    const double fast_ms = (now_seconds() - t) * 1000.0;
    std::vector<std::array<std::int16_t, 64>> blocks16(blocks.size());
    for (std::size_t k = 0; k < blocks.size(); ++k)
        for (int i = 0; i < 64; ++i) blocks16[k][std::size_t(i)] = static_cast<std::int16_t>(blocks[k][std::size_t(i)]);
    t = now_seconds();
    for (const auto& blk : blocks16) mpeg2::idct_8x8_neon(blk.data(), o16.data());
    const double neon_ms = (now_seconds() - t) * 1000.0;
    mpeg2::Frame big;
    big.width = 640;
    big.height = 448;
    big.y.assign(640 * 448, 100);
    big.u.assign(320 * 224, 120);
    big.v.assign(320 * 224, 140);
    std::vector<std::uint8_t> rgba;
    movie::frame_to_rgba(big, rgba);
    t = now_seconds();
    movie::frame_to_rgba(big, rgba);
    const double rgba_ms = (now_seconds() - t) * 1000.0;
    log("movie check (132vt2a / 132vt2a2): idct_8x8_fast %s (%d of 4000 blocks differ), idct_8x8_neon %s (%d of 4000 "
        "blocks differ), frame_to_rgba %s (%d pixels differ); 6720 blocks: idct_8x8 %.1f ms, idct_8x8_fast %.1f ms, "
        "idct_8x8_neon %.1f ms; 640 x 448 RGBA %.1f ms",
        idct_bad ? "FAILED" : "matches", idct_bad, neon_bad ? "FAILED" : "matches", neon_bad,
        rgba_bad ? "FAILED" : "matches", rgba_bad, ref_ms, fast_ms, neon_ms, rgba_ms);
}

// Port-only (research/VITA.md "Audio"): the sound host's GameSoundHost calls under the lock the sound pump mixes under,
// so gameSound (on the simulation thread) and the mix (on the pump thread) never touch the host at once. Every call is
// forwarded unchanged; the lock is recursive because a 003a85e0 reply re-enters the host through the manager.
class LockedSoundHost final : public GameSoundHost {
public:
    LockedSoundHost(SoundIopHost& host, std::recursive_mutex& lock) : h_(host), m_(lock) {}
    using Guard = std::lock_guard<std::recursive_mutex>;
    std::uint32_t owner_serial(std::uint32_t o) override { Guard g(m_); return h_.owner_serial(o); }
    GameSoundOwnerView owner_view(std::uint32_t o) override { Guard g(m_); return h_.owner_view(o); }
    std::uint8_t owner_state_108(std::uint32_t o) override { Guard g(m_); return h_.owner_state_108(o); }
    void owner_clear_2dc8(std::uint32_t o) override { Guard g(m_); h_.owner_clear_2dc8(o); }
    void lip_sync_391810(std::uint32_t o, const GameSoundResource& r) override { Guard g(m_); h_.lip_sync_391810(o, r); }
    std::optional<std::array<float, 4>> listener_4ffc0c() override { Guard g(m_); return h_.listener_4ffc0c(); }
    std::optional<GameSoundCamera> camera_4ffc14() override { Guard g(m_); return h_.camera_4ffc14(); }
    bool hud_4ffc2c() override { Guard g(m_); return h_.hud_4ffc2c(); }
    void subtitle_245e00(const std::array<std::uint8_t, 16>& n) override { Guard g(m_); h_.subtitle_245e00(n); }
    std::string level_name_4ffc90() override { Guard g(m_); return h_.level_name_4ffc90(); }
    std::uint8_t language_55d470() override { Guard g(m_); return h_.language_55d470(); }
    std::uint8_t game_mode_4ffba0() override { Guard g(m_); return h_.game_mode_4ffba0(); }
    bool multiplayer_5721c8() override { Guard g(m_); return h_.multiplayer_5721c8(); }
    std::uint32_t output_mode_4ffd2c_14() override { Guard g(m_); return h_.output_mode_4ffd2c_14(); }
    GameSoundHog open_hog_3f96e0(const std::string& p) override { Guard g(m_); return h_.open_hog_3f96e0(p); }
    void load_lpd_3eae60(std::size_t s, const GameSoundResource& r) override { Guard g(m_); h_.load_lpd_3eae60(s, r); }
    void release_lpd_3eaa60(std::size_t s, const GameSoundResource& r) override { Guard g(m_); h_.release_lpd_3eaa60(s, r); }
    void schedule_3ea1d0(std::int32_t d, std::function<void()> e) override { Guard g(m_); h_.schedule_3ea1d0(d, std::move(e)); }
    std::int32_t snd(std::uint32_t a, const std::vector<std::int32_t>& args) override { Guard g(m_); return h_.snd(a, args); }

private:
    SoundIopHost& h_;
    std::recursive_mutex& m_;
};

// Port-only (research/VITA.md "Audio"): the sound mix on its own thread (core 2, the audio priority), pulled by the
// output instead of pushed by the simulation frame. The Windows hosts mix 800 frames per 60 Hz simulation step, which
// only keeps a real-time output fed while every step takes under 16.7 ms; on the handheld a slow frame (the agent
// screen's 40 ms) starved SceAudioOut. As the PS2's SPU2 / IOP run on their own clock beside the EE, the pump keeps
// the device's queue at `target` frames by mixing 512-frame grains (SoundIopHost::mix, unchanged) as the output
// drains it. While a movie plays the window plays the movie's PCM instead; the pump then mixes on the wall clock and
// drops the result, as the window drops the mix during a movie. The movie's PCM (132vt2a): the simulation thread
// hands it over without waiting (push_movie; a picture's read brings up to ~0.75 s at once, which the device's
// blocking submit used to wait out on the simulation thread) and the pump feeds it to the device as the output drains
// it, keeping `target` frames queued; what is left when the movie ends is dropped (the window's device goes on to the
// sound mix the same way). The movie's clock (132vt2a2): movie_begin starts it, movie_played_seconds is the movie PCM
// fed to the device less what it still holds, the sound heard so far; AheadMovie paces the pictures by it. 132vf3: the
// movie's PCM is held from movie_begin until AheadMovie's movie_release (its first picture on screen), and the clock
// counts from the movie's first frame the device took (the mix queued before it no longer counts as the movie's).
class SoundPump {
public:
    static constexpr std::size_t grain = 512;
    SoundPump(SoundIopHost& host, std::recursive_mutex& lock, audio::Device& device, std::size_t target)
        : host_(host), lock_(lock), device_(device), target_(target), thread_([this] { run(); }) {}
    ~SoundPump() {
        stop_ = true;
        thread_.join();
    }
    void set_movie(bool on) {
        // 132vt2a2: the end of a movie drops its PCM here, on the caller's thread, before the next movie's decode can
        // push any (the pump thread's own check came later and could drop the next movie's first second).
        if (movie_.exchange(on) && !on) drop_movie();
    }
    // A movie's interleaved stereo PCM, queued for the device (never waits).
    void push_movie(const std::vector<std::int16_t>& pcm) {
        std::lock_guard<std::mutex> g(fifo_lock_);
        fifo_.insert(fifo_.end(), pcm.begin(), pcm.end());
        movie_pushed_ += pcm.size() / 2;
    }
    // A new movie (132vt2a2): drops any movie PCM still waiting and restarts the movie clock. 132vf3: the movie's
    // PCM is held (queued here, not fed to the device) until movie_release: the picture's first presentation.
    void movie_begin() {
        drop_movie();
        movie_hold_ = true;
        movie_origin_ = no_origin;
    }
    // 132vf3: the movie's sound may start (its first picture is on screen).
    void movie_release() { movie_hold_ = false; }
    bool movie_started() const { return movie_origin_ != no_origin; }
    // The movie's PCM heard since its first frame went to the device, in seconds (132vf3): the frames the device has
    // taken out of its queue (submitted less queued) past the movie's first frame (the frames submitted before it, the
    // front end's mix among them, are not the movie's), less the grain SceAudioOut holds while the one before plays.
    // 0 until the movie's sound starts.
    double movie_played_seconds() {
        const std::uint64_t origin = movie_origin_;
        if (origin == no_origin) return 0.0;
        const auto s = device_.stats();
        const std::uint64_t taken = s.submitted_frames - std::min<std::uint64_t>(s.queued_frames, s.submitted_frames);
        const std::uint64_t heard = taken > origin + grain ? taken - origin - grain : 0;
        return double(heard) / audio::Mixer::output_rate;
    }
    // Movie PCM pushed and not yet heard (the pump's FIFO and the device's queue), in frames.
    std::size_t movie_buffered() { return movie_waiting() + device_.stats().queued_frames; }
    int take_peak() { return peak_.exchange(0); }
    std::uint64_t mixed() const { return mixed_; }
    double busy_seconds() const { return busy_us_ * 1e-6; }
    // Movie PCM: frames pushed, fed to the device, dropped at a movie's end, and queued in the pump now.
    std::uint64_t movie_pushed() const { return movie_pushed_; }
    std::uint64_t movie_fed() const { return movie_fed_; }
    std::uint64_t movie_dropped() const { return movie_dropped_; }
    std::size_t movie_waiting() {
        std::lock_guard<std::mutex> g(fifo_lock_);
        return (fifo_.size() - fifo_read_) / 2;
    }

private:
    // Up to one grain of the movie's PCM into `out`; false when none is waiting.
    bool take_movie(std::vector<std::int16_t>& out) {
        std::lock_guard<std::mutex> g(fifo_lock_);
        const std::size_t n = std::min(fifo_.size() - fifo_read_, grain * 2);
        if (!n) return false;
        out.assign(fifo_.begin() + std::ptrdiff_t(fifo_read_), fifo_.begin() + std::ptrdiff_t(fifo_read_ + n));
        fifo_read_ += n;
        if (fifo_read_ >= 1 << 18) {
            fifo_.erase(fifo_.begin(), fifo_.begin() + std::ptrdiff_t(fifo_read_));
            fifo_read_ = 0;
        }
        return true;
    }
    void drop_movie() {
        std::lock_guard<std::mutex> g(fifo_lock_);
        movie_dropped_ += (fifo_.size() - fifo_read_) / 2;
        fifo_.clear();
        fifo_read_ = 0;
    }
    void run() {
        enter_thread_role(ThreadRole::audio, "sound mix");
        std::vector<std::int16_t> pcm, movie_pcm;
        bool was_movie = false;
        double base = now_seconds();
        std::uint64_t base_mixed = 0;
        while (!stop_) {
            const bool movie = movie_;
            if (movie != was_movie) {
                was_movie = movie;
                base = now_seconds();
                base_mixed = mixed_;
            }
            bool worked = false;
            if (movie && !movie_hold_) {
                // The device holds prime x 2 = 6144 frames: below `target` a grain always fits, so this never waits.
                while (device_.stats().queued_frames < target_ && take_movie(movie_pcm)) {
                    if (movie_origin_ == no_origin) movie_origin_ = device_.stats().submitted_frames;
                    device_.submit(movie_pcm);
                    movie_fed_ += movie_pcm.size() / 2;
                    worked = true;
                }
            }
            const bool due = movie ? (now_seconds() - base) * audio::Mixer::output_rate >= double(mixed_ - base_mixed + grain)
                                   : device_.stats().queued_frames < target_;
            if (!due) {
                if (!worked) sceKernelDelayThread(2000);
                continue;
            }
            const std::uint64_t t0 = sceKernelGetProcessTimeWide();
            {
                ThreadBusyScope busy;
                pcm.clear();
                if (!failed_) {
                    // A sound host error ends the mix (logged; silence from then on) instead of std::terminate on
                    // this thread.
                    try {
                        std::lock_guard<std::recursive_mutex> g(lock_);
                        host_.mix(grain, pcm);
                    } catch (const std::exception& e) {
                        failed_ = true;
                        log("sound mix: stopped (%s); the sound is silent from here", e.what());
                    }
                }
                pcm.resize(grain * 2, 0);
                mixed_ += grain;
            }
            busy_us_ += sceKernelGetProcessTimeWide() - t0;
            if (movie) continue;
            int p = 0;
            for (const std::int16_t v : pcm) p = std::max(p, std::abs(int(v)));
            for (int seen = peak_.load(); p > seen && !peak_.compare_exchange_weak(seen, p);) {}
            device_.submit(pcm);
        }
    }
    SoundIopHost& host_;
    std::recursive_mutex& lock_;
    audio::Device& device_;
    const std::size_t target_;
    std::atomic<bool> stop_{false}, movie_{false};
    bool failed_ = false;
    std::atomic<int> peak_{0};
    std::atomic<std::uint64_t> mixed_{0}, busy_us_{0};
    std::mutex fifo_lock_;
    std::vector<std::int16_t> fifo_;
    std::size_t fifo_read_ = 0;
    std::atomic<std::uint64_t> movie_pushed_{0}, movie_fed_{0}, movie_dropped_{0};
    static constexpr std::uint64_t no_origin = ~std::uint64_t(0);
    std::atomic<bool> movie_hold_{false};
    std::atomic<std::uint64_t> movie_origin_{no_origin};   // the device's submitted frames before the movie's first
    std::thread thread_;
};

// Port-only (132vt2a2, research/VITA.md "Movies"): the movie's parallel work on the cores the front end leaves idle
// while a movie plays. Two helper threads (core 0 and core 1, below every other thread there) and the calling thread
// run a batch's jobs, each index once; run returns when all are done. The MPEG-2 decoder's slices
// (mpeg2::Decoder::set_slice_runner: a picture's slices are independent and write their own macroblocks) and the
// RGBA conversion's 16-row bands (frame_to_rgba_rows) go through it: the same bytes, from up to three cores.
class MoviePool {
public:
    static MoviePool& get() {
        static MoviePool* pool = new MoviePool();   // never destroyed: its helpers live as long as the app
        return *pool;
    }
    // job(0) .. job(count - 1) on the helpers and this thread; the jobs must not throw.
    void run(int count, const std::function<void(int)>& job) {
        if (count <= 1) {
            if (count == 1) job(0);
            return;
        }
        Batch b;
        b.count = count;
        b.job = &job;
        {
            std::lock_guard<std::mutex> g(mutex_);
            batches_.push_back(&b);
        }
        cv_.notify_all();
        const int did = work(b);
        std::unique_lock<std::mutex> lock(mutex_);
        b.done += did;
        done_cv_.wait(lock, [&] { return b.done == b.count && b.users == 0; });
        batches_.erase(std::find(batches_.begin(), batches_.end(), &b));
    }

private:
    struct Batch {
        int count = 0;
        const std::function<void(int)>* job = nullptr;
        std::atomic<int> next{0};
        int done = 0, users = 0;   // under mutex_
    };
    MoviePool() {
        for (const unsigned core : {0u, 1u}) std::thread([this, core] { helper(core); }).detach();
    }
    static int work(Batch& b) {
        int did = 0;
        for (int i; (i = b.next.fetch_add(1)) < b.count; ++did) (*b.job)(i);
        return did;
    }
    void helper(unsigned core) {
        enter_thread_role(core == 0 ? ThreadRole::simulation : ThreadRole::render, "movie helper " + std::to_string(core));
        sceKernelChangeThreadPriority(sceKernelGetThreadId(), 165);   // below the movie colour thread's 160
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            Batch* b = nullptr;
            cv_.wait(lock, [&] {
                for (Batch* x : batches_)
                    if (x->next.load() < x->count) {
                        b = x;
                        return true;
                    }
                return false;
            });
            ++b->users;
            lock.unlock();
            const int did = work(*b);
            lock.lock();
            --b->users;
            b->done += did;
            if (b->done == b->count && b->users == 0) done_cv_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_, done_cv_;
    std::vector<Batch*> batches_;
};

// frame_to_rgba through the movie pool, in 16-row bands (frame_to_rgba_rows: the same bytes).
void movie_rgba_banded(const mpeg2::Frame& frame, std::vector<std::uint8_t>& out) {
    out.resize(std::size_t(std::max(frame.width, 0)) * std::max(frame.height, 0) * 4);
    std::exception_ptr error;
    std::mutex error_lock;
    MoviePool::get().run((frame.height + 15) / 16, [&](int band) {
        try {
            movie::frame_to_rgba_rows(frame, out.data(), band * 16, std::min(frame.height, band * 16 + 16));
        } catch (...) {
            std::lock_guard<std::mutex> g(error_lock);
            if (!error) error = std::current_exception();
        }
    });
    if (error) std::rethrow_exception(error);
}

// Port-only (132vt2a / 132vt2a2, research/VITA.md "Movies"): a movie decoded ahead of the session. The front end's
// movie player (FrontendSession::fn_3f8220) asks for a picture every second 60 Hz frame and used to decode it there,
// ~45 ms for one 640 x 448 MPEG-2 picture on the handheld's simulation thread, then convert it to RGBA (~40 ms more)
// in the same frame: 10-15 pictures a second. Here one thread ("movie decode", the loader's core 2) runs the movie's
// own next_frame / audio_format / take_audio in the session's order, its slices spread over the movie pool, and a
// second ("movie colour", core 0 below the simulation thread, which idles in end_frame while a movie plays) converts
// each picture to RGBA in bands over the pool, a few pictures ahead. The audio (132vt2a2) goes from the decode thread
// straight to the sound pump, and the decode thread reads the container ahead of the pictures (read_ahead) to keep
// about a second of it there, so a slow picture never starves the sound. The sound is the clock: the session's
// next_frame hands over the picture due at the movie PCM heard so far (SoundPump::movie_played_seconds, 30000 / 1001
// pictures a second), holding the shown one while the next is not due (repeated_picture: the session's serial stays)
// and skipping pictures that are late; without the movie's sound the wall clock from the first picture paces them.
// The pictures, their order, the audio and the end are the movie's own; only when they are produced and which are
// shown when moves.
class AheadMovie final : public movie::MovieStream {
public:
    explicit AheadMovie(std::unique_ptr<movie::MovieStream> inner, std::string name)
        : inner_(std::move(inner)), name_(std::move(name)), aspect_(inner_->picture_aspect()), sink_(audio_sink_) {
        if (active_) active_->stop();
        active_ = this;
        // Port-only test switch (132vf3, research/VITA.md "Movies"): with ux0:data/omega_strain/
        // vita_test_movie_sound_early.txt the movie's sound starts at once and no picture skips its RGBA (the sync
        // before 132vf3), for before / after comparisons in one build.
        sound_early_ = vita_file_size(std::string(vita_data_root) + "/vita_test_movie_sound_early.txt") >= 0;
        if (sound_early_) log("movie %s: vita_test_movie_sound_early.txt present: the sound starts at once (test switch)",
                              name_.c_str());
        if (sink_) sink_->movie_begin();
        if (sink_ && sound_early_) sink_->movie_release();
        decode_thread_ = std::thread([this] { decode(); });
        colour_thread_ = std::thread([this] { colour(); });
    }
    ~AheadMovie() override {
        std::size_t waiting = 0, converting = 0, ready = 0;   // 132vt2b2: where the pictures were at the end
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting = decoded_.size();
            converting = converting_;
            ready = ready_.size();
        }
        stop();
        if (active_ == this) active_ = nullptr;
        log("movie %s (decoded ahead, 132vt2a2): %llu pictures decoded, %llu shown, %llu skipped late (%llu of them "
            "before their RGBA, 132vf3), %llu held; decode %.1f ms mean (%.1f max), RGBA %.1f ms mean (%.1f max); paced "
            "by %s; audio read ahead %llu times; the decoder was behind the clock %u times; at the end %u decoded "
            "waiting, %u converting, %u ready",
            name_.c_str(), static_cast<unsigned long long>(pictures_), static_cast<unsigned long long>(shown_),
            static_cast<unsigned long long>(skipped_ + colour_skipped_), static_cast<unsigned long long>(colour_skipped_),
            static_cast<unsigned long long>(held_), pictures_ ? decode_ms_ / pictures_ : 0.0, decode_max_,
            pictures_ ? colour_ms_ / pictures_ : 0.0, colour_max_, audio_fed_ ? "the movie's sound" : "the wall clock",
            static_cast<unsigned long long>(read_aheads_.load()), behind_, unsigned(waiting), unsigned(converting),
            unsigned(ready));
        log_av_offset("at the end");
    }
    // The movie being shown (null when none).
    static AheadMovie* active() { return active_; }
    // Stops the shown movie's threads (before the next movie opens: the disc's movies share one stream).
    static void stop_active() {
        if (active_) active_->stop();
    }
    // The sound pump the movies' 48 kHz stereo PCM goes to (null: the session's take_audio hands it over).
    static void set_audio_sink(SoundPump* sink) { audio_sink_ = sink; }
    // The movie colour thread's core: the render core (the play host) or the simulation core (the app's front end).
    static void set_colour_on_render_core(bool on) { colour_on_render_core_ = on; }

    bool next_frame(mpeg2::Frame& frame) override {
        repeated_ = false;
        // The end (or an error) stays the answer: the decode thread has stopped.
        if (current_ && current_->error) std::rethrow_exception(current_->error);
        if (current_ && current_->ended) return false;
        std::unique_ptr<Item> next;
        std::vector<std::unique_ptr<Item>> late;
        if (!current_) {
            // The first picture is waited for (the movie's start). 132vf3: the clock and the sound start at the next
            // call, once this picture has been presented (the frame that hands it over can stall: INTRO's first one
            // waited ~3 s for the subtitle projection on the handheld while its sound already played).
            std::unique_lock<std::mutex> lock(mutex_);
            if (ready_.empty()) cv_.wait(lock, [this] { return stop_ || !ready_.empty(); });
            if (ready_.empty()) return false;   // stopped (the next movie opened)
            next = std::move(ready_.front());
            ready_.pop_front();
        } else {
            if (!started_) {
                started_ = true;
                start_ = now_seconds();
                audio_changed_ = start_;
                if (sink_) sink_->movie_release();
            }
            const double movie_time = clock();
            const auto due = static_cast<std::uint64_t>(std::max(0.0, movie_time * picture_rate));
            std::lock_guard<std::mutex> lock(mutex_);
            due_time_ = movie_time;
            due_wall_ = now_seconds();
            if (current_->index >= due) {
                // The shown picture is still due: held.
            } else if (ready_.empty()) {
                ++behind_;
            } else {
                while (!ready_.empty() && (!next || ready_.front()->index <= due)) {
                    if (next) late.push_back(std::move(next));
                    next = std::move(ready_.front());
                    ready_.pop_front();
                    if (next->ended || next->error) break;
                }
            }
        }
        if (!next) {
            ++held_;
            repeated_ = true;
            return true;
        }
        skipped_ += late.size();
        for (auto& item : late) recycle(std::move(item));
        if (current_) recycle(std::move(current_));
        cv_.notify_all();
        current_ = std::move(next);
        if (current_->error) std::rethrow_exception(current_->error);
        if (current_->ended) return false;
        std::swap(frame, current_->frame);
        aspect_ = current_->aspect;
        measure_av_offset(current_->index);
        if (++shown_ % 300 == 0) {
            log("movie %s: %llu pictures shown (%llu skipped late, %llu held); decode %.1f ms mean, RGBA %.1f ms mean; "
                "clock %.2f s; movie PCM buffered %u frames",
                name_.c_str(), static_cast<unsigned long long>(shown_),
                static_cast<unsigned long long>(skipped_ + colour_skipped_), static_cast<unsigned long long>(held_),
                pictures_ ? decode_ms_ / pictures_ : 0.0, pictures_ ? colour_ms_ / pictures_ : 0.0, clock(),
                sink_ ? unsigned(sink_->movie_buffered()) : 0u);
            log_av_offset("so far");
        }
        return true;
    }
    bool repeated_picture() const override { return repeated_; }
    bool audio_format(int& rate, int& channels) override {
        if (!current_ || !current_->has_format) return false;
        rate = current_->rate;
        channels = current_->channels;
        return true;
    }
    void take_audio(std::vector<std::int16_t>& out) override {
        if (!current_) return;
        out.insert(out.end(), current_->audio.begin(), current_->audio.end());
        current_->audio.clear();
    }
    float picture_aspect() const override { return aspect_; }
    // The RGBA of the picture next_frame handed over (frame_to_rgba's), swapped into `out`; false when there is none.
    bool take_rgba(std::vector<std::uint8_t>& out) {
        if (!current_ || current_->ended || current_->rgba.empty()) return false;
        std::swap(out, current_->rgba);
        current_->rgba.clear();
        return true;
    }

private:
    struct Item {
        mpeg2::Frame frame;
        std::vector<std::int16_t> audio;
        std::vector<std::uint8_t> rgba;
        bool ended = false, has_format = false;
        int rate = 0, channels = 0;
        float aspect = 0.0F;
        std::uint64_t index = 0;   // the picture's number in the movie (the end: one past the last)
        std::exception_ptr error;
    };
    static constexpr std::size_t depth = 4;               // pictures decoded or converted ahead of the session
    static constexpr std::size_t audio_ahead = 48000;     // movie PCM kept buffered ahead of the sound heard (1 s)
    static constexpr double picture_rate = 30000.0 / 1001.0;

    // Port-only diagnostic (132vf3): the A/V offset, the handed-over picture's time less the movie sound heard (+: the
    // picture ahead of the sound, -: behind it), from the first picture handed over while the sound is heard. The
    // picture reaches the screen at the frame's present, about one frame after the hand-over measured here.
    void measure_av_offset(std::uint64_t index) {
        if (!started_ || !sink_ || !audio_fed_ || !sink_->movie_started()) return;
        const double heard = sink_->movie_played_seconds();
        if (heard <= 0.0) return;
        const double offset = double(index) / picture_rate - heard;
        if (!av_count_) av_start_ = offset;
        av_min_ = av_count_ ? std::min(av_min_, offset) : offset;
        av_max_ = av_count_ ? std::max(av_max_, offset) : offset;
        av_sum_ += offset;
        av_abs_sum_ += std::abs(offset);
        if (std::abs(offset) > 0.1) ++av_over_100ms_;
        ++av_count_;
    }
    void log_av_offset(const char* when) {
        if (!av_count_) {
            log("movie %s: A/V offset (132vf3) %s: not measured (no movie sound heard)", name_.c_str(), when);
            return;
        }
        log("movie %s: A/V offset (132vf3, picture time minus the sound heard, at hand-over; + picture ahead) %s: "
            "start %+.0f ms, mean %+.0f ms (mean |offset| %.0f ms), min %+.0f ms, max %+.0f ms; %llu of %llu pictures "
            "off by more than 100 ms",
            name_.c_str(), when, av_start_ * 1000.0, av_sum_ / double(av_count_) * 1000.0,
            av_abs_sum_ / double(av_count_) * 1000.0, av_min_ * 1000.0, av_max_ * 1000.0,
            static_cast<unsigned long long>(av_over_100ms_), static_cast<unsigned long long>(av_count_));
    }
    // The movie's time (seconds) the shown picture should be at: the movie PCM heard; when it has not moved for
    // 0.15 s (not started yet, starved, or the sound ended before the pictures) the wall clock from its last value.
    double clock() {
        const double wall = now_seconds();
        if (!sink_ || !audio_fed_) return wall - start_;
        const double heard = sink_->movie_played_seconds();
        if (heard != audio_last_) {
            audio_last_ = heard;
            audio_changed_ = wall;
        }
        return wall - audio_changed_ < 0.15 ? heard : audio_last_ + (wall - audio_changed_);
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (decode_thread_.joinable()) decode_thread_.join();
        if (colour_thread_.joinable()) colour_thread_.join();
    }
    void recycle(std::unique_ptr<Item> item) {
        item->audio.clear();
        item->ended = item->has_format = false;
        item->error = nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(std::move(item));
    }
    // The movie's PCM just taken from inner_ (decode thread): to the sound pump once the format is known and is the
    // window's 48 kHz stereo; other formats are dropped (as front_end drops them). Kept in `pcm` without a pump.
    void deliver(std::vector<std::int16_t>& pcm) {
        if (!sink_) return;
        if (!format_) {
            int rate = 0, channels = 0;
            if (inner_->audio_format(rate, channels)) format_ = rate == 48000 && channels == 2 ? 1 : 2;
        }
        if (!format_) {
            early_pcm_.insert(early_pcm_.end(), pcm.begin(), pcm.end());
        } else if (format_ == 1) {
            if (!early_pcm_.empty()) {
                sink_->push_movie(early_pcm_);
                early_pcm_.clear();
            }
            if (!pcm.empty()) {
                sink_->push_movie(pcm);
                audio_fed_ = true;
            }
        }
        pcm.clear();
    }
    // Reads the container ahead of the pictures while less than audio_ahead frames of its sound wait to be heard.
    void top_up(std::vector<std::int16_t>& pcm) {
        while (sink_ && !ahead_done_ && !stop_ && sink_->movie_buffered() < audio_ahead) {
            try {
                if (!inner_->read_ahead()) {
                    ahead_done_ = true;
                    return;
                }
                inner_->take_audio(pcm);
                deliver(pcm);
                ++read_aheads_;
            } catch (...) {
                ahead_done_ = true;   // next_frame meets the error in its own read
            }
        }
    }
    void decode() {
        enter_thread_role(ThreadRole::loader, "movie decode");
        std::vector<std::int16_t> pcm;
        std::uint64_t index = 0;
        for (bool ended = false; !ended;) {
            top_up(pcm);
            std::unique_ptr<Item> item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const auto room = [this] { return decoded_.size() + ready_.size() + converting_ < depth; };
                if (!stop_ && !room()) cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return stop_ || room(); });
                if (stop_) return;
                if (!room()) continue;   // the sound topped up again meanwhile
                if (!free_.empty()) {
                    item = std::move(free_.back());
                    free_.pop_back();
                }
            }
            if (!item) item = std::make_unique<Item>();
            const double t = now_seconds();
            try {
                item->ended = !inner_->next_frame(item->frame);
                item->has_format = inner_->audio_format(item->rate, item->channels);
                inner_->take_audio(item->audio);
                deliver(item->audio);
                item->aspect = inner_->picture_aspect();
            } catch (...) {
                item->error = std::current_exception();
            }
            ended = item->ended || item->error;
            item->index = index;
            if (!ended) ++index;
            const double ms = (now_seconds() - t) * 1000.0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ended) {
                    decode_ms_ += ms;
                    decode_max_ = std::max(decode_max_, ms);
                    ++pictures_;
                }
                decoded_.push_back(std::move(item));
            }
            cv_.notify_all();
        }
    }
    void colour() {
        // 132vt2b2: in the play host the session thread draws and presents on core 0 itself (no render thread), so
        // the conversion goes to the render core, which its front end leaves idle.
        if (colour_on_render_core_) {
            enter_thread_role(ThreadRole::render, "movie colour");
        } else {
            enter_thread_role(ThreadRole::simulation, "movie colour");
        }
        sceKernelChangeThreadPriority(sceKernelGetThreadId(), 160);   // below the simulation thread's 128
        for (bool ended = false; !ended;) {
            std::unique_ptr<Item> item;
            bool late = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !decoded_.empty(); });
                if (stop_) return;
                item = std::move(decoded_.front());
                decoded_.pop_front();
                // 132vf3: a picture the session would skip anyway is not converted, so a late pipeline rejoins the
                // clock: the next decoded picture is already waiting here and is itself due by the movie time now (the
                // session's last clock, moved on by the wall clock). Only with the sound on the pump (the PCM left
                // the item at decode), and never the end or an error.
                if (sink_ && audio_fed_ && !sound_early_ && due_time_ >= 0.0 && !decoded_.empty() && !item->ended &&
                    !item->error && !decoded_.front()->ended && !decoded_.front()->error) {
                    const double now_due = (due_time_ + (now_seconds() - due_wall_)) * picture_rate;
                    late = double(decoded_.front()->index) <= now_due;
                }
                if (!late) ++converting_;
            }
            if (late) {
                ++colour_skipped_;
                recycle(std::move(item));
                cv_.notify_all();
                continue;
            }
            ended = item->ended || item->error;
            double ms = 0.0;
            if (!ended) {
                const double t = now_seconds();
                try {
                    movie_rgba_banded(item->frame, item->rgba);
                } catch (...) {
                    item->error = std::current_exception();
                    ended = true;
                }
                ms = (now_seconds() - t) * 1000.0;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --converting_;
                colour_ms_ += ms;
                colour_max_ = std::max(colour_max_, ms);
                ready_.push_back(std::move(item));
            }
            cv_.notify_all();
        }
    }

    static inline AheadMovie* active_ = nullptr;
    static inline SoundPump* audio_sink_ = nullptr;
    static inline bool colour_on_render_core_ = false;
    std::unique_ptr<movie::MovieStream> inner_;
    const std::string name_;
    float aspect_;
    SoundPump* const sink_;
    // The session's side only.
    std::unique_ptr<Item> current_;
    bool repeated_ = false;
    bool started_ = false;   // 132vf3: the first picture was presented; the sound and the clock run
    bool sound_early_ = false;   // 132vf3's test switch (set before the threads start)
    double start_ = 0.0, audio_last_ = -1.0, audio_changed_ = 0.0;
    std::uint64_t shown_ = 0, skipped_ = 0, held_ = 0;
    unsigned behind_ = 0;
    double av_start_ = 0.0, av_sum_ = 0.0, av_abs_sum_ = 0.0, av_min_ = 0.0, av_max_ = 0.0;
    std::uint64_t av_count_ = 0, av_over_100ms_ = 0;
    // The decode thread's side only.
    int format_ = 0;   // 0 unknown, 1 the pump's 48 kHz stereo, 2 another (dropped)
    std::vector<std::int16_t> early_pcm_;
    bool ahead_done_ = false;
    std::atomic<std::uint64_t> read_aheads_{0};
    std::atomic<bool> audio_fed_{false};
    // Shared, under mutex_.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Item>> decoded_, ready_, free_;
    std::size_t converting_ = 0;
    std::atomic<bool> stop_{false};
    std::uint64_t pictures_ = 0;
    double decode_ms_ = 0.0, decode_max_ = 0.0, colour_ms_ = 0.0, colour_max_ = 0.0;
    // 132vf3: the movie time the session's last next_frame computed and when (a negative time: not started yet), and
    // the pictures the colour thread skipped as late.
    double due_time_ = -1.0, due_wall_ = 0.0;
    std::atomic<std::uint64_t> colour_skipped_{0};
    std::thread decode_thread_, colour_thread_;
};

// Port-only (132vt2a2, research/VITA.md "Movies"): INTRO's first pictures through the decoder as the movies run here
// (the NEON paths, the slices over the movie pool) against tests/movie_tests.cpp's CRC of the sequential reference
// decode (test_movie_parallel_slices); logged with the decode's speed.
constexpr std::uint32_t intro_head_crc = 0x84dfc728u;
constexpr int intro_head_pictures = 45;
void intro_check(const std::function<std::unique_ptr<movie::MovieStream>(const std::string&)>& open) {
    const double start = now_seconds();
    try {
        auto m = open ? open("INTRO.PSS") : nullptr;
        if (!m) {
            log("movie check (132vt2a2): INTRO.PSS not opened; the decode check is skipped");
            return;
        }
        std::uint32_t crc = 0;
        int n = 0;
        double decode_ms = 0.0, worst = 0.0;
        mpeg2::Frame f;
        for (; n < intro_head_pictures; ++n) {
            const double t = now_seconds();
            if (!m->next_frame(f)) break;
            const double ms = (now_seconds() - t) * 1000.0;
            decode_ms += ms;
            worst = std::max(worst, ms);
            const std::uint32_t c = movie::frame_crc32(f);
            const std::uint8_t b[4]{std::uint8_t(c), std::uint8_t(c >> 8), std::uint8_t(c >> 16), std::uint8_t(c >> 24)};
            crc = movie::crc32(b, crc);
        }
        const bool ok = n == intro_head_pictures && crc == intro_head_crc;
        log("movie check (132vt2a2): INTRO's first %d pictures %s (CRC %08x, the reference decode's %08x); decode %.1f ms "
            "a picture mean (%.1f max, the slices over the movie pool, beside the front end's load); %.2f s",
            n, ok ? "match" : "DIFFER", crc, intro_head_crc, n ? decode_ms / n : 0.0, worst, now_seconds() - start);
    } catch (const std::exception& e) {
        log("movie check (132vt2a2): INTRO decode failed: %s", e.what());
    }
}

// 132vt2 (research/VITA.md "Movies"): the play host's front end (frontend_app.cpp) gets the same decode-ahead as the
// app's own front end, through frontend_disc.hpp's hooks; `on` false removes them (after the play host ends).
void install_play_movie_hooks(bool on) {
    auto& hooks = vita_movie_hooks();
    if (!on) {
        AheadMovie::stop_active();
        AheadMovie::set_colour_on_render_core(false);
        hooks = {};
        return;
    }
    AheadMovie::set_colour_on_render_core(true);
    // 132vt2b2: the play host's front end mixes on the sound pump as the app's own front end does (it mixed 800 frames
    // in each 60 Hz step on the session thread, which starved the output whenever a step ran late), and the pump is
    // the movies' sink and clock (without it their pictures were paced by the wall clock and the PCM of a late
    // picture was dropped).
    class PlayHostPump final : public VitaMovieHooks::SoundPumpHandle {
    public:
        PlayHostPump(SoundIopHost& host, std::recursive_mutex& lock, audio::Device& device)
            : pump_(host, lock, device, 4096), start_(now_seconds()) {
            AheadMovie::set_audio_sink(&pump_);
            log("play host front end: the sound pump mixes on the audio core (132vt2b2)");
        }
        ~PlayHostPump() override {
            AheadMovie::stop_active();
            AheadMovie::set_audio_sink(nullptr);
            log("play host front end: the sound pump stops after %.1f s: %.1f s mixed (%.1f s busy); movie PCM %.1f s "
                "pushed, %.1f s fed, %.1f s dropped at movie ends",
                now_seconds() - start_, double(pump_.mixed()) / audio::Mixer::output_rate, pump_.busy_seconds(),
                double(pump_.movie_pushed()) / audio::Mixer::output_rate,
                double(pump_.movie_fed()) / audio::Mixer::output_rate,
                double(pump_.movie_dropped()) / audio::Mixer::output_rate);
        }
        void set_movie(bool on) override { pump_.set_movie(on); }

    private:
        SoundPump pump_;
        const double start_;
    };
    hooks.locked_host = [](SoundIopHost& host, std::recursive_mutex& lock) -> std::unique_ptr<GameSoundHost> {
        return std::make_unique<LockedSoundHost>(host, lock);
    };
    hooks.sound_pump = [](SoundIopHost& host, std::recursive_mutex& lock,
                          audio::Device& device) -> std::unique_ptr<VitaMovieHooks::SoundPumpHandle> {
        return std::make_unique<PlayHostPump>(host, lock, device);
    };
    hooks.wrap = [](const std::function<std::unique_ptr<movie::MovieStream>(const std::string&)>& open,
                    const std::string& name) -> std::unique_ptr<movie::MovieStream> {
        AheadMovie::stop_active();   // the disc's movies share one stream: the last movie's reads stop first
        auto inner = open ? open(name) : nullptr;
        if (!inner) return nullptr;
        return std::make_unique<AheadMovie>(std::move(inner), name);
    };
    hooks.take_rgba = [](std::vector<std::uint8_t>& out) {
        AheadMovie* ahead = AheadMovie::active();
        return ahead && ahead->take_rgba(out);
    };
}

// 132vt2a: the subtitle projection's one-time setup (~1.5 s on the handheld) on a low-priority thread beside a load,
// not in the first subtitled movie frame; the first append_subtitles waits for it if it is still running.
std::thread start_subtitle_setup() {
    return std::thread([] {
        // The render core below every other thread there (the movie threads take cores 2 and 0).
        enter_thread_role(ThreadRole::render, "subtitle setup");
        sceKernelChangeThreadPriority(sceKernelGetThreadId(), 170);
        const double t = now_seconds();
        try {
            front_end_subtitle_projection();
            log("front end: subtitle projection set up in %.2f s (beside the load, 132vt2a)", now_seconds() - t);
        } catch (const std::exception& e) {
            log("front end: subtitle projection setup failed: %s", e.what());
        }
    });
}

// Port-only (research/VITA.md "Widescreen", 132ws): the frame aspect. The Vita defaults to wide (the 960 x 544
// screen is 16:9); ux0:data/omega_strain/port_settings.txt, when present, is read as the PC's port settings file and
// its aspect= / hud_margin= / movie_fill= / movies= / fov= apply (aspect= wins over the default). A malformed file: logged, defaults kept.
void apply_display_settings() {
    const std::string path = std::string(vita_data_root) + "/port_settings.txt";
    DisplaySettings display;
    bool has_aspect = false;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::string text;
        char chunk[1024];
        for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) text.append(chunk, n);
        std::fclose(f);
        try {
            const PortSettings settings = parse_port_settings(text);
            display = display_settings(settings);
            for (const auto& [key, value] : settings.other) has_aspect = has_aspect || key == "aspect";
        } catch (const std::exception& e) {
            display = DisplaySettings{};
            has_aspect = false;
            log("display settings: %s: %s; the defaults are used", path.c_str(), e.what());
        }
    }
    frame_aspect_setting() = has_aspect ? display.aspect : FrameAspect::wide;
    frame_hud_margin_setting() = display.hud_margin;
    frame_movie_zoom_setting() = display.movie_zoom;
    frame_movie_fit_setting() = display.movie_fit;   // 132vt2a2
    frame_fov_setting() = display.fov;   // 132ws3
    log("display (port-only): aspect %s%s, HUD margin %u %%, movies %s, loading screens %s, field of view %u",
        frame_aspect_name(frame_aspect_setting()), has_aspect ? " (port_settings.txt)" : " (the Vita default)",
        frame_hud_margin_setting(), frame_movie_fit_name(frame_movie_fit_setting()),
        frame_movie_zoom_setting() ? "zoom" : "bars", frame_fov_setting());
}

// Port-only test input (research/VITA.md "Front end"): ux0:data/omega_strain/vita_test_pads.txt, lines "FRAME MASK"
// (decimal front-end frame, hexadecimal 0055a44c held mask): from that frame the mask is held with the handheld's own
// buttons (the evidence runs' cursor moves on Vita3K, which has no scripted input). No file: nothing added.
struct PadScript {
    std::vector<std::pair<std::int64_t, std::uint16_t>> steps;
    std::size_t next = 0;
    std::uint16_t held = 0;
    PadScript() {
        const std::string path = std::string(vita_data_root) + "/vita_test_pads.txt";
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return;
        long long frame = 0;
        unsigned mask = 0;
        while (std::fscanf(f, "%lld %x", &frame, &mask) == 2) steps.emplace_back(frame, std::uint16_t(mask));
        std::fclose(f);
        log("front end: %u scripted pad steps from %s (port-only test)", unsigned(steps.size()), path.c_str());
    }
    std::uint16_t at(std::int64_t frame) {
        for (; next < steps.size() && steps[next].first <= frame; ++next) {
            held = steps[next].second;
            log("front end: scripted pad at frame %lld: held %04x (port-only test)", static_cast<long long>(frame), held);
        }
        return held;
    }
};

// The front end (research/VITA.md "Front end"): FrontendSession over FrontendDisc (the session, services and disc reads
// the window frontend_app.cpp uses), gameSound over the play sound host (checkpoint 126c: the FRONTEND banks and the
// MUSIC.HOG streams from the disc, mixed at 48 kHz, 800 frames per 60 Hz step) into the SceAudioOut device, the boot
// movie through movie_list / movie_texture, and the canvas through GuiDrawer / GuiRenderer. Returns the level of the
// session's hand-off (state 0xe), empty when it ends without one.
std::string App::front_end(DiscImage& disc, const std::string& iso, int frames) {
    progress("LOADING THE FRONT END", -1.0);
    const double load_start = now_seconds();
    FrontendDisc data(disc, iso);
    // Port-only (132vt2a, research/VITA.md "Movies"): the subtitle projection's one-time setup (~1.5 s here) beside the
    // front end's load and menus, not in the first subtitled movie frame (INTRO's: a 1.5 s stall). The first
    // append_subtitles waits for it if it is still running (a function-local static).
    struct Joined {
        std::thread thread;
        ~Joined() {
            if (thread.joinable()) thread.join();
        }
    } subtitle_setup{start_subtitle_setup()};
    audio::Mixer mixer;
    SoundIopHostConfig sound_config;
    sound_config.level = "TEST";  // 004ffc90's boot value; the session swaps in "FRONTEND" (frontend_app.cpp)
    SoundIopHost sound_host(disc, sound_config, mixer);
    std::recursive_mutex sound_lock;  // SoundPump: the host between gameSound and the mix thread
    LockedSoundHost locked_host(sound_host, sound_lock);
    GameSoundManager game_sound(locked_host);
    sound_host.attach(game_sound);
    const std::string card = std::string(vita_data_root) + "/card";
    sceIoMkdir(card.c_str(), 0777);
    FrontendServices services = data.services(card);
    services.sound = &game_sound;
    // Port-only test switch (research/VITA.md "Vita3K"): with ux0:data/omega_strain/vita_test_no_movies.txt the
    // movies are opened as absent (the session's own "not in ZMOVIES.HOG" path), for Vita3K builds that abort
    // when a movie starts. No file: the movies play.
    if (vita_file_size(std::string(vita_data_root) + "/vita_test_no_movies.txt") >= 0) {
        log("front end: vita_test_no_movies.txt present: the movies are skipped (test switch)");
        services.open_movie = [](const std::string&) { return std::unique_ptr<movie::PssMovie>(); };
    }
    intro_check(services.open_movie);
    // Port-only (132vt2a, research/VITA.md "Movies"): every movie is decoded ahead of the session (AheadMovie).
    services.open_movie = [open = std::move(services.open_movie)](const std::string& name) -> std::unique_ptr<movie::MovieStream> {
        AheadMovie::stop_active();
        auto inner = open ? open(name) : nullptr;
        if (!inner) return nullptr;
        return std::make_unique<AheadMovie>(std::move(inner), name);
    };
    // Port-only test switch (research/VITA.md "Vita3K"): ux0:data/omega_strain/vita_test_captures.txt lists more
    // front-end frames to capture (decimal, whitespace-separated), for Vita3K runs that stop before frame 300.
    std::set<std::int64_t> capture_frames{300, 1500, 2400};
    if (FILE* f = std::fopen((std::string(vita_data_root) + "/vita_test_captures.txt").c_str(), "r")) {
        for (long long n = 0; std::fscanf(f, "%lld", &n) == 1;) capture_frames.insert(n);
        std::fclose(f);
        log("front end: %u capture frames (vita_test_captures.txt, test switch)", unsigned(capture_frames.size()));
    }
    services.sound_level = [&sound_host, &sound_lock](const std::string& level) {
        std::lock_guard<std::recursive_mutex> g(sound_lock);
        sound_host.set_level(level);
    };
    FrontendSession session(std::move(services));
    session.set_pad_state(6);  // the handheld's own controls: always connected (pad_connection.hpp's ready state)
    sound_host.schedule = [&session](std::int32_t delay, std::function<void()> event) {
        session.schedule(delay, std::move(event));
    };
    sound_host.hud = [] { return false; };
    GuiRenderer renderer(display_.device);
    renderer.set_frame_fit(true);
    std::set<std::uint32_t> uploaded, zeus_uploaded;
    for (const auto& [slot, image] : data.fonts().images)
        if (uploaded.insert(slot).second) renderer.set_texture(slot, image);
    GuiDrawList shown;
    const auto sync_images = [&] {
        OMEGA_ZONE("sync_images");
        for (const auto& [slot, image] : data.assets().images())
            if (uploaded.insert(slot).second) renderer.set_texture(slot, image);
        // frontend_app.cpp sync_zeus: the ZEUS / MANTEX / figure pictures held now.
        const auto& images = data.zeus_images();
        std::set<std::uint32_t> drawn;
        for (const auto& b : shown.batches) drawn.insert(b.slot);
        for (auto it = zeus_uploaded.begin(); it != zeus_uploaded.end();) {
            if (images.count(*it) || drawn.count(*it)) {
                ++it;
                continue;
            }
            renderer.drop_texture(*it);
            it = zeus_uploaded.erase(it);
        }
        for (const auto& [slot, image] : images)
            if (zeus_uploaded.insert(slot).second) renderer.set_texture(slot, image);
    };
    sync_images();
    GuiDrawer drawer([&data](std::uint32_t h) { return data.fonts().font(h); });
    IeDrawState ie_state;
    drawer.set_decor(&session.decor(), &ie_state);
    std::string figure_note;
    const auto figure_log = [&](const std::string& note) {
        if (note == figure_note) return;
        figure_note = note;
        log("front end: frame %lld agent model: %s", static_cast<long long>(session.frames()), note.c_str());
    };
    // frontend_app.cpp's display callbacks: GuiAgentCreate 0028d870, GuiAgentModify 00341f00, GuiWeaponDisplay
    // 00440580 and GuiAgentDisplay 00361e70.
    drawer.character_display_draw = [&](GuiDrawList& out, IeDrawState*, const gui::Widget& screen, const gui::Widget&) {
        OMEGA_ZONE("rec.figure");
        if (screen.cls == gui::WidgetClass::agentcreate && screen.ac)
            figure_log(data.agent_figure_draw(out, screen.ac->m.model().packing, screen.ac->m.camera,
                                              screen.ac->pose_time, 0x28d870));
        else if (screen.cls == gui::WidgetClass::agentmod && screen.am)
            figure_log(data.agent_figure_draw(out, screen.am->m.packing, screen.am->m.camera, screen.am->pose_time,
                                              0x341f00));
        else
            throw std::runtime_error("gui draw: unhosted GuiScoresEquip figure draw (0035b390)");
    };
    drawer.weapon_display_draw = [&](GuiDrawList& out, IeDrawState* st, const gui::Widget& w) {
        OMEGA_ZONE("rec.weapon");
        data.weapon_display_draw(out, st, w, static_cast<std::uint32_t>(session.frames()));
    };
    drawer.agent_display_draw = [&](GuiDrawList& out, IeDrawState*, const gui::Widget& w) {
        OMEGA_ZONE("rec.figure");
        if (!w.ad) throw std::runtime_error("gui draw: GuiAgentDisplay without its state (00361e70)");
        figure_log(data.agent_figure_draw(out, w.ad->packing, w.ad->camera, w.ad->pose_time, 0x361e70, w.ad->turns));
    };
    // 64 ms primes the output; the pump keeps 4096 frames (85 ms) queued (research/VITA.md "Audio", 132vt1b).
    // Port-only test switch (research/VITA.md "Vita3K"): with ux0:data/omega_strain/vita_test_no_audio.txt the front
    // end's output device starts absent (audio::Device's silent path; the mix still runs). No file: SceAudioOut.
    const bool no_audio = vita_file_size(std::string(vita_data_root) + "/vita_test_no_audio.txt") >= 0;
    if (no_audio) log("front end: vita_test_no_audio.txt present: no audio output port (test switch)");
    // Port-only test switch (research/VITA.md "Widescreen", 132vt1b's hardware bisect): with
    // ux0:data/omega_strain/vita_test_no_wide_fill.txt the wide aspect's side areas stay black (no side fill).
    const bool no_wide_fill = vita_file_size(std::string(vita_data_root) + "/vita_test_no_wide_fill.txt") >= 0;
    if (no_wide_fill) log("front end: vita_test_no_wide_fill.txt present: no wide side fill (test switch)");
    audio::Device sound(0.064, false, no_audio);
    SoundPump pump(sound_host, sound_lock, sound, 4096);
    // 132vt2a2: the movies' PCM goes from their decode thread to the pump; before the pump goes, the movie stops.
    AheadMovie::set_audio_sink(&pump);
    struct SinkReset {
        ~SinkReset() {
            AheadMovie::stop_active();
            AheadMovie::set_audio_sink(nullptr);
        }
    } sink_reset;
    log("front end: loaded in %.2f s (%u font images, %u decorator images; disc reads %llu, %.1f MB)",
        now_seconds() - load_start, unsigned(data.fonts().images.size()), unsigned(data.assets().images().size()),
        static_cast<unsigned long long>(disc.read_stats().reads), disc.read_stats().bytes / 1048576.0);
    log_memory("front end loaded");
    PadScript script;
    FrameParts parts;
    std::uint32_t last_render_mode = 0;
    std::uint64_t movie_serial = 0;
    Texture movie_picture;
    unsigned movie_pictures = 0;
    double movie_seconds = 0.0;
    int peak = 0;
    std::uint64_t second_frames = 0, nonzero_seconds = 0;
    std::string view;
    std::string level;
    for (int frame = 0; frame < frames && !quit_; ++frame) {
        const double started = now_seconds();
        parts.begin();
        poll();
        if (pad_.pressed(ds2::select) && !(pad_.held & (ds2::start | ds2::r1))) {
            log("front end: SELECT at frame %lld: on to the TORONTO1 loading screen (VITA.md \"Controls\")",
                static_cast<long long>(session.frames()));
            break;
        }
        const std::uint16_t held = pad_.held | script.at(session.frames() + 1);
        try {
            const double t = now_seconds();
            parts.lap(0);
            session.frame(held);
            parts.lap(1);
            if (session.movie_playing()) movie_seconds += now_seconds() - t;
        } catch (const std::exception& e) {
            log("front end: stopped at frame %lld: %s", static_cast<long long>(session.frames()), e.what());
            front_end_error_ = e.what();
            break;
        }
        for (const auto& line : session.take_log()) log("front end: recorded %s", line.c_str());
        pump.set_movie(session.movie_playing());
        std::vector<std::string> sound_lines;
        {
            std::lock_guard<std::recursive_mutex> g(sound_lock);
            sound_lines = sound_host.take_log();
        }
        for (const auto& line : sound_lines)
            log("front end sound at frame %lld: %s", static_cast<long long>(session.frames()), line.c_str());
        parts.lap(2);
        sync_images();
        parts.lap(3);
        if (session.movie_playing() && session.movie_frame()) {
            if (session.movie_serial() != movie_serial) {
                movie_serial = session.movie_serial();
                // One texture rewritten in place and one conversion buffer (research/VITA.md "Movies").
                const auto& picture = *session.movie_frame();
                movie_picture.width = static_cast<std::uint32_t>(picture.width);
                movie_picture.height = static_cast<std::uint32_t>(picture.height);
                AheadMovie* ahead = AheadMovie::active();
                if (!ahead || !ahead->take_rgba(movie_picture.rgba)) movie::frame_to_rgba(picture, movie_picture.rgba);
                renderer.stream_texture(movie_slot, movie_picture);
                ++movie_pictures;
            }
            parts.lap(4);
            shown = movie_list(session.movie_aspect());
            if (const auto* track = session.movie_subtitles()) {
                OMEGA_ZONE("vita.subtitles");
                append_subtitles(shown, data.fonts(), *track, session.movie_subtitle_colour(), last_render_mode);
            }
        } else if (session.gui_drawn()) {
            shown.recycle();   // 132vf1: keeps the batches' storage for this frame's draw
            drawer.draw_canvas(shown, session.runtime(), session.now_ms());
            if (!shown.batches.empty()) last_render_mode = shown.batches.back().render_mode;
            sync_images();
        } else if (session.movie_playing() || !session.gui_active()) {
            shown.batches.clear();
        }
        parts.lap(5);
        // As the window: a movie's PCM instead of the sound mix (gameSound is not loaded while a movie plays); the
        // sound mix itself goes to the device from the pump.
        const auto movie_pcm = session.take_movie_audio();
        if (!movie_pcm.empty() && session.movie_rate() == 48000 && session.movie_channels() == 2) {
            for (const std::int16_t v : movie_pcm) peak = std::max(peak, std::abs(int(v)));
            pump.push_movie(movie_pcm);   // 132vt2a: queued, never waits (SoundPump)
        }
        peak = std::max(peak, pump.take_peak());
        for (const auto& line : sound.take_log()) log("audio: %s", line.c_str());
        parts.lap(6);
        std::string now;
        if (session.movie_playing()) now = "movie " + session.movie_name();
        else if (const gui::Widget* top = session.runtime().top(); top && session.gui_active())
            now = "screen class " + std::to_string(int(top->cls));
        else
            now = "state " + std::to_string(int(session.state()));
        if (now != view) {
            view = now;
            log("front end: frame %lld: %s", static_cast<long long>(session.frames()), view.c_str());
        }
        parts.lap(9);
        auto back = begin_frame();
        // Port-only (132ws, research/VITA.md "Widescreen"): as frontend_app.cpp's present: under the wide aspect a
        // mod's wide background ("frontend.<screen>") under the 4:3 menu, else the blurred side fill after it.
        bool wide_background = false;
        if (!session.movie_playing() && frame_wide(960, 544))
            if (const gui::Widget* top = session.runtime().top(); top && session.gui_active())
                wide_background = renderer.draw_wide_background(std::string("frontend.") + frontend_class_name(top->cls),
                                                                back.get(), 960, 544);
        // 132ws5 (research/FRONTEND.md "Checkpoint 132ws5"): the front end's wide menus (GuiRenderer::set_wide_menu):
        // the 4:3 content unclipped, the art the frame's edges cut extended to the screen's edges, the header and
        // legend spread; the side fill only for a frame with nothing to extend and for the GuiSlideShow (photographs).
        const gui::Widget* shown_top = session.gui_active() ? session.runtime().top() : nullptr;
        const bool slideshow = shown_top && shown_top->cls == gui::WidgetClass::slideshow;
        const bool edge_extend = renderer.wide_edge_extend();
        renderer.set_wide_edge_extend(!wide_background && !session.movie_playing() && !slideshow);
        renderer.set_wide_menu(!slideshow);
        renderer.draw(shown, back.get(), 960, 544);
        renderer.set_wide_menu(false);
        renderer.set_wide_edge_extend(edge_extend);
        if (!wide_background && !session.movie_playing() && !shown.batches.empty() && !no_wide_fill &&
            renderer.wide_edge_images() == 0) {
            OMEGA_ZONE("vita.wide_fill");
            renderer.draw_wide_fill(back.get(), 960, 544);
        }
        parts.lap(7);
        if (capture_frames.count(frame)) capture(back.get(), "capture_frontend_f" + std::to_string(frame));
        end_frame(started);
        parts.lap(8);
        parts.end([](const char* text) { log("%s", text); }, disc.read_stats().reads);
        if (!parts.overlay.empty()) overlay_detail_ = parts.overlay;
        if (++second_frames == 60) {
            const auto st = sound.stats();
            if (peak) ++nonzero_seconds;
            log("audio: second to frame %lld: peak %d of the submitted 48 kHz PCM (%s); submitted %llu frames, queued "
                "%u, underruns %u; sound mix thread %llu frames, %.3f s busy; movie PCM pushed %llu, fed %llu, waiting "
                "%u, dropped %llu",
                static_cast<long long>(session.frames()), peak, session.movie_playing() ? "movie" : "sound mix",
                static_cast<unsigned long long>(st.submitted_frames), unsigned(st.queued_frames), unsigned(st.underruns),
                static_cast<unsigned long long>(pump.mixed()), pump.busy_seconds(),
                static_cast<unsigned long long>(pump.movie_pushed()), static_cast<unsigned long long>(pump.movie_fed()),
                unsigned(pump.movie_waiting()), static_cast<unsigned long long>(pump.movie_dropped()));
            peak = 0;
            second_frames = 0;
        }
        if (session.handoff()) {
            level = session.level();
            log("front end: hand-off (state 0xe) to %s at frame %lld", level.c_str(), static_cast<long long>(session.frames()));
            break;
        }
    }
    if (movie_pictures)
        log("front end: %u movie pictures; the session's frames while a movie played took %.3f s (%.2f ms per frame)",
            movie_pictures, movie_seconds, 1000.0 * movie_seconds / std::max(1U, movie_pictures * 2));
    log("front end: %llu seconds with sound out of the reported ones", static_cast<unsigned long long>(nonzero_seconds));
    report("front end");
    return level;
}

void App::loading_screen(DiscImage& disc, const std::string& level, int frames) {
    progress("LOADING " + level, -1.0);
    const double load_start = now_seconds();
    auto assets = build_loading_screen(disc, level);
    GuiRenderer renderer(display_.device);
    renderer.set_frame_fit(true);
    for (const auto& [slot, image] : assets->images()) renderer.set_texture(slot, *image);
    log("loading screen %s: built in %.2f s", level.c_str(), now_seconds() - load_start);
    log_memory("loading screen built");
    IeDrawState state;
    std::int64_t vblanks = 0;
    GuiDrawList list;
    for (int frame = 0; frame < frames && !quit_; ++frame) {
        const double started = now_seconds();
        poll();
        // The loading thread wakes every third NTSC vblank (hud_loading.hpp, omega_frontend_formats --loading-capture).
        for (bool wake = false; !wake;) {
            wake = loading_vblank_1667c0(assets->globals);
            ++vblanks;
        }
        list = loading_frame(*assets, state, static_cast<std::uint64_t>(vblanks * 100000 * 1001 / 60000));
        // Port-only (132ws8): under wide the loading screen is drawn as the front end's wide menus, as
        // loading_screen_host.cpp (its art extended to the screen's edges; a mod's "loading.<level>" background
        // replaces that); movie_fill=zoom keeps the one 4:3 picture scaled to the width.
        const bool wide_menu = frame_wide(960, 544) && !frame_movie_zoom_setting();
        if (!wide_menu)
            for (GuiDrawBatch& b : list.batches) b.port_picture = true;
        auto back = begin_frame();
        const bool wide_background = wide_menu && renderer.draw_wide_background("loading." + level, back.get(), 960, 544);
        renderer.set_wide_edge_extend(wide_menu && !wide_background);
        renderer.set_wide_menu(wide_menu);
        renderer.draw(list, back.get(), 960, 544);
        renderer.set_wide_menu(false);
        renderer.set_wide_edge_extend(false);
        if (wide_menu && !wide_background && !list.batches.empty() && renderer.wide_edge_images() == 0)
            renderer.draw_wide_fill(back.get(), 960, 544);
        if (frame == 60 || frame == 400) capture(back.get(), "capture_loading_" + level + "_f" + std::to_string(frame));
        end_frame(started);
    }
    log("loading screen %s: %lld vblanks, %u batches, %u texts", level.c_str(), static_cast<long long>(vblanks),
        unsigned(list.batches.size()), unsigned(assets->screen.screen.drawn_texts().size()));
    report("loading screen");
    loading_screen_destroy(assets->screen);
}

// The three user cores at work: the same pure-integer jobs serially on the simulation thread, then through the worker
// pool (the caller on core 0 and the workers on core 2) and on the render host thread (core 1) at once.
void App::core_check() {
    constexpr std::size_t jobs = 6;
    std::vector<std::uint32_t> out(jobs);
    const auto job = [&](std::size_t i) {
        std::uint32_t x = static_cast<std::uint32_t>(i);
        for (int k = 0; k < 4000000; ++k) x = x * 1664525U + 1013904223U + (x >> 13);
        out[i] = x;
    };
    double t = now_seconds();
    for (std::size_t i = 0; i < jobs; ++i) job(i);
    const double serial = now_seconds() - t;
    const auto expected = out;
    std::fill(out.begin(), out.end(), 0U);
    t = now_seconds();
    workers_.for_each(jobs, job);
    const double pooled = now_seconds() - t;
    const bool same = out == expected;
    // The render host thread runs the render thread's loop; the loading-screen host thread is the other core 1 thread.
    std::fill(out.begin(), out.end(), 0U);
    t = now_seconds();
    auto done = HostThread::loading_screen().run([&] {
        ThreadBusyScope busy;
        job(0);
        job(1);
    });
    workers_.for_each(jobs - 2, [&](std::size_t i) { job(i + 2); });
    done->wait();
    const double all = now_seconds() - t;
    log("cores: %u workers; %u jobs serial %.3f s, pool (core 0 + core 2) %.3f s = %.2fx, pool + render host (cores "
        "0, 1, 2) %.3f s = %.2fx; results %s",
        workers_.workers(), unsigned(jobs), serial, pooled, serial / pooled, all, serial / all,
        same && out == expected ? "identical" : "DIFFER");
}

void App::run() {
    log("display: %s", display_.backend.c_str());
    log_memory("display created");
    for (std::size_t p = 0; p < gfx::gxm::gxm_program_count; ++p) {
        progress("PREPARING THE SHADERS " + std::to_string(p + 1) + " / " + std::to_string(gfx::gxm::gxm_program_count) +
                     " (THE FIRST START COMPILES THEM)",
                 double(p) / gfx::gxm::gxm_program_count);
        const double start = now_seconds();
        try {
            display_.device->prepare(static_cast<gfx::Program>(p));
            log("program %u: ready in %.3f s", unsigned(p), now_seconds() - start);
        } catch (const gfx::gxm::ShaderCompilerMissing&) {
            throw;
        } catch (const std::exception& e) {
            log("program %u: FAILED: %s", unsigned(p), e.what());
        }
    }
    progress("SELF TESTS", -1.0);
    log("self test on the main thread:");
    self_test(*inner_.device);
    log("self test through the render thread:");
    self_test(*display_.device);
    if (vita_file_size(std::string(vita_data_root) + "/vita_test_display_copy.txt") >= 0)
        display_copy_test(*display_.device, begin_frame().get());
    audio_test();
    core_check();
    // 132vt2a2 (research/VITA.md "Movies"): the movie decoder's slices over the movie pool.
    mpeg2::Decoder::set_slice_runner([](int count, const std::function<void(int)>& job) { MoviePool::get().run(count, job); });
    movie_check();
    threads_.report("boot: shaders, self tests, core check");
    log_memory("after the self tests");

    apply_display_settings();
    const std::string iso = std::string(vita_data_root) + "/omega_strain.iso";
    const std::int64_t size = vita_file_size(iso);
    if (size < 0) {
        log("no disc image at %s: the pad test runs (VITA.md \"Game data\")", iso.c_str());
        no_disc(60 * 60);
        return;
    }
    // The full SHA-256 is an option (iso_verify_vita.hpp): ux0:data/omega_strain/verify_full.txt, or TRIANGLE held now.
    const std::string full_request = std::string(vita_data_root) + "/verify_full.txt";
    pad_.read();
    const bool full = vita_file_size(full_request) >= 0 || (pad_.held & ds2::triangle);
    log("disc image %s: %lld bytes; checking (%s)", iso.c_str(), static_cast<long long>(size),
        full ? "full SHA-256 requested" : "quick");
    progress(full ? "VERIFYING THE DISC IMAGE (FULL SHA-256)" : "CHECKING THE DISC IMAGE", full ? 0.0 : -1.0);
    const double verify_start = now_seconds();
    std::string how;
    IsoStream stream = open_checked_iso(
        iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256, OMEGA_ISO_QUICK_SHA256, full,
        [&](std::uint64_t done, std::uint64_t total) {
            if (!full || now_seconds() - progress_drawn_ < 0.25) return;
            char text[120];
            std::snprintf(text, sizeof text, "VERIFYING THE DISC IMAGE (FULL SHA-256): %llu / %llu MB",
                          static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(total >> 20));
            progress(text, double(done) / double(total));
        },
        how);
    if (full && how == "full") sceIoRemove(full_request.c_str());
    log("disc image checked in %.1f s: %s", now_seconds() - verify_start, how.c_str());
    DiscImage disc(stream, OMEGA_ISO_SIZE);
    // The default: the play host (front end, levels, pause, level end). ux0:data/omega_strain/bringup.txt runs vt1's
    // bring-up loop below instead (the app's own front end and loading screen, no level).
    if (vita_file_size(std::string(vita_data_root) + "/bringup.txt") < 0) {
        const int code = play(disc, iso);
        const std::string message = !play_error_.empty() ? play_error_ : crash::vita_last_message();
        if (code != 0 || !message.empty()) {
            // The play host's display is gone; the app's own frames go on over the same device.
            notice("THE GAME STOPPED (EXIT " + std::to_string(code) + ")",
                   (message.empty() ? std::string("See ux0:data/omega_strain/omega_vita.log.") : message) +
                       "\nThe log is ux0:data/omega_strain/omega_vita.log.");
        }
        log_memory("end");
        return;
    }
    // The front end, the hand-off's loading screen, then (until the level loop is ported: research/VITA.md "Handoff")
    // a notice and a fresh front end; any stop is shown on screen and logged, never a silent exit.
    while (!quit_) {
        front_end_error_.clear();
        std::string level;
        try {
            level = front_end(disc, iso, 60 * 60 * 60);
        } catch (const std::exception& e) {
            front_end_error_ = e.what();
        }
        log_memory("front end done");
        if (quit_) break;
        if (!front_end_error_.empty()) {
            notice("THE FRONT END STOPPED", front_end_error_);
            continue;
        }
        if (level.empty()) level = "TORONTO1";
        try {
            loading_screen(disc, level, 60 * 3);
        } catch (const std::exception& e) {
            notice("THE LOADING SCREEN STOPPED", e.what());
            continue;
        }
        if (quit_) break;
        notice(level + ": IN-LEVEL PLAY IS NOT ON THE VITA YET",
               "The front end handed off to " + level + " and its loading screen ran. The level setup and the play "
               "loop are still the Windows host's (src/port/app/play_main.cpp: the level loop and its objects). "
               "Porting them to the Vita is the next stage (132vt2: research/VITA.md Handoff).");
    }
    log_memory("end");
}

}  // namespace

// Port-only diagnostic (132vt2, research/VITA.md "Memory"): every C++ allocation carries an 8-byte header (its caller's
// slot and its size), so the bytes each caller holds are known at any time. heap_profile_report logs the callers
// holding the most (the .vpk's eboot loads 0x49000 above its link base, 132vt2b2: an ELF address is the logged one
// less 0x49000, less 1 for a Thumb return address; arm-vita-eabi-addr2line -f -C names it):
// each time the heap in use passes a new 16 MB step, on a failed allocation, and when asked (log_memory).
namespace heap_profile {
// 132vt2d: lock-free (32-bit atomics; a slot's caller is claimed once by compare-and-swap). The 132vt2b2 spin-then-sleep
// lock was taken twice per allocation by the simulation, render and worker threads alike, and a contended one slept.
struct Slot {
    std::uintptr_t caller;
    std::int32_t live, peak;
    std::uint32_t count;
};
struct Header {
    std::uint32_t slot, size;
};
constexpr std::uint32_t slot_count = 8192;
Slot table[slot_count];
std::int32_t in_use = 0, reported = 0;
std::atomic<bool> enabled{false};   // the reports (the accounting is always on)

std::uint32_t record(std::uintptr_t caller, std::uint32_t size) {
    const auto bytes = std::int32_t(size);
    std::uint32_t i = std::uint32_t((caller >> 1) * 2654435761u) % slot_count;
    for (std::uint32_t n = 0; n < slot_count; ++n, i = (i + 1) % slot_count) {
        Slot& s = table[i];
        std::uintptr_t owner = __atomic_load_n(&s.caller, __ATOMIC_ACQUIRE);
        if (owner == 0) {
            std::uintptr_t expected = 0;
            owner = __atomic_compare_exchange_n(&s.caller, &expected, caller, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
                        ? caller
                        : expected;
        }
        if (owner != caller) continue;
        const std::int32_t live = __atomic_add_fetch(&s.live, bytes, __ATOMIC_RELAXED);
        std::int32_t peak = __atomic_load_n(&s.peak, __ATOMIC_RELAXED);
        while (live > peak && !__atomic_compare_exchange_n(&s.peak, &peak, live, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
        __atomic_add_fetch(&s.count, 1u, __ATOMIC_RELAXED);
        __atomic_add_fetch(&in_use, bytes, __ATOMIC_RELAXED);
        return i;
    }
    __atomic_add_fetch(&in_use, bytes, __ATOMIC_RELAXED);
    return slot_count;   // the table is full: counted in the total only
}
void release(std::uint32_t slot, std::uint32_t size) {
    if (slot < slot_count) {
        __atomic_sub_fetch(&table[slot].live, std::int32_t(size), __ATOMIC_RELAXED);
        __atomic_sub_fetch(&table[slot].count, 1u, __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&in_use, std::int32_t(size), __ATOMIC_RELAXED);
}
void report(const char* when, int top = 24) {
    // One report at a time (the rows are static): a second thread's report is skipped.
    static std::atomic_flag reporting = ATOMIC_FLAG_INIT;
    if (reporting.test_and_set(std::memory_order_acquire)) return;
    struct Reporting {
        ~Reporting() { reporting.clear(std::memory_order_release); }
    } reporting_done;
    struct Row {
        std::uintptr_t caller;
        std::int64_t live, peak;
        std::uint32_t count;
    };
    static Row rows[slot_count];
    std::uint32_t n = 0;
    for (Slot& s : table) {
        const std::uintptr_t caller = __atomic_load_n(&s.caller, __ATOMIC_ACQUIRE);
        if (caller)
            rows[n++] = {caller, __atomic_load_n(&s.live, __ATOMIC_RELAXED), __atomic_load_n(&s.peak, __ATOMIC_RELAXED),
                         __atomic_load_n(&s.count, __ATOMIC_RELAXED)};
    }
    const std::int64_t total = __atomic_load_n(&in_use, __ATOMIC_RELAXED);
    std::sort(rows, rows + n, [](const Row& a, const Row& b) { return a.live > b.live; });
    log("heap profile (%s): %.1f MB in C++ allocations by %u callers; the top %d by bytes held:", when, total / 1048576.0, n,
        top);
    for (int i = 0; i < top && std::uint32_t(i) < n; ++i)
        log("  %p: %.2f MB in %u blocks (peak %.2f MB)", reinterpret_cast<void*>(rows[i].caller), rows[i].live / 1048576.0,
            rows[i].count, rows[i].peak / 1048576.0);
}
}  // namespace heap_profile

[[gnu::noinline]] void* operator new(std::size_t size) {
    __atomic_fetch_add(&g_mallocs, 1, __ATOMIC_RELAXED);
    ++t_mallocs;
    // 132vc2 (port-only): blocks of up to 1 KB from the small-object allocator (small_alloc.hpp: per-thread lists, no
    // heap lock, no profile header); the rest, and every block once its chunks reach their limit, as before.
    if (void* small = omega::small_alloc::allocate(size)) return small;
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    void* raw = std::malloc(size + sizeof(heap_profile::Header));
    if (!raw || size >= (4u << 20)) {
        static thread_local bool inside = false;
        if (!inside) {
            inside = true;
            const struct mallinfo m = mallinfo();
            log("heap: %s %.2f MB (heap in use %.1f MB, arena %.1f MB), caller %p", raw ? "allocated" : "FAILED to allocate",
                size / 1048576.0, m.uordblks / 1048576.0, m.arena / 1048576.0, reinterpret_cast<void*>(caller));
            if (!raw) heap_profile::report("the failed allocation", 40);
            inside = false;
        }
    }
    if (!raw) throw std::bad_alloc();
    auto* header = static_cast<heap_profile::Header*>(raw);
    header->slot = heap_profile::record(caller, std::uint32_t(size));
    header->size = std::uint32_t(size);
    if (heap_profile::enabled.load(std::memory_order_relaxed) &&
        __atomic_load_n(&heap_profile::in_use, __ATOMIC_RELAXED) >= heap_profile::reported + (16 << 20)) {
        static thread_local bool inside = false;
        if (!inside) {
            inside = true;
            heap_profile::reported = __atomic_load_n(&heap_profile::in_use, __ATOMIC_RELAXED);
            char when[64];
            std::snprintf(when, sizeof when, "passed %d MB", int(heap_profile::reported >> 20));
            heap_profile::report(when, 12);
            inside = false;
        }
    }
    return header + 1;
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept {
    if (!p) return;
    if (omega::small_alloc::owns(p)) {  // 132vc2
        omega::small_alloc::release(p);
        return;
    }
    auto* header = static_cast<heap_profile::Header*>(p) - 1;
    heap_profile::release(header->slot, header->size);
    std::free(header);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

int main() {
    sceIoMkdir(vita_data_root, 0777);
    const std::string log_path = std::string(vita_data_root) + "/omega_vita.log";
    sceIoRemove(log_path.c_str());  // Vita3K's "w" open does not truncate an existing file
    g_log = std::fopen(log_path.c_str(), "w");
    g_log_path = log_path;
    // Port-only test switch (research/VITA.md "Vita3K"): vita_test_log_lines.txt reopens the log after every line, so
    // an emulator abort keeps the last line before it.
    if (vita_file_size(std::string(vita_data_root) + "/vita_test_log_lines.txt") >= 0) g_log_reopen_seconds = 0.0;
    g_log_writer.start();
    omega::zone::thread_allocations = [] { return t_mallocs; };
    // 132vc2 (port-only, research/VITA.md "Main-thread CPU (132vc2)"): the small-object allocator, up to 12 MB of
    // 64 KB chunks; vita_test_small_alloc_off.txt keeps every block on newlib's malloc (the A/B).
    const bool small_alloc_on = vita_file_size(std::string(vita_data_root) + "/vita_test_small_alloc_off.txt") < 0;
    if (small_alloc_on) omega::small_alloc::configure(std::size_t{12} << 20);
    // 132vc2: the particle tails on the pool (particle_tail.hpp); vita_test_particle_tail_off.txt draws each object's
    // particles in turn on this thread as before (the A/B).
    omega::g_particle_tail = vita_file_size(std::string(vita_data_root) + "/vita_test_particle_tail_off.txt") < 0;
    omega::crash::vita_before_exit = [] { g_log_writer.stop(); };
    log("Omega Strain native port, PS Vita (checkpoint 132vt1)");
    // 132vp1: the heap profile's caller addresses are runtime ones; log_memory's runtime address against its ELF
    // address (arm-vita-eabi-nm -C omega_vita | findstr log_memory) gives the load offset for addr2line.
    log("code: log_memory at %p", reinterpret_cast<const void*>(&log_memory));
    // research/VITA.md "Threads and cores": the clocks at their highest user settings, then this (the simulation)
    // thread on core 0.
    const int arm = scePowerSetArmClockFrequency(444), bus = scePowerSetBusClockFrequency(222),
              gpu = scePowerSetGpuClockFrequency(222), xbar = scePowerSetGpuXbarClockFrequency(166);
    log("clocks: set ARM 444 (0x%X), bus 222 (0x%X), GPU 222 (0x%X), GPU crossbar 166 (0x%X); now ARM %d MHz, bus %d "
        "MHz, GPU %d MHz, GPU crossbar %d MHz",
        unsigned(arm), unsigned(bus), unsigned(gpu), unsigned(xbar), scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    enter_thread_role(ThreadRole::simulation, "simulation");
#if defined(OMEGA_ZONE_PROFILE)
    {
        const auto cost = omega::zone::calibrate_clock();
        log("profiling: one clock read %.0f ns (std::chrono::steady_clock), %.0f ns (sceKernelGetProcessTimeWide); the "
            "zones use %s%s",
            cost.steady_ns, cost.process_ns, omega::zone::g_process_clock ? "the process clock" : "steady_clock",
#if defined(OMEGA_ZONE_PROFILE_FINE)
            ", with the GL backend's per-draw zones");
#else
            " (the GL backend's per-draw zones are compiled out: OMEGA_VITA_FINE_ZONES)");
#endif
    }
#endif
    try {
        gfx::gxm::Options options;
        options.log = [](const std::string& text) { log("%s", text.c_str()); };
        // Port-only test switch (research/VITA.md "Widescreen"): ux0:data/omega_strain/vita_test_display_readback.txt
        // brings back 132vt1's CPU read-back of the display for the wide side fill (the hardware bisect, 132vt1b).
        options.display_copy_readback =
            vita_file_size(std::string(vita_data_root) + "/vita_test_display_readback.txt") >= 0;
        if (options.display_copy_readback) log("display: vita_test_display_readback.txt present: copies read back");
        // Port-only test switch (VITA.md "Draw batching"): vita_test_gl_state_cache_off.txt sets every draw's GL state.
        options.gl_state_cache = vita_file_size(std::string(vita_data_root) + "/vita_test_gl_state_cache_off.txt") < 0;
        if (!options.gl_state_cache) log("display: vita_test_gl_state_cache_off.txt present: all GL state set every draw");
        // Port-only test switch (VITA.md "Depth across scenes"): vita_test_depth_keep_off.txt clears depth at every
        // GXM scene (vitaGL's default; the vt2d hardware's see-through walls).
        options.keep_depth = vita_file_size(std::string(vita_data_root) + "/vita_test_depth_keep_off.txt") < 0;
        // Port-only test switch (VITA.md "Shadow filter cache"): vita_test_shadow_cache_off.txt reads the shadow
        // receivers' buffers at every draw (the 132vt4 A/B).
        options.shadow_filter_cache = vita_file_size(std::string(vita_data_root) + "/vita_test_shadow_cache_off.txt") < 0;
        if (!options.shadow_filter_cache) log("display: vita_test_shadow_cache_off.txt present: shadow filter cache off");
        options.shadow_filter_check = vita_file_size(std::string(vita_data_root) + "/vita_test_shadow_cache_check.txt") >= 0;
        if (options.shadow_filter_check) log("display: vita_test_shadow_cache_check.txt present: shadow filter cache hits checked");
    // 132vg2 (VITA.md "Vita render path (132vg2)"): the GXM backend's per-scene GPU timing, on unless switched off.
    options.gpu_timing = vita_file_size(std::string(vita_data_root) + "/vita_test_gpu_timing_off.txt") < 0;
    if (!options.gpu_timing) log("display: vita_test_gpu_timing_off.txt present: GXM per-scene GPU timing off");
    options.linear_textures = vita_file_size(std::string(vita_data_root) + "/vita_test_linear_textures.txt") >= 0;
    options.patched_proofs = vita_file_size(std::string(vita_data_root) + "/vita_test_patched_proofs_off.txt") < 0;
    if (!options.patched_proofs) log("display: vita_test_patched_proofs_off.txt present: patched draws unproven");
    options.census = vita_file_size(std::string(vita_data_root) + "/vita_test_census.txt") >= 0;
    if (options.census) log("display: vita_test_census.txt present: the blend census is counted");
    options.blend_proof = vita_file_size(std::string(vita_data_root) + "/vita_test_blend_proof_off.txt") < 0;
    if (!options.blend_proof) log("display: vita_test_blend_proof_off.txt present: every scene blend kept");
    g_gpu_probe_cycle = vita_file_size(std::string(vita_data_root) + "/vita_test_gpu_probe.txt") >= 0;
    if (g_gpu_probe_cycle)
        log("display: vita_test_gpu_probe.txt present: the GPU probe cycles its modes per play window (diagnostic: the "
            "picture is wrong in modes 1-6)");
    options.shadow_vertex_flags = vita_file_size(std::string(vita_data_root) + "/vita_test_shadow_flags_off.txt") < 0;
    if (!options.shadow_vertex_flags)
        log("display: vita_test_shadow_flags_off.txt present: shadow receivers gathered per triangle");
    options.shadow_flags_check = vita_file_size(std::string(vita_data_root) + "/vita_test_shadow_flags_check.txt") >= 0;
    if (options.shadow_flags_check) log("display: vita_test_shadow_flags_check.txt present: shadow flags checked");
    options.alpha_proof = vita_file_size(std::string(vita_data_root) + "/vita_test_alpha_proof_off.txt") < 0;
    if (!options.alpha_proof)
        log("display: vita_test_alpha_proof_off.txt present: draws with an alpha test keep the full program");
    g_scene_cull = vita_file_size(std::string(vita_data_root) + "/vita_test_scene_cull_off.txt") < 0;
    if (!g_scene_cull) log("display: vita_test_scene_cull_off.txt present: models outside the view are drawn");
    options.alpha_test_variant = vita_file_size(std::string(vita_data_root) + "/vita_test_alpha_variant_off.txt") < 0;
    if (!options.alpha_test_variant)
        log("display: vita_test_alpha_variant_off.txt present: every scene draw uses the program with the alpha test");
    options.large_buffer_patches = vita_file_size(std::string(vita_data_root) + "/vita_test_patches_off.txt") < 0;
    if (!options.large_buffer_patches)
        log("display: vita_test_patches_off.txt present: large-buffer updates wait for the GPU and write in place");
        // ux0:data/omega_strain/vita_test_display_order.txt ("1" rows as read, "2" reversed): the GPU display copy's
        // row order without the first-use check (the same section; a hardware fallback should the check refuse).
        if (FILE* f = std::fopen((std::string(vita_data_root) + "/vita_test_display_order.txt").c_str(), "r")) {
            const int c = std::fgetc(f);
            std::fclose(f);
            options.display_copy_order = c == '2' ? 2 : c == '1' ? 1 : 0;
            log("display: vita_test_display_order.txt present: the GPU copy's rows %s", c == '2' ? "reversed"
                                                                                      : c == '1' ? "as read"
                                                                                                 : "checked (not 1 or 2)");
        }
        // The displayed frame rate (the presenting thread, after each swap), logged every 10 s: in-level fps evidence.
        options.presented = [] {
            static double window_start = 0.0, last = 0.0, longest = 0.0;
            static unsigned frames = 0;
            const double now = now_seconds();
            if (last > 0.0) longest = std::max(longest, now - last);
            last = now;
            if (window_start == 0.0) window_start = now;
            ++frames;
            if (now - window_start < 10.0) return;
            log("display: %u frames in %.1f s = %.1f fps (mean %.1f ms, longest %.1f ms)", frames, now - window_start,
                frames / (now - window_start), 1000.0 * (now - window_start) / frames, 1000.0 * longest);
            window_start = now;
            frames = 0;
            longest = 0.0;
        };
        gfx::Display display = gfx::gxm::create_display(options);
        try {
            App(display).run();
        } catch (const gfx::gxm::ShaderCompilerMissing& e) {
            log("FATAL: %s", e.what());
            gfx::gxm::show_message(e.what());
        } catch (const std::exception& e) {
            log("FATAL: %s", e.what());
            gfx::gxm::show_message(std::string("Omega Strain stopped:\n") + e.what());
        }
    } catch (const std::exception& e) {
        log("FATAL: %s", e.what());
    }
    log("exit");
    g_log_writer.stop();
    if (g_log) std::fclose(g_log);
    sceKernelExitProcess(0);
    return 0;
}
