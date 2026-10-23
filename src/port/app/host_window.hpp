#pragma once
// Port-only (checkpoint 132win, not the original): the one PC game window of an omega_play session and its
// D3D11 device and flip swap chain. The boot movies, the title and menus (frontend_app), the loading screen
// (loading_screen_host), the level play (play_main: SceneRenderer, the GUI overlay, particles, fades) and the
// level end all present into it, so the window is never closed and reopened between them
// (research/PLAY_SANDBOX.md "Host window and fullscreen").
//
// The window belongs to a host UI thread that only dispatches its messages: the loading screen presents
// from its own thread while the main thread loads the level, and a hand-off never leaves the window
// unpumped. The window procedure only records state (focus, minimise, size, close, keys, wheel) for the
// mode that is running; the D3D11 immediate context is used by one thread at a time (the loading thread
// while it runs, else the main thread).
//
// Each mode makes its own renderers on device(), releases them when it ends, and owns the back buffer
// while it runs: the front end and the loading screen draw into target(); the play's SceneRenderer makes
// its own view of the same buffer (drop_target() first). Nothing here reaches the game's state or clock.
//
// Fullscreen (port-only): borderless, covering the monitor the window is on; Alt+Enter and F11 toggle it
// at any time (the UI thread applies it), set_fullscreen() from the options row or --fullscreen. Leaving it
// restores the windowed placement. The swap chain follows the client size. Checkpoint 132win2: the 640 x 448
// PS2 frame (front end, movies, loading screen, the play's flat GUI) is drawn into the centred 4:3 rectangle
// of the client (render/frame_fit.hpp; black pillarbox / letterbox bars), never stretched; the play's 3D
// scene fills the client at its own aspect (Hor+, fixed vertical field of view).
//
// HiDPI (port-only, checkpoint 132win14): the process is per-monitor DPI aware (v2, enable_per_monitor_dpi_awareness
// at the top of wmain), so every size and position here is in physical pixels and the swap chain matches the
// client area pixel for pixel (no bitmap scaling by Windows on a 125 % / 150 % display). A window's DPI is its
// monitor's (dpi()). Sizes: HostWindowOptions' width x height is a client size at 96 DPI (the outer size's frame
// also measured at 96 DPI when outer_size); a dpi_scaled window (the visible game window at its default size)
// scales that client size by dpi / 96 and fits it to the monitor's work area, otherwise the client keeps that pixel
// size (hidden runs and --window-size: the same captures on a scaled machine as at 100 %) with the frame drawn at
// the window's DPI. WM_DPICHANGED (the window dragged to a monitor of another scale, or the scale changed) applies
// the rectangle Windows suggests (a fullscreen window covers its monitor again; a kept-hidden one keeps its size);
// the buffers follow at the next sync_buffers, and the render scale and the 4:3 fit (frame_fit.hpp) work on the
// new client pixels. The saved window= placement is in physical pixels (window_units=pixels, saved_window_pixels).

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "gfx_device.hpp"
#include "port_settings.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace omega {

// The outer window rectangle (left, top, right, bottom).
using WindowRect = std::array<int, 4>;

inline constexpr const wchar_t* game_window_title = L"Syphon Filter: The Omega Strain";

struct HostWindowOptions {
    std::wstring title = game_window_title;
    int width = 1280, height = 896;  // the client size, or the outer size when outer_size
    bool outer_size = false;
    float render_scale = 1.0F;  // checkpoint 132win10: the swap chain's buffers over the client size (set_render_scale)
    // Checkpoint 132win10 port-only diagnostics (the present-stall study; play's --swap-effect / --frame-latency /
    // --waitable-swap): the swap effect (flip discard by default, the legacy blt discard as the fallback), the
    // device's maximum frame latency (1 by default) and the frame-latency waitable object (off by default).
    enum class SwapEffect : std::uint8_t { flip_discard, flip_sequential, discard };
    SwapEffect swap_effect = SwapEffect::flip_discard;
    unsigned frame_latency = 1;
    bool waitable = false;
    // Checkpoint 132win14 (port-only HiDPI): scale the 96-DPI client size by the window's DPI and fit it to the
    // monitor's work area (the visible game window at its default size); off: the client keeps width x height pixels.
    bool dpi_scaled = false;
    // Port-only test hook: the DPI the sizing uses instead of the window's (0: GetDpiForWindow).
    unsigned dpi_override = 0;
};

