#include "launcher_mod_panel.hpp"

#include "image_files.hpp"
#include "mod_manager.hpp"
#include "port_settings.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace omega::launcher {
namespace fs = std::filesystem;
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(std::size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

enum : int {
    id_list = 100,
    id_details,
    id_header,
    id_status,
    id_toggle = 110,
    id_up,
    id_down,
    id_all,
    id_refresh,
    id_open,
    id_play = IDOK,
    id_close = IDCANCEL,
};

constexpr int client_width = 900, client_height = 620;

struct Panel {
    ModPanelOptions options;
    bool scripted = false;
    HWND window{}, list{}, details{}, header{}, status{};
    HWND toggle{}, up{}, down{}, all{}, refresh_button{}, open{}, play{}, close{};
    HFONT mono{}, ui{};
    mods::ModState state;
    std::optional<ModPanelResult> result;

    void log(const std::string& line) const {
        if (options.log) options.log(line);
        else std::cout << line << "\n";
    }
    std::optional<std::size_t> selected() const {
        const auto i = SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (i == LB_ERR || std::size_t(i) >= state.listing.size()) return std::nullopt;
        return std::size_t(i);
    }
    std::size_t valid_count() const {
        std::size_t n = 0;
        while (n < state.listing.size() && state.listing[n].manifest) ++n;
        return n;
    }
    void set_status(const std::string& text) const { SetWindowTextW(status, widen(text).c_str()); }

    void update_selection() const {
        const auto sel = selected();
        const bool valid = sel && state.listing[*sel].manifest;
        EnableWindow(toggle, valid);
        SetWindowTextW(toggle, valid && state.listing[*sel].enabled ? L"&Disable" : L"&Enable");
        EnableWindow(up, valid && *sel > 0);
        EnableWindow(down, valid && *sel + 1 < valid_count());
        std::string text;
        for (const auto& line : mods::mod_details(state, sel)) text += line + "\r\n";
        SetWindowTextW(details, widen(text).c_str());
    }

    void refresh(const std::optional<std::string>& keep) {
        const auto previous = selected();
        state = mods::read_mod_state(options.mods_folder, options.settings);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        std::optional<std::size_t> pick;
        for (std::size_t i = 0; i < state.listing.size(); ++i) {
            SendMessageW(list, LB_ADDSTRING, 0, LPARAM(widen(mods::mod_row_text(state.listing[i])).c_str()));
            if (keep && mods::upper(state.listing[i].folder) == mods::upper(*keep)) pick = i;
        }
        if (!pick && !state.listing.empty()) pick = std::min(previous.value_or(0), state.listing.size() - 1);
        if (pick) SendMessageW(list, LB_SETCURSEL, WPARAM(*pick), 0);
        std::size_t archives = 0;
        for (const auto& l : state.listing) archives += !l.archive.empty();
        const std::string head = "Mods folder " + utf8(state.folder) + "   " + std::to_string(state.listing.size()) + " mod(s) (" +
                                 std::to_string(archives) + " .zip), first wins, then the disc" +
                                 (state.mods_on ? "" : "   ALL MODS OFF (mods=0)");
        SetWindowTextW(header, widen(head).c_str());
        SetWindowTextW(all, state.mods_on ? L"&All mods off" : L"&All mods on");
        update_selection();
    }

    PortSettings read_settings() const {
        std::error_code error;
        if (!fs::exists(options.settings, error)) return {};
        return read_port_settings(options.settings);   // throws when malformed: nothing is written then
    }
    void write_settings(const PortSettings& s) const {
        std::error_code error;
        if (options.settings.has_parent_path()) fs::create_directories(options.settings.parent_path(), error);
        write_port_settings(options.settings, s);
    }

    void command(int id) {
        try {
            const auto sel = selected();
            const std::optional<std::string> folder = sel ? std::optional(state.listing[*sel].folder) : std::nullopt;
            switch (id) {
            case id_toggle: {
                if (!sel || !state.listing[*sel].manifest) return;
                const bool on = !state.listing[*sel].enabled;
                auto s = read_settings();
                mods::set_mod_choice(s, *folder, on);
                write_settings(s);
                const std::string line = "mod." + *folder + "=" + (on ? "1" : "0") + " written to " + utf8(options.settings);
                log("Mod panel (port-only): " + line + " (applies at the next level of a running game, else at its start)");
                set_status(line);
                refresh(folder);
                return;
            }
            case id_up:
            case id_down: {
                if (!sel) return;
                const auto changes = mods::move_mod(state.listing, *sel, id == id_up ? -1 : 1);
                if (changes.empty()) return;
                mods::apply_priority_changes(changes);
                std::string line = *folder + (id == id_up ? " moved up" : " moved down") + ":";
                for (const auto& c : changes)
                    line += " " + c.folder + " priority=" + std::to_string(c.to) + " (was " + std::to_string(c.from) + ")";
                log("Mod panel (port-only): " + line);
                set_status(line);
                refresh(folder);
                return;
            }
            case id_all: {
                const bool on = !state.mods_on;
                auto s = read_settings();
                mods::set_mods_on(s, on);
                write_settings(s);
                const std::string line = std::string("mods=") + (on ? "1" : "0") + " written to " + utf8(options.settings);
                log("Mod panel (port-only): " + line + " (applies at the next level of a running game, else at its start)");
                set_status(line);
                refresh(folder);
                return;
            }
            case id_refresh:
                refresh(folder);
                set_status("Refreshed");
                return;
            case id_open: {
                std::error_code error;
                fs::create_directories(state.folder, error);
                if (!scripted) ShellExecuteW(window, L"open", state.folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                log("Mod panel (port-only): opened " + utf8(state.folder));
                refresh(folder);
                return;
            }
            case id_play:
                result = ModPanelResult::play;
                return;
            case id_close:
                result = ModPanelResult::close;
                return;
            default:
                return;
            }
        } catch (const std::exception& e) {
            log(std::string("Mod panel (port-only): error: ") + e.what());
            set_status(std::string("Error: ") + e.what());
            if (scripted) throw;
        }
    }

    void capture(const fs::path& file) const {
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        // The client area: the background, then each control drawn by itself through WM_PRINT at its place. This also
        // works for a hidden window, where PrintWindow gives black (and the window's own PRF_CHILDREN pass offsets the
        // controls by the frame).
        RECT rc{};
        GetClientRect(window, &rc);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) throw std::runtime_error("the mod panel has no size to capture");
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        const auto old = SelectObject(memory, bitmap);
        const bool printed = bitmap != nullptr;
        if (printed) {
            FillRect(memory, &rc, GetSysColorBrush(COLOR_BTNFACE));
            for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                if (!(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
                RECT place{};
                GetWindowRect(c, &place);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&place), 2);
                POINT origin{};
                SetViewportOrgEx(memory, place.left, place.top, &origin);
                SendMessageW(c, WM_PRINT, WPARAM(memory), PRF_NONCLIENT | PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
                SetViewportOrgEx(memory, origin.x, origin.y, nullptr);
            }
        }
        GdiFlush();
        mods::RgbaImage image{std::uint32_t(w), std::uint32_t(h), {}};
        if (printed) {
            image.rgba.resize(std::size_t(w) * std::size_t(h) * 4);
            const auto* bgra = static_cast<const std::uint8_t*>(bits);
            for (std::size_t i = 0; i < std::size_t(w) * std::size_t(h); ++i) {
                image.rgba[i * 4 + 0] = bgra[i * 4 + 2];
                image.rgba[i * 4 + 1] = bgra[i * 4 + 1];
                image.rgba[i * 4 + 2] = bgra[i * 4 + 0];
                image.rgba[i * 4 + 3] = 255;
            }
        }
        SelectObject(memory, old);
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        if (!printed) throw std::runtime_error("the mod panel's capture bitmap could not be created");
        if (file.has_parent_path()) fs::create_directories(file.parent_path());
        mods::write_png(file, image);
        log("Mod panel (port-only): captured " + utf8(file) + " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    }

    void pump() const {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(int(msg.wParam));
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void click(int id) const {
        HWND button = GetDlgItem(window, id);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), LPARAM(button));
    }

    void run_script(const fs::path& file) {
        std::ifstream in(file);
        if (!in) throw std::runtime_error("cannot read the mod panel script " + utf8(file));
        std::string line;
        int number = 0;
        while (!result && std::getline(in, line)) {
            ++number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            std::istringstream words(line.substr(first));
            std::string verb;
            words >> verb;
            std::string rest;
            std::getline(words, rest);
            if (const auto b = rest.find_first_not_of(" \t"); b != std::string::npos) rest = rest.substr(b);
            else rest.clear();
            log("Mod panel (port-only): script: " + line.substr(first));
            pump();
            if (verb == "select") {
                const auto it = std::find_if(state.listing.begin(), state.listing.end(), [&](const mods::ModListing& m) {
                    return mods::upper(m.folder) == mods::upper(rest) ||
                           (!m.archive.empty() && mods::upper(utf8(m.archive.filename())) == mods::upper(rest));
                });
                if (it == state.listing.end())
                    throw std::runtime_error("mod panel script line " + std::to_string(number) + ": no mod folder " + rest);
                SendMessageW(list, LB_SETCURSEL, WPARAM(it - state.listing.begin()), 0);
                update_selection();
            } else if (verb == "toggle") click(id_toggle);
            else if (verb == "up") click(id_up);
            else if (verb == "down") click(id_down);
            else if (verb == "all") click(id_all);
            else if (verb == "refresh") click(id_refresh);
            else if (verb == "capture") {
                if (rest.empty()) throw std::runtime_error("mod panel script line " + std::to_string(number) + ": capture needs a file");
                fs::path target(std::u8string(rest.begin(), rest.end()));
                capture(target);
            } else if (verb == "play") click(id_play);
            else if (verb == "close") click(id_close);
            else throw std::runtime_error("mod panel script line " + std::to_string(number) + ": unknown command " + verb);
            pump();
        }
        if (!result) result = ModPanelResult::close;
    }
};

LRESULT CALLBACK panel_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        break;
    }
    case WM_COMMAND:
        if (!panel) break;
        if (LOWORD(wparam) == id_list) {
            if (HIWORD(wparam) == LBN_SELCHANGE) panel->update_selection();
            else if (HIWORD(wparam) == LBN_DBLCLK) panel->command(id_toggle);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED) {
            panel->command(LOWORD(wparam));
            return 0;
        }
        break;
    case WM_CLOSE:
        if (panel) panel->result = ModPanelResult::close;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND child(Panel& p, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p.window, HMENU(INT_PTR(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (!c) throw std::runtime_error("the mod panel's controls could not be created");
    SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
    return c;
}

}  // namespace

ModPanelResult run_mod_panel(const ModPanelOptions& options) {
    Panel p;
    p.options = options;
    p.scripted = options.script.has_value();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static const wchar_t* class_name = L"OmegaStrainModPanel";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.lpszClassName = class_name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("the mod panel's window class could not be registered");
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, client_width, client_height};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    p.window = CreateWindowExW(0, class_name, L"Syphon Filter: The Omega Strain - Mods", style, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, &p);
    if (!p.window) throw std::runtime_error("the mod panel's window could not be created");
    p.mono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    p.ui = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    struct Cleanup {
        Panel& p;
        ~Cleanup() {
            if (p.window) DestroyWindow(p.window);
            if (p.mono) DeleteObject(p.mono);
            if (p.ui) DeleteObject(p.ui);
        }
    } cleanup{p};
    const int list_w = client_width - 170, button_x = client_width - 150, button_w = 140;
    p.header = child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 10, 10, client_width - 20, 20, id_header, p.ui);
    p.list = child(p, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP, 10, 36,
                   list_w, 270, id_list, p.mono);
    SendMessageW(p.list, LB_SETHORIZONTALEXTENT, 1600, 0);
    p.toggle = child(p, L"BUTTON", L"&Enable", BS_PUSHBUTTON | WS_TABSTOP, button_x, 36, button_w, 30, id_toggle, p.ui);
    p.up = child(p, L"BUTTON", L"Move &up", BS_PUSHBUTTON | WS_TABSTOP, button_x, 74, button_w, 30, id_up, p.ui);
    p.down = child(p, L"BUTTON", L"Move do&wn", BS_PUSHBUTTON | WS_TABSTOP, button_x, 112, button_w, 30, id_down, p.ui);
    p.all = child(p, L"BUTTON", L"&All mods off", BS_PUSHBUTTON | WS_TABSTOP, button_x, 160, button_w, 30, id_all, p.ui);
    p.refresh_button = child(p, L"BUTTON", L"&Refresh", BS_PUSHBUTTON | WS_TABSTOP, button_x, 208, button_w, 30, id_refresh, p.ui);
    p.open = child(p, L"BUTTON", L"&Open folder", BS_PUSHBUTTON | WS_TABSTOP, button_x, 246, button_w, 30, id_open, p.ui);
    p.details = child(p, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 10, 316,
                      client_width - 20, 250, id_details, p.mono);
    p.status = child(p, L"STATIC", L"Changes are written at once and apply when the game starts.", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                     10, 584, client_width - 320, 20, id_status, p.ui);
    p.play = child(p, L"BUTTON", L"&Play", BS_DEFPUSHBUTTON | WS_TABSTOP, client_width - 300, 576, 140, 34, id_play, p.ui);
    p.close = child(p, L"BUTTON", L"&Close", BS_PUSHBUTTON | WS_TABSTOP, button_x, 576, button_w, 34, id_close, p.ui);
    p.refresh(std::nullopt);
    p.log("Mod panel (port-only): " + std::to_string(p.state.listing.size()) + " mod(s) in " + utf8(p.state.folder) +
          ", settings " + utf8(options.settings) + (p.state.mods_on ? "" : " (mods=0)"));
    if (!options.hidden) {
        ShowWindow(p.window, SW_SHOWNORMAL);
        SetForegroundWindow(p.window);
        SetFocus(p.list);
    }
    if (options.script) {
        p.run_script(*options.script);
    } else {
        MSG msg;
        while (!p.result) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(int(msg.wParam));
                p.result = ModPanelResult::close;
                break;
            }
            if (!IsDialogMessageW(p.window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    p.log(std::string("Mod panel (port-only): ") + (*p.result == ModPanelResult::play ? "play" : "closed"));
    return *p.result;
}

}  // namespace omega::launcher
