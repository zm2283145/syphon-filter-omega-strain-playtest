// Port-only (132ps5c, research/PS5.md "Interfaces"): host_window_ps5.hpp's fixed-screen HostWindow and win32_ps5.hpp's
// PostMessageW. The window is the display ps5_main.cpp handed over (set_ps5_window_display): its back buffer, its
// present (eglSwapBuffers, paced on the vertical blank) and a BMP capture of it, as host_window.cpp's on the PC.
// Nothing here reaches the game's state or clock.

#include "host_window_ps5.hpp"
#include "win32_ps5.hpp"

#include "frame_fit.hpp"
#include "gfx_gl46.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace omega {

namespace {

std::mutex g_display_mutex;
std::optional<Ps5WindowDisplay> g_display;

}  // namespace

void set_ps5_window_display(Ps5WindowDisplay display) {
    std::lock_guard lock(g_display_mutex);
    g_display = std::move(display);
}

std::string process_memory_line() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    char text[160];
    std::snprintf(text, sizeof text, "max resident %ld KB; ", long(usage.ru_maxrss));
    return std::string(text) + gfx::gl46::stats_line();
}

HostWindow::HostWindow(const HostWindowOptions&) {
    std::lock_guard lock(g_display_mutex);
    if (!g_display || !g_display->display.device || !g_display->display.swap_chain)
        throw std::runtime_error("HostWindow (PS5): no display was handed over (set_ps5_window_display)");
    gfx_ = g_display->display.device;
    gfx_swap_ = g_display->display.swap_chain;
    backend_ = g_display->display.backend;
    width_ = g_display->width;
    height_ = g_display->height;
    presented_ = g_display->presented;
    swap_description_ = "PS5 EGL window surface " + std::to_string(width_) + "x" + std::to_string(height_);
}

HostWindow::~HostWindow() {
    if (log_shutdown_) std::cout << "Host window (port-only, PS5): released after " << presents_ << " presents\n";
}

const gfx::Texture* HostWindow::back_buffer() {
    if (!back_) back_ = gfx_swap_->buffer(true, "back buffer", "back buffer view");
    return back_.get();
}

PresentResult HostWindow::present(unsigned sync_interval) {
    mask_bars_ps5();
    gfx_swap_->present(sync_interval, "present");
    ++presents_;
    if (presented_) presented_();
    return PresentResult::ok;
}

// host_window.cpp's mask_bars: the 4:3 setting's bars painted black before the present.
void HostWindow::mask_bars_ps5() {
    if (!frame_masked()) return;
    const auto bars = frame_bars(width_, height_);
    if (bars.count == 0) return;
    gfx::Rect rects[2]{};
    for (unsigned i = 0; i < bars.count; ++i) {
        const auto& r = bars.rects[i];
        rects[i] = {int(r.x), int(r.y), int(r.x + r.width), int(r.y + r.height)};
    }
    const float black[]{0, 0, 0, 1};
    gfx::CommandList list;
    list.clear_rects(back_buffer(), black, rects, bars.count);
    gfx_->submit(list);
}

void HostWindow::recover_device(const std::string& why) {
    throw std::runtime_error("HostWindow (PS5): the display cannot be re-created (" + why + ")");
}

bool HostWindow::capture(const std::filesystem::path& path) {
    mask_bars_ps5();
    if (!capture_staging_) {
        gfx::TextureDesc td;
        td.width = width_;
        td.height = height_;
        td.format = gfx::Format::bgra8;
        td.usage = gfx::Usage::staging;
        capture_staging_ = gfx_->create_texture(td, nullptr, {});
    }
    gfx::CommandList list;
    list.copy(capture_staging_.get(), back_buffer());
    gfx_->submit(list);
    const unsigned w = width_, h = height_, row = (w * 3 + 3) & ~3U;
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
    // The staging texture is BGRA (the swap chain's format): bytes B, G, R as the BMP stores them.
    gfx_->read(*capture_staging_, 0, [&](const std::uint8_t* data, unsigned pitch) {
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const std::uint8_t* p = data + std::size_t(y) * pitch + x * 4;
                std::uint8_t* q = bmp.data() + 54 + std::size_t(h - 1 - y) * row + x * 3;
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
            }
    }, "window capture read");
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bmp.data()), std::streamsize(bmp.size()));
    return bool(out);
}

std::vector<HostEvent> HostWindow::take_events() {
    std::lock_guard lock(mutex_);
    std::vector<HostEvent> out;
    out.swap(events_);
    return out;
}

void HostWindow::clear_events() {
    std::lock_guard lock(mutex_);
    events_.clear();
}

void HostWindow::post_key(int key) {
    std::lock_guard lock(mutex_);
    if (events_.size() < 256) events_.push_back({HostEvent::Type::key, key});
}

void HostWindow::log_handoff(const std::string& where) const {
    std::cout << "Host window (port-only, PS5): " << where << ": the " << width_ << "x" << height_ << " screen, "
              << presents_ << " presents, " << process_memory_line() << "\n";
}

}  // namespace omega

bool PostMessageW(HWND window, UINT message, WPARAM w, LPARAM) {
    auto* host = static_cast<omega::HostWindow*>(window);
    if (!host) return false;
    if (message == WM_SYSKEYDOWN && w == WPARAM(VK_F4)) host->request_close();
    else if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) host->post_key(int(w));
    else return false;
    return true;
}