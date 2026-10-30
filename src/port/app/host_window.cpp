#include "host_window.hpp"
#include "device_lost.hpp"
#include "frame_fit.hpp"
#include "gfx_d3d11.hpp"
#include "gfx_pc_backend.hpp"
#ifdef OMEGA_PC_VULKAN
#include "gfx_vulkan.hpp"
#endif
#include "mouse_look.hpp"
#include "texture.hpp"

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace omega {

namespace {

constexpr UINT run_message = WM_APP + 0x132;
constexpr UINT capture_message = WM_APP + 0x133;   // re-evaluates the mouse capture (132win7)
constexpr UINT pause_command = 0x1320;
constexpr const wchar_t* window_class = L"OmegaGameWindow";

// Port-only (132win3): a device loss throws DeviceLost (device_lost.hpp); the owner of the frame recovers.
void check(HRESULT hr, const char* what) { check_device(hr, what); }

LRESULT CALLBACK host_proc(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    if (auto* host = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA)))
        return host->handle_message(w, m, p, l);
    return DefWindowProcW(w, m, p, l);
}

}  // namespace

std::string dpi_awareness_name() {
    const auto c = GetThreadDpiAwarenessContext();
    if (AreDpiAwarenessContextsEqual(c, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return "per-monitor v2";
    switch (GetAwarenessFromDpiAwarenessContext(c)) {
        case DPI_AWARENESS_PER_MONITOR_AWARE: return "per-monitor";
        case DPI_AWARENESS_SYSTEM_AWARE: return "system";
        case DPI_AWARENESS_UNAWARE: return "unaware";
        default: return "unknown";
    }
}

// Checkpoint 132win14 (port-only HiDPI). Looked up at run time so an older Windows 10 still starts (v2 needs 1703).
std::string enable_per_monitor_dpi_awareness() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set = user32 ? reinterpret_cast<SetContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (set && !set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    if (!set) SetProcessDPIAware();
    return dpi_awareness_name();
}

unsigned system_dpi() { return GetDpiForSystem(); }

int scale_for_dpi(int v, unsigned dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

std::array<int, 2> window_frame_extra(unsigned dpi) {
    RECT r{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return {r.right - r.left, r.bottom - r.top};
}

HostWindowSize host_window_size(const HostWindowOptions& o, unsigned dpi, const WindowRect* work_area) {
    HostWindowSize s;
    const auto frame96 = window_frame_extra(96), frame = window_frame_extra(dpi);
    // The client size at 96 DPI: given, or the outer size less the 96-DPI frame (the window an unaware build made).
    int cw = o.outer_size ? o.width - frame96[0] : o.width, ch = o.outer_size ? o.height - frame96[1] : o.height;
    cw = std::max(cw, 1);
    ch = std::max(ch, 1);
    if (o.dpi_scaled && dpi != 96) {
        cw = scale_for_dpi(cw, dpi);
        ch = scale_for_dpi(ch, dpi);
    }
    if (o.dpi_scaled && work_area) {
        const int aw = (*work_area)[2] - (*work_area)[0] - frame[0], ah = (*work_area)[3] - (*work_area)[1] - frame[1];
        if (aw > 0 && ah > 0 && (cw > aw || ch > ah)) {
            const double k = std::min(double(aw) / cw, double(ah) / ch);  // the aspect kept
            cw = std::max(1, static_cast<int>(cw * k));
            ch = std::max(1, static_cast<int>(ch * k));
            s.fitted = true;
        }
    }
    s.client_width = cw;
    s.client_height = ch;
    s.outer_width = cw + frame[0];
    s.outer_height = ch + frame[1];
    return s;
}

std::optional<WindowRect> saved_window_pixels(const PortSettings& settings, unsigned dpi, bool& migrated) {
    migrated = false;
    if (!settings.window) return std::nullopt;
    WindowRect r = *settings.window;
    if (window_units_pixels(settings) || dpi == 96) return r;
    for (auto& v : r) v = scale_for_dpi(v, dpi);
    migrated = true;
    return r;
}

std::string process_memory_line() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    DWORD handles = 0;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c))
        return "memory unavailable";
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    return "working set " + std::to_string(c.WorkingSetSize / 1024) + " KB, private " +
           std::to_string(c.PrivateUsage / 1024) + " KB, handles " + std::to_string(handles);
}

HostWindow::HostWindow(const HostWindowOptions& options) {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    thread_ = std::thread([&, options] {
        try {
            ui_thread_id_ = GetCurrentThreadId();
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW type{};
            type.hInstance = instance;
            type.lpfnWndProc = host_proc;
            type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
            type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            type.lpszClassName = window_class;
            RegisterClassW(&type);  // already registered by an earlier host: CreateWindow still works
            const auto first = host_window_size(options, 96, nullptr);
            hwnd_ = CreateWindowExW(0, window_class, options.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                    CW_USEDEFAULT, first.outer_width, first.outer_height, nullptr,
                                    nullptr, instance, this);
            if (!hwnd_) throw std::runtime_error("Cannot create the game window");
            if (HMENU menu = GetSystemMenu(hwnd_, FALSE)) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, pause_command, L"Pause application\tPause/Break");
            }
            // Checkpoint 132win14 (port-only HiDPI): the size at the DPI of the monitor the window was placed on (the
            // window is not shown yet). An unaware process's window is always 96 DPI, so nothing changes there.
            dpi_ = options.dpi_override ? options.dpi_override : std::max(96u, GetDpiForWindow(hwnd_));
            MONITORINFO monitor{sizeof monitor};
            const bool have_monitor = GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
            const WindowRect work{monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom};
            const auto size = host_window_size(options, dpi_, have_monitor ? &work : nullptr);
            RECT now{};
            GetWindowRect(hwnd_, &now);
            if (size.outer_width != now.right - now.left || size.outer_height != now.bottom - now.top) {
                int x = now.left, y = now.top;
                if (options.dpi_scaled && have_monitor) {  // kept on the work area after growing
                    x = std::clamp(x, work[0], std::max(work[0], work[2] - size.outer_width));
                    y = std::clamp(y, work[1], std::max(work[1], work[3] - size.outer_height));
                }
                SetWindowPos(hwnd_, nullptr, x, y, size.outer_width, size.outer_height,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            }
            // Port-only mouse look (132win7): the mouse's relative counts as WM_INPUT while the window is in the
            // foreground (the legacy mouse messages are kept).
            RAWINPUTDEVICE mouse{};
            mouse.usUsagePage = 0x01;  // generic desktop
            mouse.usUsage = 0x02;      // mouse
            mouse.hwndTarget = hwnd_;
            if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse))
                std::cerr << "Host window (port-only): no raw mouse input (" << GetLastError() << "); mouse look is off\n";
        } catch (...) {
            ui_failure_ = std::current_exception();
        }
        {
            std::lock_guard lock(m);
            ready = true;
        }
        cv.notify_one();
        if (!hwnd_) return;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return ready; });
    }
    if (ui_failure_) {
        thread_.join();
        std::rethrow_exception(ui_failure_);
    }
    render_scale_ = options.render_scale;
    swap_effect_ = options.swap_effect;
    frame_latency_ = std::clamp(options.frame_latency, 1u, 16u);
    waitable_ = options.waitable;
    try {
        create_device();
    } catch (...) {
        on_ui([this] { DestroyWindow(hwnd_); });
        thread_.join();
        throw;
    }
}