// Checkpoint 132win14 (port-only HiDPI). Sets the process per-monitor DPI aware (v2; v1 or system aware on an older
// Windows); call before any window is made. Answers the awareness now in effect ("per-monitor v2", ...).
std::string enable_per_monitor_dpi_awareness();
// The awareness of the calling thread ("unaware", "system", "per-monitor", "per-monitor v2").
std::string dpi_awareness_name();
// The system DPI (the primary monitor's at sign-in; 96 for an unaware process).
unsigned system_dpi();
// v * dpi / 96, rounded to nearest (MulDiv).
int scale_for_dpi(int v, unsigned dpi);
// The outer size's frame for a WS_OVERLAPPEDWINDOW at that DPI: {extra width, extra height}.
std::array<int, 2> window_frame_extra(unsigned dpi);
struct HostWindowSize {
    int client_width = 0, client_height = 0, outer_width = 0, outer_height = 0;
    bool fitted = false;  // a dpi_scaled size shrunk (aspect kept) to fit the work area
};
// The window's sizes at `dpi` (see the HiDPI note above); work_area: the monitor's (dpi_scaled only, nullptr: no fit).
HostWindowSize host_window_size(const HostWindowOptions& options, unsigned dpi, const WindowRect* work_area);

struct HostEvent {
    enum class Type : std::uint8_t { key, wheel };
    Type type{};
    int value = 0;  // key: the virtual key (WM_KEYDOWN, repeats included); wheel: the delta
};

enum class PresentResult : std::uint8_t { ok, occluded, device_lost };

class HostWindow {
public:
    explicit HostWindow(const HostWindowOptions& options = {});
    ~HostWindow();
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    HWND handle() const { return hwnd_; }

    // D3D11 (hardware with BGRA support at feature level 11_0, else WARP, as SceneRenderer made its own).
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGISwapChain* swap_chain() const { return swap_.Get(); }
    const std::string& backend() const { return backend_; }
    // Checkpoint 132mt1 (port-only): the same device, swap chain and back buffer as the renderer abstraction layer's
    // objects (gfx_device.hpp, the D3D11 backend gfx_d3d11.cpp), for the drawing code: the front end, the loading
    // screen and the play's renderers draw through these; the D3D11 accessors above stay for this window's own
    // work and the device tests. Re-made with the device (recover_device).
    const std::shared_ptr<gfx::Device>& gfx() const { return gfx_; }
    // The play's scene display: this device and swap chain, shared (the renderer never releases them).
    gfx::Display scene_display() const { return {gfx_, gfx_swap_, backend_, false}; }
    // target() as a gfx texture (made on first use; dropped with the view).
    const gfx::Texture* back_buffer();
    // Counts device re-creations after a device loss; a mode's renderers are stale when it changes.
    std::uint64_t device_generation() const { return generation_; }

    // The back buffer's view (made on first use) and size.
    ID3D11RenderTargetView* target();
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    // Releases the view and the context's bindings (before another renderer owns the back buffer).
    void drop_target();
    // Resizes the buffers to the client area when it changed (target dropped first). Answers whether it did.
    bool sync_buffers();
    // Present with a sync interval (0: no vsync; 132win3: n > 1 holds the frame n vertical blanks, frame_pacer.hpp);
    // a removed device is re-created (generation + 1) and reported as device_lost.
    PresentResult present(unsigned sync_interval = 1);
    // Port-only (checkpoint 132win2): releases the device, context, swap chain and view and makes new ones
    // (generation + 1). present() does it on DEVICE_REMOVED / DEVICE_RESET (a TDR, a driver update, the
    // adapter going away); the play's --test-device-lost diagnostic calls it directly. Every renderer made
    // on the old device must be re-created or reset_device()d by its mode afterwards.
    void recover_device(const std::string& why);
    // Port-only diagnostic (checkpoint 132win5): the back buffer as drawn so far this frame (before present) to
    // a BMP; false (nothing written) when the device is lost.
    bool capture(const std::filesystem::path& path);
    // The removed device's reason after a device_lost.
    HRESULT lost_reason() const { return lost_reason_; }

