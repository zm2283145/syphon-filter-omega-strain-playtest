#pragma once
// Checkpoint 126c: the level loading screen on the PC (hud_loading.hpp).
//
// build_loading_screen reads what 001350e0 / 00166430 read from the disc
// (GAMEDATA\<level>\LOADING.HOG, else GAMEDATA\COMMON\LOADING.HOG; the
// "loading_screen.IE" member; the common and mission STRINGS.DAT the
// 003fdb50 lookups see; the boot fonts) and sets the screen and the thread
// globals up as 002721b0 / 00271d80 / 00271b10 and 00166430 do.
//
// LoadingScreenHost is the loading thread 00166840 and its vblank handler
// 001667c0 on a host thread: vblanks at 59.94 Hz call 001667c0, each wakeup
// runs the frame 00166850 and presents it. Host choices (research/FRONTEND.md
// "Checkpoint 126c loading screen"): it presents into the session's game window
// (host_window.hpp, checkpoint 132win: the front end's and the play's window and
// device; the thread owns the device context until stop()) while the level
// load runs on the caller's thread; the original suspends the main
// thread during each frame, 0x37 SuspendThread / 10d0f0 resume); the clock is
// a host monotonic clock in the EE's 10 us ticks; stop() is the 00166190
// teardown (the thread ends, then 00271cd0).

#include "host_window.hpp"
#include "hud_loading.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace omega {

namespace crash {
class StallWatch;
}
class DiscImage;
struct GuiDiscFonts;
class GuiHogTextures;
struct Texture;

struct LoadingScreenAssets {
    std::vector<std::string> hog;          // the mounted HOG's disc path
    std::unique_ptr<GuiHogTextures> textures;
    std::unique_ptr<GuiDiscFonts> fonts;
    LoadingScreen screen;
    LoadingThreadGlobals globals;
    std::map<std::uint32_t, const Texture*> images() const;  // fonts' and the HOG's
    const GuiFont* font(std::uint32_t handle) const;
};

// 001350e0's LOADING.HOG choice and 00166430(app, 0, path) for `level`.
std::unique_ptr<LoadingScreenAssets> build_loading_screen(DiscImage& disc, const std::string& level);

// One thread frame at the clock `ticks` (the frame's clock reads all answer it).
GuiDrawList loading_frame(LoadingScreenAssets& assets, IeDrawState& state, std::uint64_t ticks);

struct LoadingHostOptions {
    bool hidden = false;                       // no window (frames still run)
    std::filesystem::path capture_dir;         // the last frame as loading_<LEVEL>.bmp at stop()
    HostWindow* host = nullptr;                // the game window (null: none, as hidden)
    // Port-only diagnostic (checkpoint 132win5, --test-device-lost-loading-sweep <frame>): from the thread's
    // frame <frame> on, frame k loses the device at its k-th checked D3D call (device_lost.hpp) until a frame
    // has fewer calls: every call of the frame (the draw, the present) loses it once. -1: off.
    std::int64_t test_device_lost_sweep = -1;
    // Port-only diagnostic (checkpoint 132mt1, --loading-capture-frame <frame>, with capture_dir): the thread's clock
    // is its vblank count (each NTSC field 1001000 / 600 ticks, as if every wakeup were on time), so the frames are
    // the same in every run, and drawn frame <frame> is written as loading_<LEVEL>_f<frame>.bmp (the window's back
    // buffer before its present; hidden: the offscreen renderer); stop() then waits until that frame is drawn. The
    // renderer refactor's pixel check. 132mt3: the screen is then held on that frame (no later frame is drawn), so the
    // frame count and the last frame do not depend on the load's wall time. -1: off.
    std::int64_t capture_frame = -1;
    // Port-only diagnostic (checkpoint 132mt3): the stall watch (crash_report.hpp StallWatch, the process's stall
    // watchdog). When the thread draws no frame for this many seconds while it runs, the watchdog logs its state
    // (frames, vblanks, the step it is in) and writes a stall report of every thread's state and stack with a minidump
    // (the crash folder, else local\stalls). It only reports: nothing is changed. 0: off.
    double stall_seconds = 30.0;
};

// Port-only (checkpoint 132mt3, research/THREADING.md "The loading-screen hang"): the loading screen's host thread is
// made once per session and runs each LoadingScreenHost in turn, so the level hand-off neither starts nor ends a
// thread; and it makes no D3D device there (the window's device, else `offscreen` here: one offscreen device made now,
// for the hidden front end's captures). On a saturated PC the NVIDIA driver's idle-priority threads can hold the
// Windows loader's work queue for minutes while they starve; any thread start, thread exit or LoadLibrary (a D3D device
// creation) then waits, and 132mt2's hand-off joined a thread doing exactly those. Call it early (play_main does,
// before the front end); a LoadingScreenHost starts it itself otherwise (no offscreen device: made per screen).
void start_loading_screen_thread(bool offscreen);
// Port-only (132ws8, research/FRONTEND.md "Shutdown (132ws8)"): releases start_loading_screen_thread's offscreen device
// (after its creation finished) on the calling thread; play_main's teardown calls it after the last LoadingScreenHost,
// so no device is left to the static destructors at the process exit. A later hidden screen makes its own.
void release_loading_screen_device();

class LoadingScreenHost {
public:
    LoadingScreenHost(std::unique_ptr<LoadingScreenAssets> assets, std::string level, LoadingHostOptions options);
    ~LoadingScreenHost();
    LoadingScreenHost(const LoadingScreenHost&) = delete;
    LoadingScreenHost& operator=(const LoadingScreenHost&) = delete;

    // 00166190: ends the thread (after its current frame), then 00271cd0.
    // Answers the frames drawn; rethrows a thread failure. Port-only (132mt3): the session's host thread finishes this
    // screen's run (it is not ended: start_loading_screen_thread).
    std::int64_t stop();
    // The user closed the window during the load.
    bool closed() const { return closed_; }
    std::optional<std::filesystem::path> capture() const { return capture_; }
    // Port-only (132win3, frame_pacer.hpp): the thread's vblank intervals (set once stop() returned).
    const std::string& pacing_summary() const { return pacing_summary_; }

private:
    void run();
    std::string stall_state() const;  // port-only (132mt3): the stall watch's description
    std::unique_ptr<LoadingScreenAssets> assets_;
    std::string level_;
    LoadingHostOptions options_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> closed_{false};
    std::atomic<std::int64_t> frames_{0};
    std::optional<std::filesystem::path> capture_;
    std::string pacing_summary_;
    std::exception_ptr failure_;
    // Port-only (132mt3): the stall watch's view of the thread (the step it is in, its vblank count; after the thread
    // has ended the probe keeps changing, so an ended thread is never reported).
    std::atomic<const char*> phase_{"not started"};
    std::atomic<std::uint64_t> vblanks_{0};
    std::atomic<bool> ended_{false};
    std::atomic<std::int64_t> ended_ticks_{0};
    std::unique_ptr<crash::StallWatch> watch_;
    struct Run;                  // port-only (132mt3): this screen's run on the session thread
    std::shared_ptr<Run> run_;
    bool stopped_ = false;
};

}  // namespace omega
