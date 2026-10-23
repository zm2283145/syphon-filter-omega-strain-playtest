#pragma once
// Port-only (checkpoint 132mt1): the D3D11 backend's HLSL programs and vertex layouts for gfx::Program /
// gfx::VertexLayout (gfx_programs.hpp), with the compile / create labels the renderers used before.
#include "gfx_programs.hpp"

#include <d3d11.h>

#include <cstddef>

namespace omega::gfx::d3d11 {

struct HlslProgram {
    const char* source;
    std::size_t size;
    const char* source_name;    // D3DCompile's source name
    const char* vertex_entry;
    const char* geometry_entry; // null: no geometry shader
    const char* pixel_entry;
    const char* compile_label;  // the checked compile call
    const char* error_prefix;   // the compile error message
    const char* vertex_label;
    const char* geometry_label;
    const char* pixel_label;
};
const HlslProgram& hlsl_program(Program program);

struct HlslLayout {
    const D3D11_INPUT_ELEMENT_DESC* elements;
    unsigned count;
    const char* label;
};
const HlslLayout& hlsl_layout(VertexLayout layout);

}  // namespace omega::gfx::d3d11
