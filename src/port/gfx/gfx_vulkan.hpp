#pragma once
// Port-only (checkpoint 132vk1, research/VULKAN.md): the PC's Vulkan backend of the renderer abstraction layer
// (gfx_device.hpp), beside the Direct3D 11 one (gfx_d3d11.cpp, the default and the reference). Native Vulkan 1.3
// written against the same interface (no translation layer): the vulkan-1.dll loader that ships with every GPU driver
// is opened at run time (nothing else installed), the shaders are SPIR-V made offline from the same HLSL
// (gfx_vulkan_spirv.hpp), and each D3D11 state of the interface maps to its Vulkan equivalent so the pixels match the
// D3D11 backend's (research/VULKAN.md "Pixel parity").
//
// Only the platform code that makes devices and windows includes this header (HostWindow, gfx_pc_backend.cpp).
#include "gfx_device.hpp"

#include <windows.h>

#include <memory>
#include <string>

namespace omega::gfx::vulkan {

struct WindowDisplay {
    Display display;
    std::string swap_description;  // "Vulkan swap chain: FIFO, 3 images, B8G8R8A8_UNORM, frame latency 1"
};

// A device and swap chain for the game window: the back buffer is the size width x height (the window's client
// size times the render scale) and is scaled to the window's client area at present. Throws std::runtime_error when
// the machine has no usable Vulkan 1.3 device (the caller falls back to Direct3D 11). owned = false (HostWindow
// keeps it).
WindowDisplay create_window_display(HWND window, unsigned width, unsigned height, unsigned frame_latency);
// A device without a window (the offscreen GUI captures), on the same GPU as the window's.
std::shared_ptr<Device> create_offscreen_device(const char* label);

}  // namespace omega::gfx::vulkan