    // Window state, written by the UI thread.
    bool focused() const { return focused_; }
    bool minimized() const { return minimized_; }
    bool close_requested() const { return close_; }
    // Port-only diagnostic (checkpoint 132win5, --test-minimize): reports the window minimised as WM_SIZE's
    // SIZE_MINIMIZED does (a hidden window cannot be minimised); off restores it (size serial + 1, as WM_SIZE).
    void simulate_minimized(bool on);
    // Port-only diagnostic (checkpoint 132win12, --test-focus-loss): reports the window unfocused / focused as
    // WM_ACTIVATE / WM_KILLFOCUS do (a hidden window is never activated); focus_simulated() then tells the hosts
    // to follow focused() in a hidden run.
    void simulate_focus(bool on) {
        focus_simulated_ = true;
        focused_ = on;
    }
    bool focus_simulated() const { return focus_simulated_; }
    // Port-only diagnostic (checkpoint 132win12, --test-alt-enter): sends the window procedure the Alt+Enter
    // WM_SYSKEYDOWN a keyboard would (hotkeys allowed for that one message, as in a visible run).
    void simulate_alt_enter();
    // Port-only diagnostic (checkpoint 132win14 HiDPI): sends the window procedure the WM_DPICHANGED Windows sends when
    // the window's monitor DPI changes (wParam the new DPI, lParam the suggested outer rectangle); a kept-hidden
    // window applies it too for this one message (the test's view of a visible window's handling).
    void simulate_dpi_change(unsigned dpi, const WindowRect& suggested);
    // The window's DPI (96 = 100 %): GetDpiForWindow at creation (or dpi_override), then each WM_DPICHANGED.
    unsigned dpi() const { return dpi_; }
    std::uint64_t size_serial() const { return size_serial_; }
    std::vector<HostEvent> take_events();
    void clear_events();

    // Window commands (run on the UI thread).
    void show(bool activate);   // a no-op while keep_hidden(true)
    // Keeps the window invisible through every show (headless runs that still drive the shared window).
    void keep_hidden(bool on) { keep_hidden_ = on; }
    bool kept_hidden() const { return keep_hidden_; }
    void hide();
    bool visible() const;
    void set_title(const std::wstring& title);
    // The outer size (windowed; leaves fullscreen first).
    void set_outer_size(int width, int height);
    WindowRect rect() const;
    // Port-only (checkpoint 132win5): the windowed placement's outer rectangle in screen coordinates: the current
    // one, the restored one of a minimised or maximised window, or the one leaving fullscreen restores. It is the
    // port settings' window= key (saved at the session's end, fit_saved_window at the next start).
    WindowRect windowed_rect() const;
    // Moves the windowed placement there (a fullscreen window keeps it for its restore); visibility unchanged.
    void set_windowed_rect(const WindowRect& r);
    std::array<unsigned, 2> client_size() const;
    // Port-only (checkpoint 132win10): the internal render scale (port settings render_scale=, frame_fit.hpp
    // scaled_extent): the swap chain's buffers are the client size times it (DXGI stretches them over the client
    // area). 1 by default (the buffers are the client size). Takes effect at the next sync_buffers / device.
    void set_render_scale(float scale) { render_scale_ = scale; }
    float render_scale() const { return render_scale_; }
    // The buffer size for the current client area (client_size scaled; 0 x 0 for an empty client area).
    std::array<unsigned, 2> buffer_size() const;
    // Port-only (checkpoint 132win10): under the 4:3 aspect setting (frame_fit.hpp frame_masked) paints the bars
    // outside the fitted 4:3 frame black on the back buffer; present() and capture() call it, a no-op otherwise.
    void mask_bars();
    // Port-only diagnostics (132win10): the swap chain made ("flip discard, 2 buffers, latency 1", ...), and the
    // frame-latency wait of a waitable swap chain (answers at once otherwise): the milliseconds waited.
    const std::string& swap_description() const { return swap_description_; }
    double wait_frame_latency(unsigned timeout_ms = 1000);

    bool fullscreen() const { return fullscreen_; }
    void set_fullscreen(bool on);
    void toggle_fullscreen() { set_fullscreen(!fullscreen_); }
    // Called on the UI thread after each change (hotkey or set_fullscreen), e.g. to write the setting.
    void on_fullscreen_change(std::function<void(bool)> callback);
    // Hotkey toggles are ignored while false (hidden runs).
    void allow_hotkeys(bool allow) { hotkeys_ = allow; }