HostWindow::~HostWindow() {
    mouse_wanted_ = false;
    const unsigned long references = release_device();
    char handle[32];
    std::snprintf(handle, sizeof handle, "%p", static_cast<void*>(hwnd_));
    if (hwnd_) on_ui([this] { DestroyWindow(hwnd_); });
    if (thread_.joinable()) thread_.join();
    if (log_shutdown_)
        std::cout << "Host window (port-only): shut down: window " << handle << " destroyed, its thread joined, the device ("
                  << generation_ + 1 << " made) released with " << references << " references left"
                  << (references ? " (a renderer outlived it)" : " (balanced)") << ", " << process_memory_line() << "\n";
}

void HostWindow::on_ui(const std::function<void()>& f) const {
    if (GetCurrentThreadId() == ui_thread_id_) {
        f();
        return;
    }
    SendMessageW(hwnd_, run_message, 0, reinterpret_cast<LPARAM>(&f));
}

unsigned long HostWindow::release_device() {
    if (latency_wait_) CloseHandle(latency_wait_);
    latency_wait_ = nullptr;
    if (context_) context_->ClearState();
    gfx_target_.reset();
    gfx_swap_.reset();
    if (vulkan_) {   // 132vk1: the Vulkan device's references left (the gfx objects that still hold it)
        const long left = gfx_ ? gfx_.use_count() - 1 : 0;
        gfx_.reset();
        vulkan_ = false;
        return static_cast<unsigned long>(left);
    }
    gfx_.reset();
    target_.Reset();
    swap_.Reset();
    if (context_) context_->Flush();
    context_.Reset();
    return device_.Reset();
}

