#include "loading_screen_host.hpp"

#include "disc.hpp"
#include "frame_fit.hpp"
#include "frame_pacer.hpp"
#include "frontend_formats.hpp"
#include "gui_renderer.hpp"
#include "localization.hpp"
#include "string_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "device_lost.hpp"
#include "worker_pool.hpp"
#include "crash_report.hpp"  // port-only (132mt3): the stall watch

namespace omega {

namespace {

// 0036def0 answers no file: the common screen.
std::vector<std::string> loading_hog(DiscImage& disc, const std::string& level) {
    std::vector<std::string> path{"GAMEDATA", level, "LOADING.HOG"};  // 0x49aa70
    try {
        disc.file(path);
        return path;
    } catch (const std::exception&) {
        return {"GAMEDATA", "COMMON", "LOADING.HOG"};  // 0x49aa90
    }
}

std::optional<StringPool> mission_strings(DiscImage& disc, const std::string& level) {
    try {
        return decode_string_pool(disc.read(disc.file({"GAMEDATA", level, "STRINGS.DAT"})));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::map<std::uint32_t, const Texture*> LoadingScreenAssets::images() const {
    std::map<std::uint32_t, const Texture*> out;
    for (const auto& [slot, image] : fonts->images) out[slot] = &image;
    for (const auto& [slot, image] : textures->images()) out[slot] = &image;
    return out;
}

const GuiFont* LoadingScreenAssets::font(std::uint32_t handle) const { return fonts->font(handle); }

std::unique_ptr<LoadingScreenAssets> build_loading_screen(DiscImage& disc, const std::string& level) {
    auto a = std::make_unique<LoadingScreenAssets>();
    a->hog = loading_hog(disc, level);
    a->fonts = std::make_unique<GuiDiscFonts>(load_gui_fonts(disc));
    std::string label;
    for (const auto& c : a->hog) label += (label.empty() ? "" : "\\") + c;
    a->textures = std::make_unique<GuiHogTextures>(disc.read(disc.file(a->hog)), label);
    // 00241b70: "%s.IE" of the name 002721b0 passes.
    const auto ie = a->textures->member("loading_screen.IE");
    if (!ie) throw std::runtime_error("00241b70: no loading_screen.IE in " + label);
    const IeFile file = decode_ie(*ie);
    // *0055a278: the callout font 00165950 stores (app +284); SetInterfaceFont
    // (001653e0) runs from level scripts, after this screen.
    constexpr std::uint32_t callout_font = GuiDiscFonts::default_handle + 1;
    a->screen = loading_screen_create(file, callout_font, a->textures.get());
    // 003fdb50 over the common pool then the mission's (003fdca0 loads it first).
    Localization strings(decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"}))));
    if (auto mission = mission_strings(disc, level)) strings.replace_mission(std::move(*mission));
    const LoadingStrings lookup = [&](const std::string& key) -> std::optional<std::string> {
        const auto s = strings.resolve(key);
        return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
    };
    constexpr std::uint32_t app_280 = GuiDiscFonts::default_handle;  // *(app + 0x280), the default font
    loading_screen_setup(a->screen, lookup, level, app_280);
    loading_screen_progress(a->screen, 0.0F);
    a->globals.screen = &a->screen;
    a->globals.font = app_280;
    a->globals.loading_text = lookup("LoadingText");
    a->globals.continue_text = lookup("Continue");
    // 003cba90("<LEVEL>_LOADINGFRAMES") answers 0 for every level of the disc's
    // SFO_GAME.INI: 1200, unset.
    a->globals.frames = 1200;
    return a;
}

GuiDrawList loading_frame(LoadingScreenAssets& assets, IeDrawState& state, std::uint64_t ticks) {
    GuiDrawList list;
    LoadingFrameServices services;
    services.ticks = [ticks] { return ticks; };
    services.font = [&](std::uint32_t h) { return assets.font(h); };
    loading_frame_166850(list, state, assets.globals, services);
    return list;
}

// Port-only (132mt3): this screen's run on the session's loading-screen host thread (HostThread::loading_screen).
struct LoadingScreenHost::Run {
    std::shared_ptr<HostThread::Completion> completion;
};

namespace {

// The session's offscreen device for the hidden front end's captures (made by start_loading_screen_thread on the host
// thread, before the front end).
std::mutex g_offscreen_mutex;
std::shared_ptr<gfx::Device> g_offscreen;
std::shared_ptr<HostThread::Completion> g_offscreen_made;

std::shared_ptr<gfx::Device> session_offscreen_device() {
    std::shared_ptr<HostThread::Completion> made;
    {
        std::lock_guard lock(g_offscreen_mutex);
        made = g_offscreen_made;
    }
    if (!made) return nullptr;
    made->wait();
    std::lock_guard lock(g_offscreen_mutex);
    return g_offscreen;
}

}  // namespace

void start_loading_screen_thread(bool offscreen) {
    auto& thread = HostThread::loading_screen();
    thread.start();
    if (!offscreen) return;
    std::lock_guard lock(g_offscreen_mutex);
    if (g_offscreen_made) return;
    g_offscreen_made = thread.run([] {
        try {
            auto device = gfx::create_offscreen_device("Create loading-screen GUI device");
            std::lock_guard lock(g_offscreen_mutex);
            g_offscreen = std::move(device);
        } catch (const std::exception& e) {
            std::cerr << "Loading screen (port-only): no session offscreen device (" << e.what()
                      << "); each hidden screen makes its own\n";
        }
    });
}

void release_loading_screen_device() {
    std::shared_ptr<HostThread::Completion> made;
    std::shared_ptr<gfx::Device> device;
    {
        std::lock_guard lock(g_offscreen_mutex);
        made = g_offscreen_made;
    }
    if (made) made->wait();
    {
        std::lock_guard lock(g_offscreen_mutex);
        device = std::move(g_offscreen);
        g_offscreen.reset();
    }
    device.reset();
}

LoadingScreenHost::LoadingScreenHost(std::unique_ptr<LoadingScreenAssets> assets, std::string level,
                                     LoadingHostOptions options)
    : assets_(std::move(assets)), level_(std::move(level)), options_(std::move(options)) {
    if (!assets_) throw std::invalid_argument("loading screen host: no screen");
    if (options_.stall_seconds > 0)
        watch_ = std::make_unique<crash::StallWatch>(
            "loading screen " + level_, options_.stall_seconds,
            [this]() -> std::int64_t { return ended_ ? -(++ended_ticks_) : frames_.load(); },
            [this] { return stall_state(); });
    run_ = std::make_shared<Run>();
    run_->completion = HostThread::loading_screen().run([this] { run(); });
}

LoadingScreenHost::~LoadingScreenHost() {
    try {
        stop();
    } catch (...) {
    }
}

std::int64_t LoadingScreenHost::stop() {
    if (stopped_) return frames_;
    stopped_ = true;
    stop_ = true;
    if (run_) run_->completion->wait();
    watch_.reset();
    loading_screen_destroy(assets_->screen);  // 00271cd0
    assets_->globals.screen = nullptr;
    if (failure_) std::rethrow_exception(failure_);
    return frames_;
}

// Port-only (132mt3): what the stall watch logs for this thread.
std::string LoadingScreenHost::stall_state() const {
    std::ostringstream m;
    m << "the loading-screen thread (" << level_ << "): frames " << frames_ << ", vblanks " << vblanks_
      << ", its step \"" << phase_.load() << "\", stop requested " << (stop_ ? 1 : 0) << ", capture frame "
      << options_.capture_frame;
    return m.str();
}

void LoadingScreenHost::run() {
    // Port-only (132mt3): the stall watch's end mark, whatever way the thread ends.
    struct Ended {
        LoadingScreenHost* h;
        ~Ended() {
            h->phase_ = "ended";
            h->ended_ = true;
        }
    } ended{this};
    phase_ = "make the renderer";
    try {
        using Clock = std::chrono::steady_clock;
        HostWindow* window = options_.hidden ? nullptr : options_.host;
        std::unique_ptr<GuiRenderer> renderer;
        std::uint64_t generation = 0;
        // Port-only (132win5): a loss while the renderer is made (its textures' upload) leaves the generation
        // stale; the frame's present re-creates the device and the next frame makes it again.
        const auto make_renderer = [&] {
            renderer.reset();
            if (window) {
                renderer = std::make_unique<GuiRenderer>(window->gfx());
                renderer->set_frame_fit(true);  // port-only 132win2: 4:3 with black bars (frame_fit.hpp)
                generation = window->device_generation();
            } else if (!options_.capture_dir.empty()) {
                // 132mt3: the session's offscreen device when start_loading_screen_thread made one (no device made here).
                if (auto device = session_offscreen_device())
                    renderer = std::make_unique<GuiRenderer>(std::move(device), 640, 448);
                else
                    renderer = std::make_unique<GuiRenderer>(640, 448);
            }
            if (renderer)
                for (const auto& [slot, image] : assets_->images()) renderer->set_texture(slot, *image);
        };
        try {
            make_renderer();
        } catch (const DeviceLost&) {
            renderer.reset();
        }
        std::optional<std::int64_t> sweep_call;  // port-only diagnostic (132win5): LoadingHostOptions sweep
        std::vector<std::string> sweep_sites;
        const auto epoch = Clock::now();
        std::uint64_t vblank_count = 0;  // port-only (132mt1): the fixed clock of --loading-capture-frame
        const bool fixed_clock = options_.capture_frame >= 0;
        const auto ticks = [&] {  // 10 us ticks
            if (fixed_clock) return vblank_count * 1001000ULL / 600ULL;
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count() / 10);
        };
        const auto capture_path = [&] {
            char name[32];
            std::snprintf(name, sizeof name, "_f%05lld.bmp", static_cast<long long>(options_.capture_frame));
            return options_.capture_dir / ("loading_" + level_ + name);
        };
        const std::chrono::duration<double> vblank(1001.0 / 60000.0);  // NTSC field
        auto next = epoch;
        // Port-only (132win3): the thread sleeps on a high-resolution timer (std::this_thread::sleep_until is a
        // coarse Sleep) to the next NTSC field; its intervals are the pacing summary.
        FramePacer pacer;
        FrameTimes times;
        const double cpu_start = process_cpu_seconds();
        IeDrawState draw;
        LoadingFrameServices services;
        services.ticks = ticks;
        services.font = [&](std::uint32_t h) { return assets_->font(h); };
        GuiDrawList last;
        std::int64_t drawn_frames = 0;
        // --loading-capture-frame: the thread runs on until that frame is drawn, even after stop() (the level load is
        // often quicker than 30 frames).
        while (!stop_ || (fixed_clock && frames_ < options_.capture_frame)) {
            next += std::chrono::duration_cast<Clock::duration>(vblank);
            phase_ = "sleep to the vblank";
            pacer.sleep_until(next);
            times.mark(Clock::now());
            ++vblank_count;
            ++vblanks_;
            if (window) {
                phase_ = "window events";
                window->clear_events();
                if (window->close_requested()) closed_ = true;
            }
            // Port-only (132mt3): with --loading-capture-frame the screen is held on that frame once it is drawn (no
            // later frame is drawn while the load runs on), so the frame count and the last frame are the same in every
            // run whatever the load's wall time. 132mt2's runs always stopped there because the thread started late
            // (its start waited on the Windows loader); the session's host thread starts at once.
            if (fixed_clock && frames_ >= options_.capture_frame) {
                phase_ = "held at the capture frame";
                continue;
            }
            if (!loading_vblank_1667c0(assets_->globals)) continue;
            GuiDrawList list;
            phase_ = "the frame 00166850";
            loading_frame_166850(list, draw, assets_->globals, services);
            ++frames_;
#ifdef __vita__
            // Port-only (132vl2, research/VITA.md "Load time (132vl2)"): the Vita draws and presents every 2nd game
            // frame (10 fps; the game frames still run at 20), leaving core 1 to the load's decode workers. The PC
            // draws every frame. A --loading-capture-frame frame is always drawn.
            const bool drawn = (frames_ & 1) != 0 || frames_ == options_.capture_frame;
#else
            const bool drawn = true;
#endif
            if (drawn) ++drawn_frames;
            if (drawn && window && !window->minimized()) {
                phase_ = "draw into the window";
                if (window && options_.test_device_lost_sweep >= 0 && frames_ == options_.test_device_lost_sweep &&
                    !sweep_call) {
                    sweep_call = 0;
                    std::cout << "Device-lost sweep (port-only diagnostic) from loading frame " << frames_ << "\n";
                }
                if (sweep_call) device_lost_inject(*sweep_call, "--test-device-lost-loading-sweep");
                // Port-only (132win3): a device lost in any D3D call drops the frame; present re-creates it.
                // 132win5: the renderer is made again inside the catch, whenever the device's generation moved.
                try {
                    if (!renderer || window->device_generation() != generation) make_renderer();
                    window->sync_buffers();
                    const float black[4] = {0, 0, 0, 1};
                    gfx::clear_target(*window->gfx(), window->back_buffer(), black);
                    // Port-only (132ws8, research/FRONTEND.md "Checkpoint 132ws8"): under the wide aspect the loading
                    // screen (the level's briefing) is drawn as the front end's wide menus (132ws5): its art extended
                    // to the window's edges, the text centred, the side fill only when nothing was extended; a mod's
                    // wide background ("loading.<level>", 132md12) replaces both. movie_fill=zoom keeps 132ws's one
                    // 4:3 picture scaled to the width; every other aspect draws as before.
                    const bool wide_menu = frame_wide(window->width(), window->height()) && !frame_movie_zoom_setting();
                    if (!wide_menu)
                        for (GuiDrawBatch& b : list.batches) b.port_picture = true;
                    const bool wide_background =
                        wide_menu && renderer->draw_wide_background("loading." + level_, window->back_buffer(),
                                                                    window->width(), window->height());
                    renderer->set_wide_edge_extend(wide_menu && !wide_background);
                    renderer->set_wide_menu(wide_menu);
                    renderer->draw(list, window->back_buffer(), window->width(), window->height());
                    renderer->set_wide_menu(false);
                    renderer->set_wide_edge_extend(false);
                    if (wide_menu && !wide_background && !list.batches.empty() && renderer->wide_edge_images() == 0)
                        renderer->draw_wide_fill(window->back_buffer(), window->width(), window->height());
                    if (frames_ == options_.capture_frame && !options_.capture_dir.empty()) {
                        std::filesystem::create_directories(options_.capture_dir);
                        if (window->capture(capture_path())) std::cout << "Loading screen capture (port-only) at loading frame " << frames_ << "\n";
                    }
                } catch (const DeviceLost&) {
                }
                phase_ = "present";
                window->present();
                if (sweep_call) {
                    if (device_lost_injection.load() >= 0) {
                        device_lost_inject(-1);
                        std::map<std::string, unsigned> sites;
                        for (const auto& site : sweep_sites) ++sites[site];
                        std::cout << "Device-lost sweep (port-only diagnostic) complete at loading frame " << frames_
                                  << ": the device was lost at each of the " << *sweep_call
                                  << " checked D3D calls of a loading frame in turn and recovered every time; calls:";
                        for (const auto& [site, n] : sites) std::cout << " [" << site << "] x" << n;
                        std::cout << "\n";
                        options_.test_device_lost_sweep = -1;
                        sweep_call.reset();
                    } else {
                        sweep_sites.push_back(device_lost_by());
                        ++*sweep_call;
                    }
                }
            }
            if (!window && renderer && frames_ == options_.capture_frame && !options_.capture_dir.empty()) {
                std::filesystem::create_directories(options_.capture_dir);
                renderer->clear({0.0F, 0.0F, 0.0F, 1.0F});
                renderer->draw(list);
                renderer->capture(capture_path());
                std::cout << "Loading screen capture (port-only) at loading frame " << frames_ << "\n";
            }
            last = std::move(list);
            // Behind schedule (a slow present): the missed vblanks are not replayed.
            if (Clock::now() > next + std::chrono::duration_cast<Clock::duration>(vblank)) next = Clock::now();
        }
        phase_ = "after the loop";
        {
            const double wall = std::chrono::duration<double>(Clock::now() - epoch).count();
            pacing_summary_ = frame_time_summary(times.stats(vblank.count() * 1000.0), vblank.count() * 1000.0) +
                              "; process CPU " + std::to_string(process_cpu_seconds() - cpu_start) + " s in " +
                              std::to_string(wall) + " s wall (the level load runs alongside)";
#ifdef __vita__
            pacing_summary_ += "; drawn " + std::to_string(drawn_frames) + " of " + std::to_string(frames_.load()) +
                               " frames (every 2nd, port-only 132vl2)";
#endif
        }
        if (renderer && !options_.capture_dir.empty()) {
            std::filesystem::create_directories(options_.capture_dir);
            const auto path = options_.capture_dir / ("loading_" + level_ + ".bmp");
            if (window) {
                GuiRenderer offscreen(window->gfx(), 640, 448);  // 132mt3: on the window's device (no device made)
                for (const auto& [slot, image] : assets_->images()) offscreen.set_texture(slot, *image);
                offscreen.clear({0.0F, 0.0F, 0.0F, 1.0F});
                offscreen.draw(last);
                offscreen.capture(path);
            } else {
                renderer->clear({0.0F, 0.0F, 0.0F, 1.0F});
                renderer->draw(last);
                renderer->capture(path);
            }
            capture_ = path;
        }
        if (sweep_call) {  // the load ended inside a sweep: disarmed so that the loss cannot fire in the level
            device_lost_inject(-1);
            std::cout << "Device-lost sweep (port-only diagnostic) cut short at loading frame " << frames_ << " after "
                      << *sweep_call << " calls\n";
        }
        // The play owns the device context next: the renderer's objects go and the back buffer view with them.
        renderer.reset();
        if (window) window->drop_target();
    } catch (...) {
        failure_ = std::current_exception();
    }
}

}  // namespace omega
