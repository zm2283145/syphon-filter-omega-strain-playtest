// Port-only (132ps5c, research/PS5.md "Design"): the PS5 app (PPSA99720). It is the PS5's counterpart of the PC's
// OmegaStrain.exe (play_main.cpp's wmain with launcher.cpp's run): it brings up the platform layer (log, fault
// handlers, the user service), the OpenGL 4.6 display (gfx_gl46.hpp, 1920 x 1080), finds and checks the user's disc
// image, derives the play host's disc data (derived_data.hpp, the launcher's derive step), builds the play host's
// argument list (launcher::play_arguments, the list OmegaStrain.exe passes) and runs the shared play host
// (omega_play_entry: the front end, the in-process hand-off, the loading screen, the level, the level end). Status
// and errors are shown on screen with the frame overlay's font and logged (ps5_log.hpp); the title never returns to
// the shell on its own (a returning title reads as a crash), it waits until the user closes it.
//
// Files: the working folder is /data/omega_strain (a plain folder FTP reaches, also after a crash), else /download0
// when /data cannot be written; it holds config/disc_us.json (copied from /app0/assets/config), local/... (settings,
// memory card, derived data, crash reports), mods/, omega_ps5.log (also in /download0) and gl-stdout.log. The sandbox
// refuses chdir, so the working folder is the platform layer's own (ps5_cwd.cpp). The disc image is the first of
// iso_candidates below; <working folder>/play_args.txt (one argument per line, "!--x" drops one) adds play host
// arguments, as the Vita app's play_args.txt.

#include "crash_report.hpp"
#include "crash_report_ps5.hpp"
#include "derived_data.hpp"
#include "disc.hpp"
#include "frame_overlay.hpp"
#include "gfx_device.hpp"
#include "gfx_gl46.hpp"
#include "gfx_gxm_shaders.hpp"
#include "host_window.hpp"
#include "iso_verify_ps5.hpp"
#include "launcher.hpp"
#include "ps5_imports.hpp"
#include "ps5_log.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// play_main.cpp's entry on the non-Windows targets (the PC's wmain without the launcher).
int omega_play_entry(int argc, wchar_t** argv);

extern "C" {
// The OpenGL runtime's malloc heap (native-app/app_heap.c wraps malloc / new): CPU-cached direct memory, mapped once.
// 1 GiB, the size the Melee PS5 port runs with (the level's heap is about 0.5 GB on the PC; past it the runtime
// allocates from the libc heap). 132ps5c hw2 asked for 2 GiB (2^31 bytes) and faulted in the first allocations.
extern const std::size_t ps5_opengl_heap_size;
const std::size_t ps5_opengl_heap_size = std::size_t(1024) << 20;
int sceKernelGetModuleInfoFromAddr(const void* address, int flags, void* info);
int sceUserServiceInitialize(void* params);
}