void HostWindow::create_device() {
    const auto size = buffer_size();
#ifdef OMEGA_PC_VULKAN
    // Port-only (checkpoint 132vk1, research/VULKAN.md): the Vulkan backend when chosen (--renderer vulkan, renderer=);
    // a machine without a usable Vulkan 1.3 device falls back to Direct3D 11 below.
    if (gfx::pc_backend() == gfx::PcBackend::vulkan) {
        try {
            auto made = gfx::vulkan::create_window_display(hwnd_, std::max(1u, size[0]), std::max(1u, size[1]), frame_latency_);
            gfx_ = made.display.device;
            gfx_swap_ = made.display.swap_chain;
            backend_ = made.display.backend;
            swap_description_ = made.swap_description;
            gfx_swap_->size(width_, height_, "Vulkan back buffer size");
            vulkan_ = true;
            return;
        } catch (const DeviceLost&) {
            throw;
        } catch (const std::exception& e) {
            gfx::note_pc_vulkan_failure(e.what());
            std::cout << "Host window (port-only): Vulkan unavailable (" << e.what() << "); using Direct3D 11\n";
        }
    }
#endif
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Width = std::max(1u, size[0]);
    d.BufferDesc.Height = std::max(1u, size[1]);
    d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd_;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = E_FAIL;
    bool warp = false;
    const DXGI_SWAP_EFFECT preferred = swap_effect_ == HostWindowOptions::SwapEffect::flip_sequential ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                                       : swap_effect_ == HostWindowOptions::SwapEffect::discard       ? DXGI_SWAP_EFFECT_DISCARD
                                                                                                      : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    for (const auto swap : {preferred, DXGI_SWAP_EFFECT_DISCARD}) {
        d.SwapEffect = swap;
        const bool flip = swap == DXGI_SWAP_EFFECT_FLIP_DISCARD || swap == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.BufferCount = flip ? 2 : 1;
        // 132win10 diagnostic: the frame-latency waitable object (flip model only).
        d.Flags = flip && waitable_ ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
        for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                               D3D11_SDK_VERSION, &d, swap_.ReleaseAndGetAddressOf(),
                                               device_.ReleaseAndGetAddressOf(), &got, context_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr)) {
                warp = type == D3D_DRIVER_TYPE_WARP;
                break;
            }
        }
        if (SUCCEEDED(hr)) break;
    }
    check(hr, "D3D11CreateDeviceAndSwapChain");
    backend_ = warp ? "Direct3D 11 WARP" : "Direct3D 11 hardware";
    // Port-only (132win3, frame pacing): at most one frame queued ahead of the display, so a present with a
    // sync interval holds the loop to the vertical blank right away (the PS2 draws one frame ahead at most).
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi1;
    if (SUCCEEDED(device_.As(&dxgi1))) dxgi1->SetMaximumFrameLatency(frame_latency_);
    swap_flags_ = d.Flags;
    if (swap_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        Microsoft::WRL::ComPtr<IDXGISwapChain2> swap2;
        if (SUCCEEDED(swap_.As(&swap2))) {
            swap2->SetMaximumFrameLatency(frame_latency_);
            latency_wait_ = swap2->GetFrameLatencyWaitableObject();
        }
    }
    swap_description_ = std::string(d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD      ? "flip discard"
                                     : d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "flip sequential"
                                                                                        : "blt discard") +
                        ", " + std::to_string(d.BufferCount) + " buffer" + (d.BufferCount == 1 ? "" : "s") + ", frame latency " +
                        std::to_string(frame_latency_) + (latency_wait_ ? ", waitable" : "");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf()))) {
        DXGI_ADAPTER_DESC a{};
        if (SUCCEEDED(adapter->GetDesc(&a))) {
            std::string name;
            for (const wchar_t* c = a.Description; *c; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
            backend_ += " (" + name + ")";
        }
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
            factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    DXGI_SWAP_CHAIN_DESC got_desc{};
    swap_->GetDesc(&got_desc);
    width_ = got_desc.BufferDesc.Width;
    height_ = got_desc.BufferDesc.Height;
    gfx_ = gfx::d3d11::wrap_device(device_, context_, backend_);   // 132mt1: the gfx layer's view of them
    gfx_swap_ = gfx::d3d11::wrap_swap_chain(gfx_, swap_);
}

const gfx::Texture* HostWindow::back_buffer() {
    if (vulkan_) {   // 132vk1: the swap chain's back buffer (its size is width() x height())
        if (!gfx_target_) gfx_target_ = gfx_swap_->buffer(true, "Vulkan back buffer", "Vulkan back buffer target");
        return gfx_target_.get();
    }
    ID3D11RenderTargetView* view = target();
    if (!gfx_target_) gfx_target_ = gfx::d3d11::wrap_target(gfx_, view);
    return gfx_target_.get();
}

ID3D11RenderTargetView* HostWindow::target() {
    if (vulkan_) {   // 132vk1: no D3D11 view; the drawing code uses back_buffer()
        back_buffer();
        return nullptr;
    }
    if (!target_) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        check(swap_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "GetBuffer");
        check(device_->CreateRenderTargetView(back.Get(), nullptr, target_.GetAddressOf()), "CreateRenderTargetView");
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        width_ = d.Width;
        height_ = d.Height;
    }
    return target_.Get();
}

