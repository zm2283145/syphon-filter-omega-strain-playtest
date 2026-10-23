#include "disc.hpp"
#include "texture.hpp"
#include "windows_hash.hpp"
#include "native_paths.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::runtime_error win_error(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed: " + std::to_string(GetLastError()));
}

struct State {
    omega::Texture texture;
    std::vector<std::uint8_t> pixels;
    std::wstring name;
    std::filesystem::path output;
    bool fit{true};
    bool checker{true};
    bool capture{};
    bool captured{};
    double scale{1};
    std::string error;
};

double fit_scale(int width, int height, const omega::Texture& texture) {
    return std::clamp(std::min(double(width) / texture.width, double(height) / texture.height), 0.125, 8.0);
}

void paint(HWND window, HDC dc, const State& state) {
    RECT client{};
    if (!GetClientRect(window, &client)) throw win_error("GetClientRect");
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return;
    SetDCBrushColor(dc, RGB(24, 27, 32));
    if (!FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)))) throw win_error("FillRect");
    const auto font = GetStockObject(DEFAULT_GUI_FONT);
    const auto previous = SelectObject(dc, font);
    if (!previous || previous == HGDI_ERROR) throw win_error("Select font");
    struct RestoreFont {
        HDC dc;
        HGDIOBJ old;
        ~RestoreFont() { SelectObject(dc, old); }
    } restore{dc, previous};
    SetTextColor(dc, RGB(230, 233, 239));
    SetBkMode(dc, TRANSPARENT);
    RECT title{20, 14, width - 20, 38};
    const std::wstring heading = L"Native asset research - " + state.name;
    if (width > 40 && !DrawTextW(dc, heading.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS))
        throw win_error("Draw title");
    const std::wstring info = std::to_wstring(state.texture.width) + L" x " + std::to_wstring(state.texture.height)
        + L" | TDX v5 palette | verified user ISO | not a playable game";
    RECT details{20, 40, width - 20, 64};
    if (width > 40 && !DrawTextW(dc, info.c_str(), -1, &details, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS))
        throw win_error("Draw details");
    const int available_width = width - 40;
    const int available_height = height - 112;
    if (available_width > 0 && available_height > 0) {
        const int saved = SaveDC(dc);
        if (!saved) throw win_error("Save image drawing state");
        struct RestoreClip {
            HDC dc;
            int saved;
            ~RestoreClip() { RestoreDC(dc, saved); }
        } clip{dc, saved};
        if (IntersectClipRect(dc, 20, 76, width - 20, height - 36) == ERROR)
            throw win_error("Clip image viewport");
        const double fit = fit_scale(available_width, available_height, state.texture);
        const double scale = state.fit ? fit : state.scale;
        const int drawn_width = std::max(1, static_cast<int>(std::round(state.texture.width * scale)));
        const int drawn_height = std::max(1, static_cast<int>(std::round(state.texture.height * scale)));
        const int x = (width - drawn_width) / 2;
        const int y = 76 + (available_height - drawn_height) / 2;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(state.texture.width);
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(state.texture.height);
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        if (!SetStretchBltMode(dc, COLORONCOLOR)) throw win_error("SetStretchBltMode");
        const auto drawn = StretchDIBits(dc, x, y, drawn_width, drawn_height, 0, 0,
                                         state.texture.width, state.texture.height, state.pixels.data(),
                                         &bitmap, DIB_RGB_COLORS, SRCCOPY);
        if (drawn == 0 || drawn == GDI_ERROR) throw win_error("StretchDIBits");
    }
    RECT controls{20, height - 28, width - 20, height - 8};
    const wchar_t* hint = L"Wheel: zoom    F: fit    1: actual pixels    A: alpha background    Esc: close";
    if (width > 40 && height > 40
        && !DrawTextW(dc, hint, -1, &controls, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS))
        throw win_error("Draw controls");
}

