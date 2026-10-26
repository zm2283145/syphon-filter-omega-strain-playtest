#pragma once
// Port-only (checkpoint 132vk1, research/VULKAN.md): the PC's renderer backend choice. The PC has two native
// backends of gfx_device.hpp: Direct3D 11 (gfx_d3d11.cpp, the default and the reference) and Vulkan (gfx_vulkan.cpp,
// built with the CMake option OMEGA_PC_VULKAN). The choice is made once at start (omega_play --renderer, the port
// settings' renderer= key, the Port Settings page's RENDERER row) and applies to every device made from then on: the
// game window's (HostWindow) and the offscreen GUI devices (create_offscreen_device). It changes how the frame is
// drawn, never what the game computes.
#include "gfx_device.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace omega::gfx {

enum class PcBackend : std::uint8_t { d3d11, vulkan };

// The backend the next device is made with (d3d11 until set).
PcBackend pc_backend();
void set_pc_backend(PcBackend backend);
// "d3d11" / "vulkan".
const char* pc_backend_name(PcBackend backend);
// This build has the Vulkan backend (OMEGA_PC_VULKAN).
bool pc_vulkan_built();

// Port-only (132vk1): when the Vulkan device cannot be made (no Vulkan 1.3 driver, a missing feature), the reason;
// the caller falls back to Direct3D 11 and logs it. Empty when Vulkan was not tried or worked.
std::string pc_vulkan_failure();
void note_pc_vulkan_failure(const std::string& why);

}  // namespace omega::gfx