namespace {

using namespace omega;
namespace fs = std::filesystem;

constexpr unsigned screen_width = 1920, screen_height = 1080;
// The working folder (choose_data_root): /data/omega_strain, else /download0.
std::string data_root = "/download0";
const char* const iso_candidates[]{"/app0/omega_strain.iso", "/download0/omega_strain.iso",
                                   "/data/omega_strain/omega_strain.iso", "/mnt/usb0/omega_strain.iso",
                                   "/mnt/usb1/omega_strain.iso"};
const char* const title_line = "SYPHON FILTER: THE OMEGA STRAIN (NATIVE PORT, PS5)";

double now_seconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

std::int64_t file_size(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : -1;
}

// ---- faults ----

__attribute__((noinline)) int main_marker() { return 0; }

// /data/omega_strain when it can be made and written (FTP reaches it after a crash), else /download0.
std::string choose_data_root() {
    const char* folder = "/data/omega_strain";
    mkdir(folder, 0777);
    const std::string probe = std::string(folder) + "/.write_test";
    if (FILE* f = std::fopen(probe.c_str(), "w")) {
        std::fclose(f);
        std::remove(probe.c_str());
        return folder;
    }
    return "/download0";
}

void describe_address(const char* label, std::uintptr_t value) {
    static std::uint64_t info[0x400 / 8];
    const auto self = reinterpret_cast<std::uintptr_t>(&main_marker);
    if (value + 0x2000000u >= self && value < self + 0x4000000u) {
        ps5::log("%s 0x%llx  main_marker%+lld", label, static_cast<unsigned long long>(value),
                 static_cast<long long>(value - self));
        return;
    }
    std::memset(info, 0, sizeof info);
    info[0] = 0x160;
    if (sceKernelGetModuleInfoFromAddr(reinterpret_cast<const void*>(value), 1, info) != 0) {
        std::memset(info, 0, sizeof info);
        info[0] = 0x1a8;
        if (sceKernelGetModuleInfoFromAddr(reinterpret_cast<const void*>(value), 1, info) != 0) {
            if (label[0] != ' ') ps5::log("%s 0x%llx  (no module)", label, static_cast<unsigned long long>(value));
            return;
        }
    }
    reinterpret_cast<char*>(info)[8 + 255] = 0;
    const auto seg0 = static_cast<std::uintptr_t>(info[33]);  // ModuleInfo: segments from byte 264 ({addr, size, prot})
    ps5::log("%s 0x%llx  %s (+0x%llx)", label, static_cast<unsigned long long>(value),
             reinterpret_cast<const char*>(info) + 8, static_cast<unsigned long long>(value - seg0));
}

// SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT: the registers and the code addresses near the stack pointers go to
// the log (the Melee PS5 port's handler), then the signal's default action ends the process.
void on_fault(int sig, siginfo_t* info, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    // The PS5's ucontext is FreeBSD's mcontext six words later than the SDK header says (132ps5c hw4: cs 0x43, rflags,
    // rsp, ss at words 29..32): rip is word 28 and rsp word 31.
    const auto* context_words = reinterpret_cast<const std::uintptr_t*>(uc);
    const auto rip = uc ? context_words[28] : 0u;
    const auto rsp = uc ? context_words[31] : 0u;
    ps5::log("FATAL signal %d addr=%p rip=%p rsp=%p main_marker=%p (link address = runtime - (main_marker - its link address))",
             sig, info ? info->si_addr : nullptr, reinterpret_cast<void*>(rip), reinterpret_cast<void*>(rsp),
             reinterpret_cast<void*>(&main_marker));
    if (uc) {
        // The PS5's ucontext layout is not FreeBSD's (mc_rip / mc_rsp may be misread): the raw words, code pointers
        // into this executable marked with their link address (main_marker's link address + their offset).
        const auto* raw = reinterpret_cast<const std::uintptr_t*>(uc);
        const auto base = reinterpret_cast<std::uintptr_t>(&main_marker);
        for (int i = 0; i < 96; ++i) {
            const std::uintptr_t v = raw[i];
            const bool code = v + 0x2000000u >= base && v < base + 0x4000000u;
            if (code || (i < 48 && v != 0))
                ps5::log("  uc[%d] = 0x%llx%s%+lld", i, static_cast<unsigned long long>(v), code ? "  main_marker" : "",
                         code ? static_cast<long long>(v - base) : 0LL);
        }
        describe_address("rip", rip);
        if (rsp >= 0x7e0000000ull && rsp < 0x800000000ull)
            describe_address("[rsp] (the caller when rip is 0)", *reinterpret_cast<const std::uintptr_t*>(rsp));
        const auto* words = reinterpret_cast<const std::uintptr_t*>(uc);
        std::uintptr_t seen[8];
        int nseen = 0;
        for (int i = 2; i < 40; ++i) {
            const std::uintptr_t v = words[i] & ~std::uintptr_t(7);
            if (v < 0x7e0000000ull || v >= 0x800000000ull) continue;  // thread stacks
            bool dup = false;
            for (int j = 0; j < nseen; ++j) dup = dup || seen[j] == v;
            if (dup || nseen == 8) continue;
            seen[nseen++] = v;
            ps5::log("  stack from uc[%d]=0x%llx:", i, static_cast<unsigned long long>(v));
            for (int k = -4; k < 48; ++k) {
                const std::uintptr_t w = reinterpret_cast<const std::uintptr_t*>(v)[k];
                if (w < 0x400000u || w >= 0x1000000000ull) continue;
                char label[32];
                std::snprintf(label, sizeof label, "   [%+d]", k);
                describe_address(label, w);
            }
        }
    }
    ps5::log_flush();
    ps5::log_drain();
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_fault_handlers() {
    static const int signals[]{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction action{};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    for (int s : signals) sigaction(s, &action, nullptr);
    std::set_terminate([] {
        std::string what = "unknown";
        if (const auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                what = x.what();
            } catch (...) {
            }
        }
        ps5::log("FATAL std::terminate (an exception escaped a thread or a noexcept function): %s", what.c_str());
        ps5::log_flush();
        ps5::log_drain();
        std::abort();
    });
}

// ---- the app ----

class App {
public:
    void run();

private:
    void screen(const std::vector<std::string>& lines, double fraction = -1.0);
    [[noreturn]] void stop(const std::string& title, const std::string& text);
    std::string find_iso();
    int play(DiscImage& disc, const std::string& iso);

    gfx::Display display_;
    std::shared_ptr<gfx::Texture> back_;
    FrameOverlay overlay_;
    double drawn_ = 0.0;
};

// One status frame: the lines in the overlay font (3 x scale) and an optional progress bar.
void App::screen(const std::vector<std::string>& lines, double fraction) {
    if (!display_.device) return;
    auto& device = *display_.device;
    if (!back_) back_ = display_.swap_chain->buffer(true, "back buffer", "back buffer view");
    gfx::CommandList list;
    const float black[4]{0, 0, 0, 1};
    list.clear_target({back_.get(), 0}, black);
    if (fraction >= 0.0) {
        const float frame[4]{0.25F, 0.27F, 0.32F, 1.0F}, fill[4]{0.85F, 0.55F, 0.12F, 1.0F};
        const gfx::Rect outer{360, 600, 1560, 640};
        list.clear_rects(back_.get(), frame, &outer, 1);
        const int right = 364 + int(1192.0 * std::min(fraction, 1.0));
        if (right > 364) {
            const gfx::Rect inner{364, 604, right, 636};
            list.clear_rects(back_.get(), fill, &inner, 1);
        }
    }
    device.submit(list);
    overlay_.draw(device, back_.get(), screen_width, screen_height, lines, 120, 300, 3);
    display_.swap_chain->present(1, "present");
    drawn_ = now_seconds();
}

// A stop the app cannot recover from: on screen (and in the log) until the user closes the title.
void App::stop(const std::string& title, const std::string& text) {
    ps5::log("STOP: %s: %s", title.c_str(), text.c_str());
    std::vector<std::string> lines{title, ""};
    constexpr std::size_t width = 90;
    std::string line, word;
    auto flush_word = [&] {
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
        } else if (c == ' ') {
            flush_word();
        } else {
            word += c;
        }
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    lines.push_back("");
    lines.push_back("THE LOG IS /DOWNLOAD0/OMEGA_PS5.LOG. CLOSE THE GAME FROM THE HOME SCREEN.");
    ps5::log_flush();
    for (;;) {
        try {
            screen(lines);
        } catch (const std::exception& e) {
            ps5::log("stop screen: %s", e.what());
            for (;;) sleep(60);
        }
    }
}

std::string App::find_iso() {
    for (const char* path : iso_candidates) {
        const auto size = file_size(path);
        ps5::log("disc image: %s %s", path, size < 0 ? "(none)" : (std::to_string(size) + " bytes").c_str());
        if (size >= 0) return path;
    }
    return {};
}

void App::run() {
    if (chdir(data_root.c_str()) != 0) ps5::log("working folder %s: chdir failed (errno %d)", data_root.c_str(), errno);
    char cwd[512]{};
    ps5::log("working folder %s", getcwd(cwd, sizeof cwd) ? cwd : "(getcwd failed)");

    gfx::gl46::Options options;
    options.debug_output = file_size(data_root + "/gl_debug.txt") >= 0;
    // The displayed frame rate, logged every 10 s (the presenting thread, after each swap).
    options.presented = [] {
        static double window_start = 0.0, last = 0.0, longest = 0.0;
        static unsigned frames = 0;
        const double now = now_seconds();
        if (last > 0.0) longest = std::max(longest, now - last);
        last = now;
        if (window_start == 0.0) window_start = now;
        ++frames;
        if (now - window_start < 10.0) return;
        ps5::log("display: %u frames in %.1f s = %.1f fps (mean %.1f ms, longest %.1f ms); %s", frames,
                 now - window_start, frames / (now - window_start), 1000.0 * (now - window_start) / frames,
                 1000.0 * longest, gfx::gl46::stats_line().c_str());
        window_start = now;
        frames = 0;
        longest = 0.0;
    };
    display_ = gfx::gl46::create_display(options);
    ps5::log("display: %s", display_.backend.c_str());
    screen({title_line, "PREPARING THE SHADERS"});
    for (std::size_t p = 0; p < gfx::gxm::gxm_program_count; ++p) {
        try {
            display_.device->prepare(static_cast<gfx::Program>(p));
        } catch (const std::exception& e) {
            ps5::log("program %u: FAILED: %s", unsigned(p), e.what());
        }
    }

    const std::string iso = find_iso();
    if (iso.empty()) {
        std::string where;
        for (const char* path : iso_candidates) where += std::string("\n") + path;
        stop("NO DISC IMAGE",
             "Copy your own verified disc image (research/REQUIRED_DISC.md) as omega_strain.iso to one of:" + where);
    }
    screen({title_line, "CHECKING THE DISC IMAGE " + iso}, 0.0);
    const double verify_start = now_seconds();
    std::string how;
    IsoStream stream;
    try {
        stream = open_checked_iso_ps5(
            iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256,
            [&](std::uint64_t done, std::uint64_t total) {
                if (now_seconds() - drawn_ < 0.25) return;
                char text[120];
                std::snprintf(text, sizeof text, "VERIFYING THE DISC IMAGE (SHA-256, ONCE): %llu / %llu MB",
                              static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(total >> 20));
                screen({title_line, text}, double(done) / double(total));
            },
            how);
    } catch (const std::exception& e) {
        stop("THE DISC IMAGE WAS NOT ACCEPTED", e.what());
    }
    ps5::log("disc image checked in %.1f s: %s", now_seconds() - verify_start, how.c_str());
    DiscImage disc(stream, OMEGA_ISO_SIZE);
    int code = 0;
    std::string error;
    try {
        code = play(disc, iso);
    } catch (const std::exception& e) {
        error = e.what();
        code = -1;
    }
    if (error.empty()) error = crash::ps5_last_message();
    stop("THE GAME STOPPED (EXIT " + std::to_string(code) + ")", error.empty() ? "See the log." : error);
}

// The play host as OmegaStrain.exe runs it (launcher.cpp's run): the working folder's config/disc_us.json and
// local/derived, then the launcher's argument list.
int App::play(DiscImage& disc, const std::string& iso) {
    std::error_code ec;
    fs::create_directories("config", ec);
    fs::create_directories("local/derived", ec);
    const std::string profile = data_root + "/config/disc_us.json";
    if (file_size(profile) != file_size("/app0/assets/config/disc_us.json")) {
        std::ifstream in("/app0/assets/config/disc_us.json", std::ios::binary);
        std::ofstream out(profile, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
        ps5::log("play host: %s copied from the package", profile.c_str());
    }
    const derived::DiscFingerprint fingerprint{OMEGA_ISO_SHA256, OMEGA_BOOT_EXECUTABLE, OMEGA_EXE_SHA256, OMEGA_EXE_SIZE};
    const fs::path derived_folder = "local/derived";
    const auto files = derived::derived_files(derived_folder);
    if (!derived::derived_data_current(files, fingerprint)) {
        screen({title_line, "PREPARING THE GAME DATA (THE FIRST START READS THE BOOT EXECUTABLE AND THE MOTION FILES)"});
        const double start = now_seconds();
        derived::derive_game_data(disc, fingerprint, derived_folder);
        ps5::log("play host: derived the motion graphs and tunings into local/derived in %.1f s", now_seconds() - start);
    } else {
        ps5::log("play host: local/derived is current");
    }

    launcher::Options options;
    options.dialogs = false;
    std::vector<std::wstring> drops;
    if (std::ifstream in{data_root + "/play_args.txt"}) {
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '!') {
                drops.emplace_back(line.begin() + 1, line.end());
                ps5::log("play host: argument %s dropped (play_args.txt)", line.c_str() + 1);
                continue;
            }
            options.extra.emplace_back(line.begin(), line.end());
            ps5::log("play host: argument %s (play_args.txt)", line.c_str());
        }
    }
    std::vector<std::wstring> storage{L"omega_ps5"};
    for (auto& a : launcher::play_arguments(fs::path(iso), files, options))
        if (std::find(drops.begin(), drops.end(), a) == drops.end()) storage.push_back(std::move(a));
    std::vector<wchar_t*> pointers;
    for (auto& a : storage) pointers.push_back(a.data());
    pointers.push_back(nullptr);
    {
        std::string line;
        for (const auto& a : storage) line += std::string(a.begin(), a.end()) + " ";
        ps5::log("play host: %s", line.c_str());
    }
    back_.reset();
    set_ps5_window_display({display_, screen_width, screen_height, {}});
    crash::install({fs::absolute("local") / "crashes", false});
    crash::ps5_before_exit = [] { ps5::log_flush(); };
    const int code = omega_play_entry(int(storage.size()), pointers.data());
    std::cout.flush();
    std::cerr.flush();
    ps5::log("play host: ended (exit %d)", code);
    return code;
}

void* game_thread(void*) {
    try {
        App().run();
    } catch (const std::exception& e) {
        ps5::log("FATAL: %s", e.what());
    }
    ps5::log_flush();
    return nullptr;
}

}  // namespace