struct CaptureSurface {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* pixels{};
    CaptureSurface(int width, int height) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw win_error("Create capture DC");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) {
            const auto error = win_error("Create capture bitmap");
            DeleteDC(dc);
            throw error;
        }
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            const auto error = win_error("Select capture bitmap");
            DeleteObject(bitmap);
            DeleteDC(dc);
            throw error;
        }
    }
    ~CaptureSurface() {
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    CaptureSurface(const CaptureSurface&) = delete;
    CaptureSurface& operator=(const CaptureSurface&) = delete;
};

void capture(HWND window, State& state) {
    RECT rectangle{};
    if (!GetClientRect(window, &rectangle)) throw win_error("Read capture bounds");
    if (rectangle.right <= 0 || rectangle.bottom <= 0 || rectangle.right > 4096 || rectangle.bottom > 4096)
        throw std::runtime_error("Window capture dimensions are outside the supported range");
    CaptureSurface surface(rectangle.right, rectangle.bottom);
    if (!PrintWindow(window, surface.dc, PW_CLIENTONLY)) throw win_error("PrintWindow");
    if (!GdiFlush()) throw win_error("Flush capture drawing");
    if (!state.error.empty()) throw std::runtime_error(state.error);
    const auto count = std::size_t(rectangle.right) * rectangle.bottom * 4;
    omega::write_bmp(state.output / "window_capture.bmp", rectangle.right, rectangle.bottom,
                     {static_cast<const std::uint8_t*>(surface.pixels), count});
    state.captured = true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    try {
        if (!state) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT region{};
            const auto dc = BeginPaint(window, &region);
            if (!dc) throw win_error("BeginPaint");
            struct End {
                HWND window;
                PAINTSTRUCT* region;
                ~End() { EndPaint(window, region); }
            } end{window, &region};
            paint(window, dc, *state);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint(window, reinterpret_cast<HDC>(wparam), *state);
            if (!GdiFlush()) throw win_error("Flush window printing");
            return 0;
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* sizes = reinterpret_cast<MINMAXINFO*>(lparam);
            sizes->ptMinTrackSize = {360, 260};
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (state->fit) {
                RECT client{};
                if (!GetClientRect(window, &client)) throw win_error("Read zoom bounds");
                if (client.right <= 40 || client.bottom <= 112) return 0;
                state->scale = fit_scale(client.right - 40, client.bottom - 112, state->texture);
            }
            state->fit = false;
            state->scale = std::clamp(state->scale * std::pow(1.25, double(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA), 0.125, 8.0);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) DestroyWindow(window);
            else {
                if (wparam == 'F') state->fit = true;
                if (wparam == '1') { state->fit = false; state->scale = 1; }
                if (wparam == 'A') {
                    state->checker = !state->checker;
                    state->pixels = omega::composite_bgra(state->texture, state->checker);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_TIMER:
            if (wparam == 1 && state->capture) {
                KillTimer(window, 1);
                capture(window, *state);
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(state->error.empty() ? 0 : 1);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    } catch (const std::exception& error) {
        if (state) state->error = error.what();
        DestroyWindow(window);
        return 0;
    }
}

void show(State& state) {
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"OmegaAssetResearchWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) throw win_error("RegisterClass");
    RECT size{0, 0, 1000, 740};
    if (!AdjustWindowRect(&size, WS_OVERLAPPEDWINDOW, FALSE)) throw win_error("AdjustWindowRect");
    const auto window = CreateWindowExW(0, class_name, L"Omega Strain - native asset research (not gameplay)",
                                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                        size.right - size.left, size.bottom - size.top,
                                        nullptr, nullptr, instance, &state);
    if (!window) throw win_error("CreateWindow");
    ShowWindow(window, state.capture ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    UpdateWindow(window);
    if (!state.error.empty()) throw std::runtime_error(state.error);
    if (state.capture && !SetTimer(window, 1, 350, nullptr)) {
        const auto error = win_error("Set capture timer");
        DestroyWindow(window);
        throw error;
    }
    MSG message{};
    for (;;) {
        const int result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) break;
        if (result < 0) {
            const auto error = win_error("GetMessage");
            DestroyWindow(window);
            throw error;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnregisterClassW(class_name, instance);
    if (!state.error.empty()) throw std::runtime_error(state.error);
    if (state.capture && !state.captured) throw std::runtime_error("Window closed before capture completed");
}

}

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::filesystem::path iso;
        std::string archive = "LOADING.HOG";
        std::string mission = "BELARUS1";
        std::string texture_name;
        bool export_only = false;
        bool capture_window = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            if (argument == L"--help") {
                std::cout << "Usage: omega_assets --iso <USA ISO> [--mission TORONTO1|BELARUS1] [--archive <HOG name>] [--texture <TDX name>]\n"
                             "                    [--export-only | --capture]\n"
                             "TDX v5, single-frame, no mipmaps; bounded PSMT8 and direct-upload PSMT4.\n"
                             "This displays a decoded asset, not playable gameplay. Run from the repository root.\n";
                return 0;
            }
            if (argument == L"--iso" && i + 1 < argc && iso.empty()) iso = argv[++i];
            else if (argument == L"--mission" && i + 1 < argc) mission = omega::uppercase_asset(omega::asset_basename(argv[++i]));
            else if (argument == L"--archive" && i + 1 < argc) archive = omega::asset_basename(argv[++i]);
            else if (argument == L"--texture" && i + 1 < argc) texture_name = omega::asset_basename(argv[++i]);
            else if (argument == L"--export-only") export_only = true;
            else if (argument == L"--capture") capture_window = true;
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        if (iso.empty()) throw std::runtime_error("A user-supplied ISO is required");
        if (mission != "TORONTO1" && mission != "BELARUS1") throw std::runtime_error("Unsupported mission");
        if (texture_name.empty()) texture_name = mission + "BACKGROUND.TDX";
        if (export_only && capture_window) throw std::runtime_error("Choose export-only or capture, not both");
        if (!std::filesystem::is_regular_file("config/disc_us.json"))
            throw std::runtime_error("Run the viewer from the repository root");
        std::cout << "Verifying the complete ISO before extracting a real game texture...\n";
        auto stream = omega::open_verified_iso(iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256);
        omega::DiscImage disc(stream, OMEGA_ISO_SIZE);
        const auto raw = disc.archive_member({"GAMEDATA", mission, archive}, texture_name);
        State state;
        state.texture = omega::decode_tdx(raw);
        state.pixels = omega::composite_bgra(state.texture, true);
        state.name = std::wstring(texture_name.begin(), texture_name.end());
        state.output = std::filesystem::current_path() / "local" / "runtime";
        if (mission != "BELARUS1") state.output /= mission;
        state.output /= "texture";
        state.capture = capture_window;
        std::filesystem::create_directories(state.output);
        omega::write_bytes(state.output / "source.tdx", raw);
        omega::write_bytes(state.output / "texture.rgba", state.texture.rgba);
        omega::write_bmp(state.output / "texture.bmp", state.texture.width, state.texture.height, state.pixels);
        std::ofstream info(state.output / "texture.json", std::ios::binary | std::ios::trunc);
        if (!info) throw std::runtime_error("Cannot create texture report");
        info << "{\"archive\":" << std::quoted(archive) << ",\"texture\":" << std::quoted(texture_name)
             << ",\"width\":" << state.texture.width << ",\"height\":" << state.texture.height
             << ",\"uses_alpha\":" << (state.texture.uses_alpha ? "true" : "false")
             << ",\"scope\":\"Native TDX asset preview, not game rendering\"}\n";
        info.close();
        if (!info) throw std::runtime_error("Cannot finish texture report");
        std::cout << "Decoded " << texture_name << ": " << state.texture.width << " x " << state.texture.height
                  << ". Local outputs: " << state.output << '\n';
        if (!export_only) show(state);
        if (capture_window) std::cout << "Captured this viewer's client area and exited.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native asset viewer failed: " << error.what() << '\n';
        return 1;
    }
}
