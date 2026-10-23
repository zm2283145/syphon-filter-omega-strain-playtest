#pragma once
// Port-only (checkpoint 132mt1): every shader program (shader variant) the port draws with, in one place. A
// program is a vertex shader, an optional geometry shader and a pixel shader over one vertex layout; its
// constant-buffer, texture and sampler slots are listed here so each backend can provide the same interface. The
// D3D11 backend's HLSL for each is in gfx_d3d11_shaders.cpp (the text the renderers compiled before 132mt1,
// unchanged); a GXM backend supplies its own compiled programs for the same list.
//
//   program            layout         constants (stage: slot)       textures (pixel)              samplers
//   scene_model        mesh           VS/PS b0 Scene                t0 surface                    s0
//   shadow_buffer      mesh           VS/PS b0 Scene, b1 Shadow     t0 surface                    s0
//   shadow_receiver    mesh (+ GS)    VS/GS/PS b0, b1               t1 shadow buffers, t2 projector s1
//   gui                gui            -                             t0 image                      s0
//   particle_sprite    particle       -                             t0 image                      s0
//   particle_clip      particle_clip  -                             t0 image                      s0
//   particle_haze      particle_haze  -                             t0 image, t1 ScreenFxBuf, t2 Cd s0
//   particle_copy      particle       -                             t0 frame copy                 s0
//   corona_probe       corona         -                             -                             -
//   corona_reduce      corona         -                             t0 frame alpha                s0
//   corona_element     corona         -                             t0 element, t1 visibility     s0
//   scene_skinned      skinned        VS/PS b0 Scene, VS b2 Skin    t0 surface                    s0
//   shadow_skinned     skinned        VS/PS b0, b1, VS b2 Skin      t0 surface                    s0
#include <cstddef>
#include <cstdint>

namespace omega::gfx {

enum class Program : std::uint8_t {
    scene_model,       // scene_renderer: the level and character models (VU1 programs 0x4f8 / 0x595 / 0x5cd host)
    shadow_buffer,     // scene_renderer: a ShadowBuf slot render (003d2d10)
    shadow_receiver,   // scene_renderer: the receiver pass (VU1 routine 0047ae50), with the guard-box GS
    gui,               // gui_renderer: the GUI primitive lists, fades, vision layers
    particle_sprite,   // particle_renderer: GS sprites (VU1 0x5e0 / 0x8a0)
    particle_clip,     // particle_renderer: ShatterWindow triangles in clip space
    particle_haze,     // particle_renderer: heat haze (VU1 0x1cc8), two targets
    particle_copy,     // particle_renderer: the frame to ScreenFxBuf (0037bd40)
    corona_probe,      // corona_renderer: the visibility probe (003781b0)
    corona_reduce,     // corona_renderer: the 32 -> 1 reduce
    corona_element,    // corona_renderer: the element sprites
    // Port-only (checkpoint 132md8, research/MODDING.md "Speed", RENDERER.md "GPU skinning"): scene_model /
    // shadow_buffer for a mod replacement part skinned on the GPU (gfx::GpuSkinVertex, the palette in b2); prepared at
    // the level's load when a mod replaces models (SceneRenderer::prepare_skinning), else on the first skinned draw.
    scene_skinned,
    shadow_skinned,
    count
};
constexpr std::size_t program_count = static_cast<std::size_t>(Program::count);

// Vertex layouts (the renderers' vertex structs; strides in bytes).
enum class VertexLayout : std::uint8_t {
    mesh,           // MeshVertex (60): position, normal, uv, colour, gore
    gui,            // 40: position xy, uv, colour, depth zw
    particle,       // 36: position xyz, uv, colour
    particle_clip,  // 40: position xyzw, uv, colour
    particle_haze,  // 44: position xyz, uv, st, colour
    corona,         // 56: position xyzw, uv, colour, params
    skinned,        // 72: position, normal, uv, 8 bone bytes, 8 weights (132md8)
    count
};

constexpr VertexLayout program_layout(Program p) {
    switch (p) {
    case Program::scene_model: case Program::shadow_buffer: case Program::shadow_receiver: return VertexLayout::mesh;
    case Program::gui: return VertexLayout::gui;
    case Program::particle_sprite: case Program::particle_copy: return VertexLayout::particle;
    case Program::particle_clip: return VertexLayout::particle_clip;
    case Program::particle_haze: return VertexLayout::particle_haze;
    case Program::scene_skinned: case Program::shadow_skinned: return VertexLayout::skinned;
    default: return VertexLayout::corona;
    }
}

// The program whose pixel stage a program shares (itself if none): a skinned program differs from its base only in
// the vertex stage, so a backend may reuse the base's compiled pixel shader (the scene's is the costly one to build).
constexpr Program pixel_program(Program p) {
    switch (p) {
    case Program::scene_skinned: return Program::scene_model;
    case Program::shadow_skinned: return Program::shadow_buffer;
    default: return p;
    }
}

}  // namespace omega::gfx