int main() {
    // The GL runtime prints its own profile to stdout; the game's std::cout / std::cerr go to the log.
    data_root = choose_data_root();
    if (data_root != "/download0") ps5::log_mirror((data_root + "/omega_ps5.log").c_str());
    if (std::freopen((data_root + "/gl-stdout.log").c_str(), "w", stdout)) std::setvbuf(stdout, nullptr, _IOLBF, 0);
    ps5::redirect_streams();
    ps5::log("Syphon Filter: The Omega Strain, native port, PS5 (132ps5c); main_marker at %p",
             reinterpret_cast<void*>(&main_marker));
    ps5::log("data folder %s (the log is also in %s)", data_root.c_str(), ps5::log_file);
    if (const unsigned missing = ps5::log_missing_imports()) {
        // Port-only (132ps5c): a call through a null import faults with rip 0 somewhere much later; stop here instead.
        ps5::log("FATAL: %u imports are not resolved by the system modules (listed above); the title must implement"
                 " them (src/port/ps5/ps5_cwd.cpp, ps5_libc_extras.cpp). Stopping.", missing);
        ps5::log_flush();
        return 1;
    }
    ps5::log_flush();
    install_fault_handlers();
    const int user = sceUserServiceInitialize(nullptr);
    if (user != 0 && user != int(0x80960003)) ps5::log("sceUserServiceInitialize: 0x%08x", unsigned(user));
    // The play host's deepest call chains (the level load, the mission scripts) want a large stack.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::size_t(32) << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, game_thread, nullptr) != 0) {
        ps5::log("could not start the game thread");
    } else {
        pthread_join(thread, nullptr);
    }
    pthread_attr_destroy(&attr);
    ps5::log("game thread ended; the title stays until it is closed from the home screen");
    ps5::log_flush();
    for (;;) sleep(60);
}
