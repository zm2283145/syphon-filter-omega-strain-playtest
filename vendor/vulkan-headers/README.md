# Vulkan-Headers (vendored, port-only, checkpoint 132vk1)

The Khronos Vulkan headers, tag v1.3.296 of https://github.com/KhronosGroup/Vulkan-Headers (`include/vulkan/vk_platform.h`,
`vulkan_core.h`, `vulkan_win32.h` and the `include/vk_video` headers `vulkan_core.h` includes), unchanged, under the
licence in `LICENSE.md` (Apache-2.0 OR MIT). Only the PC Vulkan renderer uses them (`src/port/gfx/gfx_vulkan.cpp`,
`research/VULKAN.md`); the game opens the driver's `vulkan-1.dll` at run time and links no Vulkan library.
