#pragma once
// Port-only (132ps5c, research/PS5.md "Interfaces"): the Win32 names the PC hosts (play_main.cpp, frontend_app.cpp,
// frame_profile.hpp) use around their game window and event loop, for the PS5 build, where host_window_ps5.hpp's
// fixed screen is the window. The PS5 screen has no message queue: PeekMessageW finds nothing, PostMessageW of a key
// (a port-only test key, the overlay toggle) goes to the window's own events as WM_KEYDOWN would, and Alt+F4
// (WM_SYSKEYDOWN VK_F4) is the window's close request. The virtual-key values are Windows' (the port settings' key
// bindings name them). The clock is CLOCK_MONOTONIC in nanoseconds. Never part of the Windows build.

#include <cerrno>
#include <cstdint>
#include <strings.h>
#include <time.h>

// The MSVC CRT's ASCII case-insensitive compare (the same result for the ASCII names compared).
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

using UINT = unsigned int;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using HWND = void*;

struct MSG {
    HWND hwnd = nullptr;
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
};

inline constexpr UINT PM_REMOVE = 0x0001;
inline constexpr UINT WM_QUIT = 0x0012;
inline constexpr UINT WM_KEYDOWN = 0x0100;
inline constexpr UINT WM_SYSKEYDOWN = 0x0104;
inline constexpr int WHEEL_DELTA = 120;

inline constexpr int VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0d, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
                     VK_ESCAPE = 0x1b, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
                     VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2d, VK_DELETE = 0x2e,
                     VK_F1 = 0x70, VK_F2 = 0x71, VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76,
                     VK_F8 = 0x77, VK_F9 = 0x78, VK_F10 = 0x79, VK_F11 = 0x7a, VK_F12 = 0x7b;

inline bool PeekMessageW(MSG*, HWND, UINT, UINT, UINT) { return false; }
inline bool TranslateMessage(const MSG*) { return false; }
inline LRESULT DispatchMessageW(const MSG*) { return 0; }
bool PostMessageW(HWND window, UINT message, WPARAM w, LPARAM l);  // host_window_ps5.cpp

union LARGE_INTEGER {
    std::int64_t QuadPart;
};
inline bool QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1000000000;
    return true;
}
inline bool QueryPerformanceCounter(LARGE_INTEGER* count) {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    count->QuadPart = std::int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
    return true;
}

inline void Sleep(unsigned milliseconds) {
    timespec t{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}