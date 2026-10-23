#pragma once
// Port-only (checkpoint 132vt2, research/VITA.md "The play host"): host_window.hpp's HostWindow on the PS Vita. The
// handheld has one fixed 960 x 544 screen and no windows: the "game window" is the display the Vita app
// (vita_main.cpp) made (its gfx::Device and swap chain on vitaGL / GXM, unwrapped: the play makes its own render
// thread over it, as on the PC), handed over with set_vita_window_display before play_main runs. The members the play
// host, the front end and the loading screen use keep their meaning; the window-only ones (placement, fullscreen,
// DPI, mouse capture, hiding) are the fixed screen's answers. Nothing here reaches the game's state or clock.

#include "gfx_device.hpp"
#include "port_settings.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace omega {

using WindowRect = std::array<int, 4>;

inline constexpr const wchar_t* game_window_title = L"Syphon Filter: The Omega Strain";

struct HostWindowOptions {
    std::wstring title = game_window_title;
    int width = 1280, height = 896;
    bool outer_size = false;
    float render_scale = 1.0F;
    enum class SwapEffect : std::uint8_t { flip_discard, flip_sequential, discard };
    SwapEffect swap_effect = SwapEffect::flip_discard;
    unsigned frame_latency = 1;
    bool waitable = false;
    bool dpi_scaled = false;
    unsigned dpi_override = 0;
};

inline std::string enable_per_monitor_dpi_awareness() { return "fixed screen"; }
inline std::string dpi_awareness_name() { return "fixed screen"; }
inline unsigned system_dpi() { return 96; }

struct HostEvent {
    enum class Type : std::uint8_t { key, wheel };
    Type type{};
    int value = 0;  // key: the win32_vita.hpp virtual key; wheel: the delta (never on the handheld)
};

enum class PresentResult : std::uint8_t { ok, occluded, device_lost };

// The Vita app's display (vita_main.cpp): the unwrapped device and swap chain, the screen size and an optional hook
// run after each present (the app's frame overlay / pacing bookkeeping).
struct VitaWindowDisplay {
    gfx::Display display;
    unsigned width = 960, height = 544;
    std::function<void()> presented;
};
void set_vita_window_display(VitaWindowDisplay display);

class HostWindow {
public:
    explicit HostWindow(const HostWindowOptions& options = {});
    ~HostWindow();
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // The window's handle for win32_vita.hpp's PostMessageW (the window itself).
    void* handle() const { return const_cast<HostWindow*>(this); }

    const std::string& backend() const { return backend_; }
    const std::shared_ptr<gfx::Device>& gfx() const { return gfx_; }
    gfx::Display scene_display() const { return {gfx_, gfx_swap_, backend_, false}; }
    const gfx::Texture* back_buffer();
    std::uint64_t device_generation() const { return 0; }

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    void drop_target() {}
    bool sync_buffers() { return false; }
    PresentResult present(unsigned sync_interval = 1);
    void recover_device(const std::string& why);
    bool capture(const std::filesystem::path& path);

    bool focused() const { return focused_; }
    bool minimized() const { return minimized_; }
    bool close_requested() const { return close_; }
    void request_close() { close_ = true; }
    void simulate_minimized(bool on) { minimized_ = on; }
    void simulate_focus(bool on) {
        focus_simulated_ = true;
        focused_ = on;
    }
    bool focus_simulated() const { return focus_simulated_; }
    void simulate_alt_enter() {}
    unsigned dpi() const { return 96; }
    std::uint64_t size_serial() const { return 0; }
    std::vector<HostEvent> take_events();
    void clear_events();
    void post_key(int key);  // win32_vita.hpp's PostMessageW (WM_KEYDOWN)

    void show(bool) {}
    void keep_hidden(bool on) { keep_hidden_ = on; }
    bool kept_hidden() const { return keep_hidden_; }
    void hide() {}
    bool visible() const { return true; }
    void set_title(const std::wstring&) {}
    void set_outer_size(int, int) {}
    WindowRect rect() const { return {0, 0, int(width_), int(height_)}; }
    WindowRect windowed_rect() const { return rect(); }
    void set_windowed_rect(const WindowRect&) {}
    std::array<unsigned, 2> client_size() const { return {width_, height_}; }
    void set_render_scale(float) {}  // the screen's buffers are its pixels
    float render_scale() const { return 1.0F; }
    std::array<unsigned, 2> buffer_size() const { return {width_, height_}; }
    void mask_bars() { mask_bars_vita(); }
    const std::string& swap_description() const { return swap_description_; }
    double wait_frame_latency(unsigned = 1000) { return 0.0; }

    bool fullscreen() const { return true; }
    void set_fullscreen(bool) {}
    void toggle_fullscreen() {}
    void on_fullscreen_change(std::function<void(bool)>) {}
    void allow_hotkeys(bool) {}

    void want_mouse_capture(bool) {}
    bool mouse_captured() const { return false; }
    std::array<int, 2> take_mouse_delta() { return {0, 0}; }
    void simulate_mouse_motion(int, int) {}

    void log_handoff(const std::string& where) const;
    void log_shutdown(bool on) { log_shutdown_ = on; }

private:
    void mask_bars_vita();

    std::shared_ptr<gfx::Device> gfx_;
    std::shared_ptr<gfx::SwapChain> gfx_swap_;
    std::shared_ptr<gfx::Texture> back_, capture_staging_;
    std::function<void()> presented_;
    std::string backend_, swap_description_;
    unsigned width_ = 960, height_ = 544;
    std::atomic<bool> focused_{true}, minimized_{false}, close_{false}, keep_hidden_{false}, focus_simulated_{false};
    bool log_shutdown_ = false;
    std::uint64_t presents_ = 0;
    mutable std::mutex mutex_;
    std::vector<HostEvent> events_;
};

std::string process_memory_line();

inline std::vector<WindowRect> monitor_work_areas() { return {{0, 0, 960, 544}}; }
inline std::optional<WindowRect> fit_saved_window(const WindowRect&, const std::vector<WindowRect>&) { return std::nullopt; }
inline std::optional<WindowRect> saved_window_pixels(const PortSettings&, unsigned, bool& migrated) {
    migrated = false;
    return std::nullopt;
}

}  // namespace omega
