#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct Vec3 { float x{}, y{}, z{}; };
struct Bounds { Vec3 minimum{}, maximum{}; };
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    std::array<float, 2> uv{};
    std::array<float, 4> color{};
    // Overlay characters only (Checkpoint 132wb): the character program's gore
    // subtract (VU1 micro 0x3058) from the lit colour, in units of 0x80 = 1.0:
    // red, green, blue. Zero for every other vertex.
    std::array<float, 3> gore{};
};
struct MaterialPass {
    std::uint32_t texture{}, shader{};
    // Pass k >= 1: ALPHA_1 of its section in the strip's later-pass packet
    // (00377270 patches the section's TEX0 / MSCAL; ALPHA comes from the file).
    std::optional<std::uint64_t> gs_alpha;
};
struct MeshMaterial {
    std::uint32_t texture{}, flags{}, shader{};
    std::vector<MaterialPass> passes;
    // Model Z axis in mesh space and whether the placement was a proper
    // rotation (orthonormal, determinant +1). The reflection-map passes build
    // their tangent frame from the model Z axis (VU1 program 0x4f8).
    Vec3 model_up{0, 0, 1};
    bool rigid_pose{true};
    // The owning model's VUM header +4 flags (kept per material so merged
    // meshes keep them). 00377570 maps bit 0 -> model+0x24 0x400000 (GS TEST
    // alpha > 0x40 else frame buffer only, 0037d374), bit 1 -> 0x800000,
    // otherwise 0x200000 (both alpha > 0 else keep, 0037d3a8 / 0037d638).
    std::uint32_t model_flags{};
    // GS ALPHA_1 of the base pass, from the batch render-state packet
    // (qword 5, register 0x42) that the VU1 microcode kicks before the
    // strip; it replaces the renderer default 0x44 / FIX 0x80 (004bc1b0).
    // Empty when no strip of the material carries a render-state packet.
    std::optional<std::uint64_t> gs_alpha;
    // Material +8/+0xc: the shader-13 animator parameters (00376ad0). Flag 2
    // (UV scroll 003d1e90): the u and v speeds (float bits); else flag 4
    // (texture frames 003d1b60): the mode and the frame rate (float bits).
    std::array<std::uint32_t, 2> animation{};
    // Overlay only: the surface is drawn without the port's lighting term (the
    // gun flare programs 12 / 14 write a constant colour and read no normals).
    bool unlit{};
};
struct MeshSurface {
    std::uint32_t material{}, first_index{}, index_count{};
    // Port-only (checkpoint 132md8, research/MODDING.md "Speed"): 1 + the index of the overlay skin (SceneRenderer
    // OverlaySkin, a mod replacement part skinned on the GPU) whose own buffers the surface draws (first_index and
    // index_count then index the model's triangles); 0: the mesh's own vertices and indices.
    std::uint32_t skin{};
};
struct ColorPatchSpan {
    std::uint32_t source_offset{}, first_vertex{};
    std::vector<std::uint32_t> colors;
};
struct ColorPatchSet { std::uint32_t key{}; std::vector<ColorPatchSpan> spans; };
// One VIF vertex batch in decode order: its vertices and the first two
// qwords of its 6-qword header (VU1 TOP+6 / TOP+7), which the terrain
// point-light pass of microcode set 0 reads as the batch bounds (min, max).
struct MeshBatch {
    std::uint32_t first_vertex{}, vertex_count{};
    std::array<float, 4> header_minimum{}, header_maximum{};
};
struct Mesh {
    std::vector<std::string> textures;
    std::vector<MeshMaterial> materials;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshSurface> surfaces;
    Bounds bounds;
    Bounds declared_bounds;
    std::uint32_t batches{};
    std::vector<MeshBatch> batch_spans;
    std::vector<ColorPatchSet> color_patches; // Header +0x58 table (00377050); toggled by toggle_color_patch.
    // VUM header +4 (flags word read by 00377570 / 00377120 and the scene
    // buckets of 0013e1b0) and the W lanes of declared_bounds (+0x3c, +0x4c).
    std::uint32_t header_flags{};
    std::array<float, 2> declared_bounds_w{1.0F, 1.0F};
};
struct CollisionTriangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint16_t flags{};
    float coefficient{};
    std::uint32_t reserved{};
};
struct CollisionMesh {
    std::vector<Vec3> vertices;
    std::vector<CollisionTriangle> triangles;
};

Mesh decode_vum(std::span<const std::uint8_t> bytes);
CollisionMesh decode_col(std::span<const std::uint8_t> bytes);
// 003767e0(model, index, on): `bits_68` is the model header +0x68, which
// 00377050 sets to -1 (the stored colours are the all-on state). When bit
// (index & 31) (srlv) already equals `on` nothing happens and false is
// returned; otherwise the bit flips and every block of patch set `index` adds
// (on) or subtracts (off) its payload bytes 0, 1, 2 to the RGB bytes it
// targets, each wrapping at 8 bits (no saturation). `colours` holds the
// decoded vertex colour bytes (MeshVertex::color * 128) indexed like
// set.spans[].first_vertex; a shared VIF source has one span per decoded copy,
// so each span is applied once. `changed` (optional) receives the vertex
// indices written. Throws for a span outside `colours`.
bool toggle_color_patch(std::uint32_t& bits_68, const ColorPatchSet& set, std::uint32_t index, bool on,
                        std::vector<std::array<std::uint8_t, 3>>& colours, std::vector<std::uint32_t>* changed = nullptr);
void write_model(std::ostream& output, const std::string& name, const Mesh& mesh,
                 const CollisionMesh& collision);

}