void HostWindow::drop_target() {
    if (vulkan_) {
        gfx_target_.reset();
        return;
    }
    if (context_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->ClearState();
        context_->Flush();
    }
    gfx_target_.reset();
    target_.Reset();
}

bool HostWindow::sync_buffers() {
    if (minimized_) return false;
    const auto [w, h] = buffer_size();
    if (w == 0 || h == 0 || (w == width_ && h == height_)) return false;
    drop_target();
    if (vulkan_)
        gfx_swap_->resize(w, h, "Resize the Vulkan back buffer");
    else
        check(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swap_flags_), "ResizeBuffers");
    width_ = w;
    height_ = h;
    return true;
}

PresentResult HostWindow::present(unsigned sync_interval) {
    if (device_lost_pending()) {  // 132win3: a draw of this frame saw the loss; the frame is dropped
        lost_reason_ = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        if (const char* forced = device_lost_forced())  // 132win4: the test hook's loss has its own label
            recover_device(std::string("forced mid-frame device loss, ") + forced + ", at " + device_lost_by());
        else
            recover_device(std::string("display device lost mid-frame (") + device_lost_by() + ")");
        return PresentResult::device_lost;
    }
    mask_bars();
    if (vulkan_) {   // 132vk1
        const auto r = gfx_swap_->present(sync_interval, "Present");
        if (r == gfx::SwapChain::Presented::occluded) return PresentResult::occluded;
        if (r == gfx::SwapChain::Presented::lost || device_lost_pending()) {
            lost_reason_ = DXGI_ERROR_DEVICE_REMOVED;
            recover_device("display device lost (Vulkan)");
            return PresentResult::device_lost;
        }
        return PresentResult::ok;
    }
    const HRESULT hr = device_result(swap_->Present(std::min(sync_interval, 4u), 0), "Present");
    if (hr == DXGI_STATUS_OCCLUDED) return PresentResult::occluded;
    if (device_lost_code(hr)) {
        lost_reason_ = device_ ? device_->GetDeviceRemovedReason() : hr;
        if (lost_reason_ == S_OK) lost_reason_ = hr;
        char code[16];
        std::snprintf(code, sizeof code, "%08lx", static_cast<unsigned long>(lost_reason_));
        recover_device(std::string("display device lost (reason ") + code + ")");
        return PresentResult::device_lost;
    }
    throw_if_failed(hr, "Present");
    return PresentResult::ok;
}

void HostWindow::recover_device(const std::string& why) {
    std::cout << "Host window (port-only): " << why << "; re-creating the device (generation " << generation_ + 1
              << ")\n";
    // A display-mode change or a driver reset can refuse a new device for a moment (132win3): a few tries.
    for (int attempt = 1;; ++attempt) {
        release_device();
        try {
            device_lost_recovered();
            create_device();
            break;
        } catch (const std::exception& e) {
            if (attempt >= 8) throw;
            std::cout << "Host window (port-only): device creation failed (" << e.what() << "); retrying\n";
            Sleep(250);
        }
    }
    ++generation_;
}

