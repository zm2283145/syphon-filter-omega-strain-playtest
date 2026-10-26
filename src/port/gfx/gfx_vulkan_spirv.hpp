#pragma once
// Port-only (checkpoint 132vk1, research/VULKAN.md "Shaders"): the PC Vulkan backend's shader programs, SPIR-V made
// offline by tools/build_vulkan_spirv.py from the D3D11 backend's HLSL (gfx_d3d11_shaders.cpp) and embedded in
// gfx_vulkan_spirv.cpp (generated). Bindings: HLSL b<n> -> binding n, t<n> -> 16 + n, s<n> -> 32 + n, set 0.
#include "gfx_programs.hpp"

#include <cstddef>
#include <cstdint>

namespace omega::gfx::vulkan {

struct SpirvStage {
    const std::uint32_t* code = nullptr;  // null: no such stage
    std::size_t words = 0;
    const char* entry = nullptr;  // the SPIR-V entry point (the HLSL entry's name)
};
// A vertex program input: its D3D semantic (name, index) and the location DXC gave it.
struct SpirvInput {
    const char* semantic;
    unsigned index;
    unsigned location;
};
struct SpirvProgram {
    SpirvStage vertex, geometry, pixel;
    const SpirvInput* inputs;
    unsigned input_count;
};
const SpirvProgram& spirv_program(Program program);
// The HLSL / flags hash the SPIR-V was made from (tools/build_vulkan_spirv.py --check compares it).
const char* spirv_source_hash();

constexpr unsigned binding_constants = 0, binding_textures = 16, binding_samplers = 32;

}  // namespace omega::gfx::vulkan
