#pragma once
// Port-only (checkpoint 132mt1): the Direct3D 11 backend of the renderer abstraction layer (gfx_device.hpp). Only
// the platform code that makes devices and windows (HostWindow, the scene / actor tools' own windows, the GUI
// renderer's offscreen device) and the device tests include this header; the drawing code sees gfx::Device.
//
// Every call the renderers made directly before 132mt1 is made here with the same arguments (the HLSL text, the
// state descriptions, the bind order of a draw), so the pixels are unchanged; every HRESULT goes through
// check_device / device_result (device_lost.hpp) with the label the renderer passes, as before.
#include "gfx_device.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace omega::gfx::d3d11 {

// A device of an existing D3D11 device and its immediate context (HostWindow's).
std::shared_ptr<Device> wrap_device(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, std::string name);
// A swap chain of an existing DXGI swap chain on that device.
std::shared_ptr<SwapChain> wrap_swap_chain(const std::shared_ptr<Device>& device,
                                           Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain);
// A texture of an existing render target view (HostWindow::target(): the window's back buffer view).
std::shared_ptr<Texture> wrap_target(const std::shared_ptr<Device>& device, ID3D11RenderTargetView* view);

// Its own device and legacy-discard swap chain for a window (hardware with BGRA support at feature level 11_0,
// else WARP): the scene and actor preview tools (SceneRenderer's own window before 132mt1). owned = true.
Display create_window_display(HWND window);
// Its own device without a window (hardware, else WARP): the GUI renderer's offscreen captures.
std::shared_ptr<Device> create_offscreen_device(const char* label);

// The D3D11 objects behind a gfx object (tests, diagnostics).
ID3D11Device* native_device(const Device& device);
ID3D11DeviceContext* native_context(const Device& device);
ID3D11Texture2D* native_texture(const Texture& texture);

}  // namespace omega::gfx::d3d11