double HostWindow::wait_frame_latency(unsigned timeout_ms) {
    if (!latency_wait_) return 0.0;
    const auto begin = std::chrono::steady_clock::now();
    WaitForSingleObjectEx(latency_wait_, timeout_ms, TRUE);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void HostWindow::mask_bars() {
    if (vulkan_) {   // 132vk1: the same bars through the gfx layer (CommandList::clear_rects is D3D11.1 ClearView)
        if (!frame_masked() || !gfx_ || !gfx_target_) return;
        const auto bars = frame_bars(width_, height_);
        if (bars.count == 0) return;
        gfx::Rect rects[2]{};
        for (unsigned i = 0; i < bars.count && i < 2; ++i) {
            const auto& r = bars.rects[i];
            rects[i] = {int(r.x), int(r.y), int(r.x + r.width), int(r.y + r.height)};
        }
        const float black[]{0, 0, 0, 1};
        gfx::CommandList list;
        list.clear_rects(gfx_target_.get(), black, rects, bars.count);
        gfx_->submit(list);
        return;
    }
    if (!frame_masked() || !context_ || !target_) return;
    const auto bars = frame_bars(width_, height_);
    if (bars.count == 0) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context_.As(&context1))) return;
    D3D11_RECT rects[2]{};
    for (unsigned i = 0; i < bars.count; ++i) {
        const auto& r = bars.rects[i];
        rects[i] = {LONG(r.x), LONG(r.y), LONG(r.x + r.width), LONG(r.y + r.height)};
    }
    const float black[]{0, 0, 0, 1};
    context1->ClearView(target_.Get(), black, rects, bars.count);
}

