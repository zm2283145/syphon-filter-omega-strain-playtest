#pragma once
// Port-only (checkpoint 132vt1): the GXM backend's Cg programs (gfx_programs.hpp lists the programs; the D3D11 HLSL in
// gfx_d3d11_shaders.cpp is the reference each one transcribes). The sources are compiled on the PS Vita at first use
// by SceShaccCg (libshacccg.suprx, through vitaShaRK) and the GXP binaries are cached on ux0:data (gfx_gxm.cpp).
//
// Interface conventions shared with gfx_gxm.cpp:
// - A constant buffer bound at slot N is the uniform array named by GxmProgram::constants[N] ("Scene", "Shadow"):
//   float4 elements in the cbuffer's byte order (row_major matrices are four float4 rows; v * M is written out as
//   v.x * M[0] + v.y * M[1] + v.z * M[2] + v.w * M[3]).
// - omegaTexSize[k] = (width, layer height, 1 / width, 1 / layer height) and omegaLayers[k] = the layer count of the
//   texture bound at slot k (Texture2DArray layers are stacked vertically in one GXM texture; Load() becomes a
//   texel-centre sample).
// - omegaBlendMul: the D3D blend factor where GXM has none (rgb multiplies the output; a >= 0 replaces the alpha;
//   gfx_gxm.cpp blend_for()).
// - OMEGA_FLOAT_OUT is defined for a fragment program drawn into a one-channel float target (r32f).
#include "gfx_programs.hpp"

#include <cstddef>
#include <cstdint>

namespace omega::gfx::gxm {

struct GxmAttribute {
    const char* name;    // the Cg vertex input
    std::uint16_t offset;
    std::uint8_t components;  // F32 components
};

struct GxmLayout {
    const GxmAttribute* attributes;
    unsigned count;
};

// One vertex or fragment Cg source.
struct GxmStage {
    const char* name;    // for logs and the GXP cache
    const char* source;
};

struct GxmProgram {
    unsigned vertex;     // index into gxm_vertex_stage
    unsigned fragment;   // index into gxm_fragment_stage
    unsigned fragment_alt;  // particle_haze: the frame-alpha (SV_Target1) fragment program, else ~0U
    const char* constants[2];  // uniform names of constant-buffer slots b0, b1 (null: unused)
    bool cpu_geometry;   // shadow_receiver: the guard-box / facing GS runs on the CPU (gfx_gxm.cpp)
    unsigned char samplers[4];  // the sampler slot (s0 / s1) each texture slot t0..t3 samples with
};

// The programs this backend has (every one before the GPU skinning pair of 132md8): Program::scene_skinned /
// shadow_skinned and VertexLayout::skinned are not ported (capabilities().gpu_skinning is false, so the mod system
// skins on the CPU and never asks for them); gxm_program / gxm_layout throw for them.
constexpr std::size_t gxm_program_count = static_cast<std::size_t>(Program::scene_skinned);
constexpr std::size_t gxm_layout_count = static_cast<std::size_t>(VertexLayout::skinned);

const GxmProgram& gxm_program(Program program);
const GxmLayout& gxm_layout(VertexLayout layout);
const GxmStage& gxm_vertex_stage(unsigned index);
const GxmStage& gxm_fragment_stage(unsigned index);

// Backend-internal programs (clears and texture copies).
enum : unsigned {
    gxm_clear_vertex = 100,     // position xy (NDC); omegaClear.x = depth
    gxm_clear_fragment = 101,   // omegaColour
    gxm_copy_vertex = 102,      // position xy (NDC), uv
    gxm_copy_fragment = 103     // the texel at uv
};

}  // namespace omega::gfx::gxm