    // Port-only mouse look (checkpoint 132win7, mouse_look.hpp). The window reads the mouse through raw input
    // (relative counts, no pointer acceleration). The cursor is captured (hidden over the client area and clipped
    // to it) only while it is wanted (a level playing) and the window is visible, focused and not minimised
    // (mouse_capture_active); it is released otherwise, and the motion is only counted while captured.
    // Set by the play each window frame; a change is applied on the UI thread.
    void want_mouse_capture(bool on);
    bool mouse_captured() const { return mouse_captured_; }
    // The motion counted since the last call (dx right, dy down), then zero.
    std::array<int, 2> take_mouse_delta();
    // Port-only diagnostic: counts motion as a captured WM_INPUT does (the unit test; no real mouse is moved).
    void simulate_mouse_motion(int dx, int dy);

    // The evidence line of a hand-off: the window handle and the process working set.
    void log_handoff(const std::string& where) const;
    // Port-only (checkpoint 132win5): the destructor logs the shutdown (the device's references left after its
    // release: 0 when every renderer made on it was released first, a balanced teardown).
    void log_shutdown(bool on) { log_shutdown_ = on; }

    // UI-thread internals (the window procedure).
    LRESULT handle_message(HWND window, UINT message, WPARAM w, LPARAM l);

private:
    void create_device();
    void apply_fullscreen(bool on);  // UI thread
    void update_mouse_capture();     // UI thread
    void on_ui(const std::function<void()>& f) const;
    unsigned long release_device();  // the device's references left

    HWND hwnd_ = nullptr;
    DWORD ui_thread_id_ = 0;
    std::thread thread_;
    std::exception_ptr ui_failure_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    std::shared_ptr<gfx::Device> gfx_;            // 132mt1: device_ / context_ for the gfx layer
    std::shared_ptr<gfx::SwapChain> gfx_swap_;    // swap_
    std::shared_ptr<gfx::Texture> gfx_target_;    // target_
    std::string backend_;
    unsigned width_ = 0, height_ = 0;
    float render_scale_ = 1.0F;
    HostWindowOptions::SwapEffect swap_effect_ = HostWindowOptions::SwapEffect::flip_discard;
    unsigned frame_latency_ = 1;
    bool waitable_ = false;
    UINT swap_flags_ = 0;
    HANDLE latency_wait_ = nullptr;
    std::string swap_description_;
    std::uint64_t generation_ = 0;
    HRESULT lost_reason_ = S_OK;

    std::atomic<bool> focused_{false}, minimized_{false}, close_{false}, fullscreen_{false}, hotkeys_{true}, keep_hidden_{false},
        focus_simulated_{false}, dpi_test_{false};
    std::atomic<unsigned> dpi_{96};
    bool log_shutdown_ = false;
    std::atomic<bool> mouse_wanted_{false}, mouse_captured_{false};
    std::atomic<int> mouse_dx_{0}, mouse_dy_{0};
    std::atomic<std::uint64_t> size_serial_{0};
    mutable std::mutex mutex_;
    std::vector<HostEvent> events_;
    std::function<void(bool)> fullscreen_changed_;
    WINDOWPLACEMENT windowed_{sizeof(WINDOWPLACEMENT)};
};

// The process's working set and private bytes, for the hand-off lines.
std::string process_memory_line();

// Port-only (checkpoint 132win5): the monitors' work areas (screen coordinates), and a saved windowed rectangle
// made usable on them: nullopt (the default placement) when its size is out of range (under 160 x 120 or over
// 16384) or its title strip (the top 32 rows) is not at least 64 x 16 pixels on one work area (a monitor that
// was removed or moved); a rectangle larger than the work area its title strip is most on shrinks to fit it
// and moves inside it; otherwise it is kept as saved.
std::vector<WindowRect> monitor_work_areas();
std::optional<WindowRect> fit_saved_window(const WindowRect& saved, const std::vector<WindowRect>& work_areas);

// Checkpoint 132win14 (port-only HiDPI): the saved window= placement in physical pixels. Builds since 132win14 write it
// in physical pixels with window_units=pixels (port_settings.hpp); a window= without that key was saved by a
// DPI-unaware build, whose coordinates Windows scaled to 96 DPI, so it is scaled by system_dpi / 96 once (migrated
// set; the next save writes pixels). nullopt when there is no window=.
std::optional<WindowRect> saved_window_pixels(const PortSettings& settings, unsigned system_dpi, bool& migrated);

}  // namespace omega