bool HostWindow::capture(const std::filesystem::path& path) try {
    if (device_lost_pending()) return false;
    mask_bars();
    if (vulkan_) {   // 132vk1: the back buffer through a staging copy
        const auto back = gfx_swap_->buffer(false, "Get window capture buffer", nullptr);
        gfx::TextureDesc d = back->desc();
        d.usage = gfx::Usage::staging;
        d.bind = 0;
        gfx::TextureLabels labels;
        labels.texture = "Create window readback";
        const auto staging = gfx_->create_texture(d, nullptr, labels);
        gfx::CommandList list;
        list.copy(staging.get(), back.get());
        gfx_->submit(list);
        std::vector<std::uint8_t> pixels(std::size_t(d.width) * d.height * 4);
        gfx_->read(*staging, 0, [&](const std::uint8_t* data, unsigned pitch) {
            for (unsigned y = 0; y < d.height; ++y) std::memcpy(pixels.data() + std::size_t(y) * d.width * 4, data + std::size_t(y) * pitch, d.width * 4);
        }, "Read window frame");
        for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xff;
        std::filesystem::create_directories(path.parent_path());
        write_bmp(path, d.width, d.height, pixels);
        return true;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    check(swap_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "Get window capture buffer");
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    check(device_->CreateTexture2D(&d, nullptr, staging.GetAddressOf()), "Create window readback");
    context_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read window frame");
    std::vector<std::uint8_t> pixels(std::size_t(d.Width) * d.Height * 4);
    for (unsigned y = 0; y < d.Height; ++y)
        std::memcpy(pixels.data() + std::size_t(y) * d.Width * 4,
                    static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, d.Width * 4);
    context_->Unmap(staging.Get(), 0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xff;  // the swap chain's alpha is not shown
    std::filesystem::create_directories(path.parent_path());
    write_bmp(path, d.Width, d.Height, pixels);
    return true;
} catch (const DeviceLost&) {
    return false;
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

void HostWindow::show(bool activate) {
    if (keep_hidden_) return;
    on_ui([this, activate] {
        if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        else if (activate) SetForegroundWindow(hwnd_);
    });
}

void HostWindow::hide() {
    on_ui([this] { ShowWindow(hwnd_, SW_HIDE); });
}

bool HostWindow::visible() const { return IsWindowVisible(hwnd_) != FALSE; }

void HostWindow::set_title(const std::wstring& title) {
    on_ui([this, &title] {
        title_ = title;
        refresh_title();
    });
}

void HostWindow::set_application_paused(bool paused) {
    on_ui([this, paused] { apply_application_pause(paused); });
}

void HostWindow::apply_application_pause(bool paused) {
    if (application_paused_.exchange(paused) == paused) return;
    if (HMENU menu = GetSystemMenu(hwnd_, FALSE)) {
        ModifyMenuW(menu, pause_command, MF_BYCOMMAND | MF_STRING, pause_command,
                    paused ? L"Resume application\tPause/Break" : L"Pause application\tPause/Break");
        CheckMenuItem(menu, pause_command, MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
    }
    refresh_title();
    update_mouse_capture();
    std::cout << "Host window (port-only): application " << (paused ? "paused" : "resumed")
              << " (Pause/Break or the window system menu)\n";
}

void HostWindow::refresh_title() {
    const std::wstring shown = application_paused_ ? title_ + L" [PAUSED]" : title_;
    SetWindowTextW(hwnd_, shown.c_str());
}

void HostWindow::set_outer_size(int width, int height) {
    on_ui([this, width, height] {
        if (fullscreen_) apply_fullscreen(false);
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    });
}

void HostWindow::simulate_minimized(bool on) {
    minimized_ = on;
    if (!on) ++size_serial_;
}

void HostWindow::simulate_dpi_change(unsigned dpi, const WindowRect& suggested) {
    on_ui([this, dpi, &suggested] {
        RECT r{suggested[0], suggested[1], suggested[2], suggested[3]};
        std::cout << "Host window (port-only test): WM_DPICHANGED " << dpi << "\n";
        dpi_test_ = true;
        SendMessageW(hwnd_, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&r));
        dpi_test_ = false;
    });
}

void HostWindow::simulate_alt_enter() {
    on_ui([this] {
        const bool allowed = hotkeys_.exchange(true);
        std::cout << "Host window (port-only test): Alt+Enter (WM_SYSKEYDOWN VK_RETURN, Alt down)\n";
        SendMessageW(hwnd_, WM_SYSKEYDOWN, VK_RETURN, static_cast<LPARAM>(1 | (1 << 29)));
        hotkeys_ = allowed;
    });
}

namespace {
// GetWindowPlacement's rectangles are in workspace coordinates (the primary monitor's work area at 0, 0).
POINT workspace_origin() {
    MONITORINFO m{sizeof m};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &m);
    return {m.rcWork.left - m.rcMonitor.left, m.rcWork.top - m.rcMonitor.top};
}
}  // namespace

WindowRect HostWindow::windowed_rect() const {
    WindowRect out{};
    on_ui([&] {
        WINDOWPLACEMENT p{sizeof p};
        if (fullscreen_) p = windowed_;
        else GetWindowPlacement(hwnd_, &p);
        const POINT o = workspace_origin();
        const RECT& r = p.rcNormalPosition;
        out = {r.left + o.x, r.top + o.y, r.right + o.x, r.bottom + o.y};
    });
    return out;
}

void HostWindow::set_windowed_rect(const WindowRect& r) {
    on_ui([&] {
        if (fullscreen_) {
            const POINT o = workspace_origin();
            windowed_.rcNormalPosition = {r[0] - o.x, r[1] - o.y, r[2] - o.x, r[3] - o.y};
            return;
        }
        SetWindowPos(hwnd_, nullptr, r[0], r[1], r[2] - r[0], r[3] - r[1], SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    });
}

std::vector<WindowRect> monitor_work_areas() {
    std::vector<WindowRect> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM list) -> BOOL {
            MONITORINFO m{sizeof m};
            if (GetMonitorInfoW(monitor, &m))
                reinterpret_cast<std::vector<WindowRect>*>(list)->push_back(
                    {m.rcWork.left, m.rcWork.top, m.rcWork.right, m.rcWork.bottom});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

std::optional<WindowRect> fit_saved_window(const WindowRect& saved, const std::vector<WindowRect>& work_areas) {
    const long long w = static_cast<long long>(saved[2]) - saved[0], h = static_cast<long long>(saved[3]) - saved[1];
    if (w < 160 || h < 120 || w > 16384 || h > 16384) return std::nullopt;
    const WindowRect strip{saved[0], saved[1], saved[2], saved[1] + static_cast<int>(std::min<long long>(32, h))};
    const WindowRect* best = nullptr;
    long long best_area = 0;
    for (const auto& a : work_areas) {
        const long long iw = std::min(strip[2], a[2]) - static_cast<long long>(std::max(strip[0], a[0]));
        const long long ih = std::min(strip[3], a[3]) - static_cast<long long>(std::max(strip[1], a[1]));
        if (iw < 64 || ih < 16) continue;
        if (iw * ih > best_area) {
            best_area = iw * ih;
            best = &a;
        }
    }
    if (!best) return std::nullopt;
    const WindowRect& a = *best;
    const int aw = a[2] - a[0], ah = a[3] - a[1];
    if (w <= aw && h <= ah) return saved;
    const int fw = static_cast<int>(std::min<long long>(w, aw)), fh = static_cast<int>(std::min<long long>(h, ah));
    const int left = std::clamp(saved[0], a[0], a[2] - fw), top = std::clamp(saved[1], a[1], a[3] - fh);
    return WindowRect{left, top, left + fw, top + fh};
}

WindowRect HostWindow::rect() const {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    return {r.left, r.top, r.right, r.bottom};
}

std::array<unsigned, 2> HostWindow::buffer_size() const {
    const auto [w, h] = client_size();
    return {scaled_extent(w, render_scale_), scaled_extent(h, render_scale_)};
}

std::array<unsigned, 2> HostWindow::client_size() const {
    RECT r{};
    GetClientRect(hwnd_, &r);
    return {static_cast<unsigned>(std::max<LONG>(0, r.right - r.left)),
            static_cast<unsigned>(std::max<LONG>(0, r.bottom - r.top))};
}

void HostWindow::set_fullscreen(bool on) {
    on_ui([this, on] { apply_fullscreen(on); });
}

void HostWindow::on_fullscreen_change(std::function<void(bool)> callback) {
    on_ui([this, &callback] { fullscreen_changed_ = std::move(callback); });
}

// Borderless fullscreen (the window covers its monitor, no mode change): the windowed placement is kept to
// restore. An invisible window stays invisible either way (hidden runs and tests).
void HostWindow::apply_fullscreen(bool on) {
    if (on == fullscreen_) return;
    const bool shown = IsWindowVisible(hwnd_) != FALSE;
    const WindowRect placement = windowed_rect();  // kept (on) / restored (off)
    const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (on) {
        MONITORINFO monitor{sizeof monitor};
        windowed_.length = sizeof windowed_;
        if (!GetWindowPlacement(hwnd_, &windowed_) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(hwnd_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (shown ? 0U : SWP_NOACTIVATE | SWP_NOZORDER));
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        WINDOWPLACEMENT p = windowed_;
        if (!shown) p.showCmd = SW_HIDE;
        else if (p.showCmd == SW_SHOWMINIMIZED || p.showCmd == SW_MINIMIZE) p.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd_, &p);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (shown ? 0U : SWP_NOACTIVATE));
    }
    fullscreen_ = on;
    ++size_serial_;
    RECT now{};
    GetWindowRect(hwnd_, &now);
    char line[200];
    std::snprintf(line, sizeof line, "style 0x%08llx, outer rect %ld,%ld,%ld,%ld; windowed placement %d,%d,%d,%d %s",
                  static_cast<unsigned long long>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), now.left, now.top, now.right,
                  now.bottom, placement[0], placement[1], placement[2], placement[3], on ? "kept" : "restored");
    std::cout << "Host window (port-only): fullscreen " << (on ? "on" : "off") << " (" << line << (shown ? ")" : "; invisible)")
              << "\n";
    update_mouse_capture();
    if (fullscreen_changed_) fullscreen_changed_(on);
}

void HostWindow::want_mouse_capture(bool on) {
    if (mouse_wanted_.exchange(on) != on && hwnd_) PostMessageW(hwnd_, capture_message, 0, 0);
}

std::array<int, 2> HostWindow::take_mouse_delta() { return {mouse_dx_.exchange(0), mouse_dy_.exchange(0)}; }

void HostWindow::simulate_mouse_motion(int dx, int dy) {
    mouse_dx_ += dx;
    mouse_dy_ += dy;
}

// Captured: the cursor hidden over the client area and clipped to it (re-clipped after a move, a resize or a
// fullscreen toggle); released: unclipped, the arrow back and the uncounted motion dropped.
void HostWindow::update_mouse_capture() {
    const bool active = mouse_capture_active(mouse_wanted_, IsWindowVisible(hwnd_) && !keep_hidden_,
                                             focused_ && GetForegroundWindow() == hwnd_, minimized_ || IsIconic(hwnd_));
    if (active) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
        if (!mouse_captured_) {
            mouse_dx_ = 0;
            mouse_dy_ = 0;
            mouse_captured_ = true;
            SetCursor(nullptr);
            std::cout << "Host window (port-only): mouse captured (mouse look)\n";
        }
    } else if (mouse_captured_) {
        mouse_captured_ = false;
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
        mouse_dx_ = 0;
        mouse_dy_ = 0;
        std::cout << "Host window (port-only): mouse released\n";
    }
}

void HostWindow::log_handoff(const std::string& where) const {
    char handle[32];
    std::snprintf(handle, sizeof handle, "%p", static_cast<void*>(hwnd_));
    std::cout << "Host window (port-only): " << where << ": window " << handle << (visible() ? " (visible" : " (invisible")
              << (fullscreen_ ? ", fullscreen)" : ", windowed)") << ", device generation " << generation_ << ", "
              << process_memory_line() << ", DPI " << dpi_.load() << " (" << dpi_awareness_name() << ")\n";
}

LRESULT HostWindow::handle_message(HWND window, UINT message, WPARAM w, LPARAM l) {
    const auto push = [this](HostEvent::Type type, int value) {
        std::lock_guard lock(mutex_);
        if (events_.size() < 256) events_.push_back({type, value});
    };
    switch (message) {
        case run_message: (*reinterpret_cast<const std::function<void()>*>(l))(); return 0;
        case capture_message: update_mouse_capture(); return 0;
        case WM_ACTIVATE:
            focused_ = LOWORD(w) != WA_INACTIVE && !HIWORD(w);
            update_mouse_capture();
            break;
        case WM_SETFOCUS: focused_ = true; update_mouse_capture(); return 0;
        case WM_KILLFOCUS: focused_ = false; update_mouse_capture(); return 0;
        case WM_SIZE:
            minimized_ = w == SIZE_MINIMIZED;
            if (!minimized_) ++size_serial_;
            update_mouse_capture();
            return 0;
        case WM_MOVE: update_mouse_capture(); break;
        case WM_DPICHANGED: {
            // Checkpoint 132win14 (port-only HiDPI): the new monitor DPI. Windowed: the suggested rectangle (the same
            // size in DIPs on the new monitor); fullscreen: the monitor covered again; kept hidden: unchanged (fixed
            // capture sizes) unless a test simulates it. WM_SIZE then bumps the size serial (the buffers follow).
            const unsigned before = dpi_.exchange(HIWORD(w));
            const RECT* r = reinterpret_cast<const RECT*>(l);
            const char* action = "kept (hidden)";
            if (fullscreen_) {
                MONITORINFO monitor{sizeof monitor};
                if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                    const RECT& m = monitor.rcMonitor;
                    SetWindowPos(window, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
                }
                action = "fullscreen kept on its monitor";
            } else if (r && (!keep_hidden_ || dpi_test_)) {
                SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
                action = "resized to the suggested rectangle";
            }
            ++size_serial_;
            std::cout << "Host window (port-only): DPI " << before << " -> " << dpi_.load() << ", " << action << "\n";
            update_mouse_capture();
            return 0;
        }
        case WM_SHOWWINDOW: PostMessageW(window, capture_message, 0, 0); break;  // after the visibility changes
        case WM_SETCURSOR:
            if (mouse_captured_ && LOWORD(l) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_INPUT:
            if (mouse_captured_) {
                RAWINPUT raw{};
                UINT size = sizeof raw;
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                    raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    mouse_dx_ += raw.data.mouse.lLastX;
                    mouse_dy_ += raw.data.mouse.lLastY;
                }
            }
            break;  // DefWindowProc cleans the input up
        case WM_CLOSE: close_ = true; return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == pause_command) {
                apply_application_pause(!application_paused_);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (w == VK_PAUSE) {
                if (!(l & (1 << 30)) && hotkeys_) apply_application_pause(!application_paused_);
                return 0;
            }
            if (w == VK_F11) {
                if (!(l & (1 << 30)) && hotkeys_) apply_fullscreen(!fullscreen_);
                return 0;
            }
            push(HostEvent::Type::key, static_cast<int>(w));
            return 0;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN && (l & (1 << 29))) {
                if (!(l & (1 << 30)) && hotkeys_) apply_fullscreen(!fullscreen_);
                return 0;
            }
            if (w == VK_F4 && (l & (1 << 29))) close_ = true;
            if (w == VK_F10) push(HostEvent::Type::key, VK_F10);
            return 0;  // no system menu (Alt / F10) stealing the keyboard
        case WM_SYSKEYUP:
        case WM_SYSCHAR: return 0;
        case WM_MOUSEWHEEL: push(HostEvent::Type::wheel, GET_WHEEL_DELTA_WPARAM(w)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case WM_DESTROY:
            if (mouse_captured_.exchange(false)) ClipCursor(nullptr);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, w, l);
}

}  // namespace omega
