#pragma once
// Port-only (checkpoint 132win3, not the original): one path for a lost Direct3D 11 device. Every D3D / DXGI
// HRESULT the D3D11 backend (gfx_d3d11.cpp: the scene, particle / decal, GUI and corona renderers' calls since
// checkpoint 132mt1) and the game window check goes through check_device or device_result. A device-lost code
// (DXGI_ERROR_DEVICE_REMOVED / RESET / HUNG, DRIVER_INTERNAL_ERROR: a TDR, a driver update or reset, some
// display-mode changes) marks the session's device lost and check_device throws DeviceLost. The backend-neutral
// half (DeviceLost, the loss mark, the test hook) is gfx_device_lost.hpp, which the renderers use.
#include "gfx_device_lost.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <string>

namespace omega {

inline bool device_lost_code(HRESULT r) {
    return r == DXGI_ERROR_DEVICE_REMOVED || r == DXGI_ERROR_DEVICE_RESET || r == DXGI_ERROR_DEVICE_HUNG ||
           r == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// A checked call's result: `r`, or DXGI_ERROR_DEVICE_REMOVED when the test hook fires; a loss code marks the
// device lost.
inline HRESULT device_result(HRESULT r, const char* operation) {
    if (device_lost_injected_now()) r = DXGI_ERROR_DEVICE_REMOVED;
    if (device_lost_code(r)) device_lost_mark(operation);
    return r;
}

inline std::string device_error_text(const char* operation, HRESULT r) {
    char code[16];
    std::snprintf(code, sizeof code, "%lx", static_cast<unsigned long>(r));
    return std::string(operation) + " failed (HRESULT 0x" + code + ")";
}

// Throws DeviceLost for a loss code, std::runtime_error for any other failure (a result already filtered by
// device_result: the hook is not counted again).
inline void throw_if_failed(HRESULT r, const char* operation) {
    if (device_lost_code(r)) {
        device_lost_mark(operation);
        throw DeviceLost(r, device_error_text(operation, r));
    }
    if (FAILED(r)) throw std::runtime_error(device_error_text(operation, r));
}

// Throws DeviceLost for a loss code (or an injected loss), std::runtime_error for any other failure.
inline void check_device(HRESULT r, const char* operation) { throw_if_failed(device_result(r, operation), operation); }

}  // namespace omega
