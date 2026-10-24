#include "scene_renderer.hpp"
#include "character_model.hpp"
#include "frame_fit.hpp"
#include "gfx_device_lost.hpp"
#include "gs_alpha.hpp"
#include "texture_blocks.hpp"

#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "directxmath_vita.hpp"  // port-only (132vt2): the PS Vita build's camera-matrix subset
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace omega {
namespace {
using namespace DirectX;

// The model shader (gfx::Program::scene_model, HLSL in gfx_d3d11_shaders.cpp) and the character shadows
// (CHARACTER_SHADOWS.md sections 5, 6 and 9: shadow_buffer, the buffer render 003d2d10; shadow_receiver, the
// receiver pass of the VU1 routine at 0047ae50) read these constants.
struct alignas(16) ShadowConstants {
    float clip[16];
    float scale[4];
    float offset[4];
    float facing[4];
    float params[4];
};
constexpr unsigned shadow_buffer_size = 128;
constexpr unsigned shadow_buffer_layers = 32;

struct alignas(16) Constants {
    XMFLOAT4X4 mvp;
    XMFLOAT4 options;
    XMFLOAT4 eye;
    XMFLOAT4 up{0, 0, 1, 0};
    XMFLOAT4 alpha_test{-1, 0, 0, 0};
    XMFLOAT4 uv_offset{0, 0, 0, 0};
};
static_assert(sizeof(MeshVertex) == 60);

gfx::TargetBlend blend_target(gfx::BlendFactor src, gfx::BlendFactor dst, gfx::BlendOp op, gfx::BlendFactor src_alpha,
                              gfx::BlendFactor dst_alpha, std::uint8_t mask = gfx::mask_all) {
    gfx::TargetBlend t;
    t.enable = true;
    t.src = src;
    t.dst = dst;
    t.op = op;
    t.src_alpha = src_alpha;
    t.dst_alpha = dst_alpha;
    t.write_mask = mask;
    return t;
}
gfx::BlendDesc one_target(const gfx::TargetBlend& t) {
    gfx::BlendDesc d;
    d.targets[0] = t;
    return d;
}

}

OrbitCamera::OrbitCamera(const Bounds& bounds) {
    target = {(bounds.minimum.x + bounds.maximum.x) * 0.5F,
              (bounds.minimum.y + bounds.maximum.y) * 0.5F,
              (bounds.minimum.z + bounds.maximum.z) * 0.5F};
    const double x = double(bounds.maximum.x) - bounds.minimum.x;
    const double y = double(bounds.maximum.y) - bounds.minimum.y;
    const double z = double(bounds.maximum.z) - bounds.minimum.z;
    radius = static_cast<float>(std::sqrt(x * x + y * y + z * z) * 0.5);
    if (!std::isfinite(radius) || radius <= 0 || radius > 10000000
        || !std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z))
        throw std::runtime_error("Model bounds cannot define a finite preview camera");
    distance = radius * 2.8F;
}

void OrbitCamera::orbit(float horizontal, float vertical) {
    yaw = std::remainder(yaw + horizontal, DirectX::XM_2PI);
    pitch = std::clamp(pitch + vertical, -1.45F, 1.45F);
}

void OrbitCamera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(0.85F, steps), radius * 0.2F, radius * 20);
}

SceneRenderer::Ptr<gfx::Buffer> SceneRenderer::buffer(const void* data, std::size_t size, unsigned binding, bool updatable) {
    if (!data || !size || size > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Invalid mesh GPU buffer size");
    try {
        return device_->create_buffer({size, updatable ? gfx::Usage::gpu : gfx::Usage::immutable, binding}, data,
                                      "Create mesh GPU buffer");
    } catch (const DeviceLost&) {
        return {};  // port-only (132win2): reset_device re-uploads it
    }
}

namespace {
constexpr unsigned max_viewport = 16384;  // D3D11's texture limit (a fullscreen 8K monitor fits)
}

SceneRenderer::SceneRenderer(const SceneDisplay& display, const Mesh& mesh, const CollisionMesh& collision,
                             const std::vector<Texture>& textures)
    : mesh_(mesh), owned_(display.owned), backend_(display.backend), device_(display.device),
      swap_chain_(display.swap_chain) {
    if (!device_ || !swap_chain_) throw std::runtime_error("Incomplete shared 3D display");
    swap_chain_->size(width_, height_, "Read the shared swap chain");
    if (!width_ || !height_ || width_ > max_viewport || height_ > max_viewport)
        throw std::runtime_error("Unsupported 3D viewport dimensions");
    if (!owned_) textures_source_ = &textures;
    initialize(collision, textures);
}

void SceneRenderer::reset_device(const SceneDisplay& display) {
    if (owned_ || !textures_source_) throw std::runtime_error("reset_device needs a renderer of a shared display");
    if (!display.device || !display.swap_chain) throw std::runtime_error("Incomplete shared 3D display");
    release_device();
    device_ = display.device;
    swap_chain_ = display.swap_chain;
    backend_ = display.backend;
    swap_chain_->size(width_, height_, "Read the shared swap chain");
    if (!width_ || !height_ || width_ > max_viewport || height_ > max_viewport)
        throw std::runtime_error("Unsupported 3D viewport dimensions");
    initialize(CollisionMesh{}, *textures_source_);
    if (collision_index_count_) {
        collision_vertices_ = buffer(collision_points_.data(), collision_points_.size() * sizeof(MeshVertex), gfx::bind_vertices);
        collision_indices_ = buffer(collision_index_data_.data(), collision_index_data_.size() * sizeof(std::uint32_t),
                                    gfx::bind_indices);
    }
    if (overlay_source_) set_overlay_textures(*overlay_source_);
    if (projector_source_) set_projector_textures(*projector_source_);
    refresh_overlay();
}

void SceneRenderer::release_device() {
    if (owned_) throw std::runtime_error("release_device needs a renderer of a shared display");
    list_.clear();
    if (device_) {
        list_.reset_state();
        device_->submit(list_);
    }
    // Everything made on the old device goes; the CPU-side state (mesh_, the overlay mesh, plans, frames,
    // offsets, untextured materials, the shadow frame pointer) is kept and re-uploaded.
    target_.reset(); depth_.reset(); frame_alpha_.reset();
    constants_.reset();
    vertices_.reset(); indices_.reset(); collision_vertices_.reset(); collision_indices_.reset();
    skin_buffers_.clear(); skin_constants_.clear(); skin_written_.clear(); skin_current_ = nullptr; skin_ready_ = false;  // 132md8
    overlay_textures_.clear(); projector_textures_.clear(); overlay_vertices_.reset(); overlay_indices_.reset();
    solid_.reset(); wire_.reset(); depth_enabled_.reset(); depth_disabled_.reset();
    blend_.reset(); blend_add_half_.reset(); blend_subtract_half_.reset();
    blend_alpha_add_.reset(); blend_alpha_subtract_.reset(); blend_fix_over_.reset();
    blend_one_add_.reset(); blend_one_subtract_.reset();
    raw_alpha_textures_.clear(); sampler_.reset(); textures_.clear(); texture_frames_.clear();
    shadow_array_.reset(); shadow_constants_.reset();
    blend_opaque_.reset(); blend_shadow_subtract_.reset(); blend_shadow_reflect_.reset();
    depth_test_only_.reset(); shadow_sampler_.reset();
    swap_chain_.reset(); device_.reset();
}

void SceneRenderer::initialize(const CollisionMesh& collision, const std::vector<Texture>& textures) {
    const Mesh& mesh = mesh_;
    device_->prepare(gfx::Program::scene_model);
    constants_ = device_->create_buffer({sizeof(Constants), gfx::Usage::gpu, gfx::bind_constants}, nullptr,
                                        "Create camera buffer");
    vertices_ = buffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(MeshVertex), gfx::bind_vertices, true);
    indices_ = buffer(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t), gfx::bind_indices);

    solid_ = device_->create_raster({false}, "Create solid rasterizer");
    wire_ = device_->create_raster({true}, "Create wireframe rasterizer");
    depth_enabled_ = device_->create_depth({true, true, gfx::Compare::less_equal}, "Create depth state");
    depth_disabled_ = device_->create_depth({false, false, gfx::Compare::less_equal}, "Create overlay depth state");
    using F = gfx::BlendFactor;
    using O = gfx::BlendOp;
    blend_ = device_->create_blend(one_target(blend_target(F::src_alpha, F::inv_src_alpha, O::add, F::one, F::zero)),
                                   "Create preview blend state");
    // Later material passes: GS ALPHA 0x68 / 0x62 with FIX 0x40 give
    // Cd + Cs*64/128 and Cd - Cs*64/128 (blend factor 0.5). RGB only.
    blend_add_half_ = device_->create_blend(one_target(blend_target(F::factor, F::one, O::add, F::one, F::zero, gfx::mask_rgb)),
                                            "Create additive pass blend state");
    blend_subtract_half_ = device_->create_blend(
        one_target(blend_target(F::factor, F::one, O::rev_subtract, F::one, F::zero, gfx::mask_rgb)),
        "Create subtractive pass blend state");
    // Base-pass render states from the VUM batch packets (gs_blend_state):
    // 0x48 Cs*As + Cd, 0x42 Cd - Cs*As, 0x64 (Cs - Cd)*FIX + Cd. 0x68 and
    // 0x62 use the pass states above with the blend factor FIX / 128.
    blend_alpha_add_ = device_->create_blend(one_target(blend_target(F::src_alpha, F::one, O::add, F::one, F::zero)),
                                             "Create alpha add blend state");
    blend_alpha_subtract_ = device_->create_blend(
        one_target(blend_target(F::src_alpha, F::one, O::rev_subtract, F::one, F::zero)), "Create alpha subtract blend state");
    blend_fix_over_ = device_->create_blend(one_target(blend_target(F::factor, F::inv_factor, O::add, F::one, F::zero)),
                                            "Create constant blend state");
    blend_one_add_ = device_->create_blend(one_target(blend_target(F::one, F::one, O::add, F::one, F::zero)),
                                           "Create premultiplied add blend state");
    blend_one_subtract_ = device_->create_blend(one_target(blend_target(F::one, F::one, O::rev_subtract, F::one, F::zero)),
                                                "Create premultiplied subtract blend state");
    // Decoded mip levels are sampled with the host's derivative-based LOD;
    // the PS2 TEX1 LOD formula (K/L) is not reproduced.
    sampler_ = device_->create_sampler({gfx::Filter::linear, gfx::Address::wrap, std::numeric_limits<float>::max(), true},
                                       "Create texture sampler");
    for (const auto& texture : textures) {
        textures_.push_back(upload_texture(texture));
        auto& frames = texture_frames_.emplace_back();
        for (const auto& frame : texture.animation) frames.push_back(upload_texture(frame));
        auto& raw = raw_alpha_textures_.emplace_back();
        raw.push_back(upload_raw_alpha(texture));
        for (const auto& frame : texture.animation) raw.push_back(upload_raw_alpha(frame));
    }
    if (!collision.triangles.empty()) {
        std::vector<MeshVertex> points;
        for (const auto& point : collision.vertices) points.push_back({point, {}, {}, {1, 1, 1, 1}});
        std::vector<std::uint32_t> indices;
        for (const auto& triangle : collision.triangles)
            indices.insert(indices.end(), triangle.indices.begin(), triangle.indices.end());
        collision_vertices_ = buffer(points.data(), points.size() * sizeof(MeshVertex), gfx::bind_vertices);
        collision_indices_ = buffer(indices.data(), indices.size() * sizeof(std::uint32_t), gfx::bind_indices);
        collision_index_count_ = static_cast<unsigned>(indices.size());
        if (!owned_) {
            collision_points_ = std::move(points);
            collision_index_data_ = std::move(indices);
        }
    }
    create_shadow_resources();
    targets();
}

void SceneRenderer::create_shadow_resources() {
    device_->prepare(gfx::Program::shadow_buffer);
    device_->prepare(gfx::Program::shadow_receiver);
    shadow_constants_ = device_->create_buffer({sizeof(ShadowConstants), gfx::Usage::gpu, gfx::bind_constants}, nullptr,
                                               "Create shadow constants");
    // ShadowBuf: 128x128 PSMCT32 per slot (bytes stored directly, 0x80 = 128/255).
    gfx::TextureDesc array;
    array.width = array.height = shadow_buffer_size;
    array.layers = shadow_buffer_layers;
    array.format = gfx::Format::rgba8;
    array.bind = gfx::bind_target | gfx::bind_sampled;
    gfx::TextureLabels labels;
    labels.texture = "Create shadow buffers";
    labels.target = "Create shadow buffer target";
    labels.sampled = "Create shadow buffer view";
    shadow_array_ = device_->create_texture(array, nullptr, labels);
    using F = gfx::BlendFactor;
    using O = gfx::BlendOp;
    blend_opaque_ = device_->create_blend({}, "Create opaque blend state");
    // GS ALPHA 0xA1 with FIX 128: (Cd - Cs) * 128 >> 7; the UNORM target clamps at 0 (COLCLAMP).
    blend_shadow_subtract_ = device_->create_blend(
        one_target(blend_target(F::one, F::one, O::rev_subtract, F::zero, F::one, gfx::mask_rgb)), "Create shadow blend state");
    // GS ALPHA 0x44: (Cs - Cd) * As >> 7 + Cd; As/128 above 1.0 has no D3D11 equivalent and is clamped.
    blend_shadow_reflect_ = device_->create_blend(
        one_target(blend_target(F::src_alpha, F::inv_src_alpha, O::add, F::zero, F::one, gfx::mask_rgb)),
        "Create reflection blend state");
    depth_test_only_ = device_->create_depth({true, false, gfx::Compare::less_equal}, "Create shadow depth state");
    // CLAMP/CLAMP; TEX1 33: MMAG linear, MMIN nearest, LOD 0.
    shadow_sampler_ = device_->create_sampler({gfx::Filter::min_point_mag_linear, gfx::Address::clamp, 0.0F, true},
                                              "Create shadow sampler");
}

void SceneRenderer::set_shadows(const ShadowDrawFrame* frame) { shadows_ = frame; }

void SceneRenderer::draw_shadow_buffers() {
    const auto& buffers = shadows_->buffers;
    if (buffers.empty()) return;
    if (buffers.size() > shadow_buffer_layers) throw std::runtime_error("More shadow slots than shadow buffers");
    if ((!overlay_vertices_ || !overlay_indices_) && !has_overlay_skins())
        throw std::runtime_error("Shadow slots need the caster overlay mesh");
    auto& l = list_;
    l.set_texture(1, nullptr);
    l.set_viewport({1, 1, 126, 126, 0, 1});   // 003d3690: x = y = 1, w = h = 126
    l.set_raster(solid_.get());
    l.set_depth(depth_disabled_.get());
    l.set_blend(blend_opaque_.get());
    l.set_program(gfx::Program::shadow_buffer);
    if (overlay_vertices_ && overlay_indices_) {
        l.set_vertex_buffer(overlay_vertices_.get(), sizeof(MeshVertex));
        l.set_index_buffer(overlay_indices_.get());
    }
    l.set_constants(gfx::stage_vertex | gfx::stage_pixel, 1, shadow_constants_.get());
    l.set_sampler(0, sampler_.get());
    // 132md8: a GPU-skinned caster surface (MeshSurface::skin) draws its model's buffers with the skinned program;
    // false: the surface is skipped (no indices, or its buffers were lost with the device).
    bool skinned = false;
    const SkinBuffers* skin_bound = nullptr;
    const gfx::Buffer* constants_bound = nullptr;
    const auto bind_surface = [&](const MeshSurface& surface) {
        if (!surface.skin) {
            if (skinned) {
                l.set_program(gfx::Program::shadow_buffer);
                l.set_vertex_buffer(overlay_vertices_.get(), sizeof(MeshVertex));
                l.set_index_buffer(overlay_indices_.get());
                skinned = false;
                skin_bound = nullptr;
            }
            if (!overlay_vertices_ || !overlay_indices_) {
                if (surface.index_count) throw std::runtime_error("Shadow slots need the caster overlay mesh");
                return false;
            }
            return true;
        }
        if (!surface.index_count) return false;
        const SkinBuffers* b = bind_skin(surface);
        if (!b) return false;
        if (!skinned) {
            l.set_program(gfx::Program::shadow_skinned);
            skinned = true;
            constants_bound = nullptr;
        }
        if (skin_current_ != constants_bound) {
            l.set_constants(gfx::stage_vertex, 2, skin_current_);
            constants_bound = skin_current_;
        }
        if (b != skin_bound) {
            l.set_vertex_buffer(b->vertices.get(), sizeof(gfx::GpuSkinVertex));
            l.set_index_buffer(b->indices.get());
            skin_bound = b;
        }
        return true;
    };
    for (std::size_t k = 0; k < buffers.size(); ++k) {
        const auto& slot = buffers[k];
        if (slot.type != 1 && slot.type != 2) throw std::runtime_error("Unsupported shadow slot type");
        const gfx::TargetView target{shadow_array_.get(), static_cast<unsigned>(k)};
        // Clears (003bb180): type 1 (0, 0, 0, 0x80), type 2 (0, 0, 0, 0).
        const float clear[4]{0, 0, 0, slot.type == 1 ? 128.0F / 255.0F : 0.0F};
        l.clear_target(target, clear);
        l.set_targets(1, &target, nullptr);
        ShadowConstants data{};
        std::copy(slot.clip.begin(), slot.clip.end(), data.clip);
        std::copy(slot.scale.begin(), slot.scale.end(), data.scale);
        std::copy(slot.offset.begin(), slot.offset.end(), data.offset);
        data.params[0] = slot.silhouette / 255.0F;
        data.params[1] = slot.type;
        const std::size_t surfaces = overlay_->surfaces.size();
        if (slot.first_surface > surfaces) throw std::runtime_error("Shadow caster surfaces outside the overlay");
        const std::size_t caster_end = slot.surface_count == 0xffffffffU ? surfaces : std::size_t{slot.first_surface} + slot.surface_count;
        if (caster_end > surfaces) throw std::runtime_error("Shadow caster surfaces outside the overlay");
        // 132md11: a skinned caster at its level of detail for this slot's 126-pixel buffer.
        const auto draw_caster = [&](const MeshSurface& surface) {
            if (!surface.skin || !skin_bound) return l.draw_indexed(surface.index_count, surface.first_index);
            const auto range = skin_range(surface, *skin_bound, slot.clip.data(), 126.0F * std::fabs(slot.scale[0]),
                                          126.0F * std::fabs(slot.scale[1]));
            l.draw_indexed(range[0], range[1]);
        };
        if (slot.type == 1) {
            l.update_buffer(shadow_constants_.get(), &data, sizeof data);
            for (std::size_t s = slot.first_surface; s < caster_end; ++s)
                if (bind_surface(overlay_->surfaces[s])) draw_caster(overlay_->surfaces[s]);
            continue;
        }
        for (std::size_t s = slot.first_surface; s < caster_end; ++s) {
            const auto& surface = overlay_->surfaces[s];
            if (!bind_surface(surface)) continue;
            const gfx::Texture* texture = nullptr;
            const auto index = overlay_->materials.at(surface.material).texture;
            if (index < overlay_textures_.size()) texture = overlay_textures_[index].get();
            data.params[3] = texture ? 1.0F : 0.0F;
            l.update_buffer(shadow_constants_.get(), &data, sizeof data);
            l.set_texture(0, texture);
            draw_caster(surface);
        }
    }
}

void SceneRenderer::draw_shadow_receivers(const gfx::Viewport& viewport, std::size_t first_surface,
                                          std::size_t surface_count) {
    if (shadows_->receivers.empty()) return;
    auto& l = list_;
    l.set_viewport(viewport);
    l.set_raster(solid_.get());
    l.set_depth(depth_test_only_.get());
    l.set_blend(blend_shadow_subtract_.get());
    l.set_vertex_buffer(vertices_.get(), sizeof(MeshVertex));
    l.set_index_buffer(indices_.get());
    l.set_program(gfx::Program::shadow_receiver);
    const gfx::Buffer* constants[2]{constants_.get(), shadow_constants_.get()};
    l.set_constants(gfx::stage_vertex | gfx::stage_geometry | gfx::stage_pixel, 0, 2, constants);
    l.set_texture(1, shadow_array_.get());
    l.set_sampler(1, shadow_sampler_.get());
    for (const auto& receiver : shadows_->receivers) {
        if (receiver.first_surface < first_surface || receiver.first_surface - first_surface >= surface_count) continue;
        if (std::size_t(receiver.first_surface) + receiver.surface_count > mesh_.surfaces.size())
            throw std::runtime_error("Shadow receiver surfaces outside the scene mesh");
        for (const auto& batch : receiver.batches) {
            for (const auto& slot : batch) {
                if (slot.type != 1 && slot.type != 2 && slot.type != 3) throw std::runtime_error("Unsupported shadow pass type");
                if (slot.type != 3 && slot.buffer >= shadows_->buffers.size())
                    throw std::runtime_error("Shadow pass names a missing buffer");
                bool raw_alpha = false;
                if (slot.type == 3) {
                    // A projector (003d4350): its texture, not a ShadowBuf.
                    if (slot.texture < 0 || std::size_t(slot.texture) >= projector_textures_.size()
                        || !projector_textures_[std::size_t(slot.texture)].first)
                        throw std::runtime_error("Shadow projector names a missing texture");
                    const auto& image = projector_textures_[std::size_t(slot.texture)];
                    l.set_texture(2, image.first.get());
                    raw_alpha = image.second;
                }
                // ALPHA 0xA1 (type 1) and 0x42 (type 3) subtract; 0x44 (type 2) blends.
                l.set_blend(slot.type == 2 ? blend_shadow_reflect_.get() : blend_shadow_subtract_.get());
                ShadowConstants data{};
                std::copy(slot.clip.begin(), slot.clip.end(), data.clip);
                std::copy(slot.scale.begin(), slot.scale.end(), data.scale);
                std::copy(slot.offset.begin(), slot.offset.end(), data.offset);
                std::copy(slot.facing.begin(), slot.facing.end(), data.facing);
                data.params[1] = slot.type;
                data.params[2] = slot.type == 3 ? 0.0F : static_cast<float>(slot.buffer);
                data.params[3] = raw_alpha ? 1.0F : 0.0F;
                l.update_buffer(shadow_constants_.get(), &data, sizeof data);
                for (std::uint32_t k = 0; k < receiver.surface_count; ++k) {
                    const auto& surface = mesh_.surfaces[receiver.first_surface + k];
                    // Material gate (batch state word 0): type 1 needs flag 0x10, type 2 flag 0x20, type 3 none.
                    if (slot.type != 3 && !(mesh_.materials.at(surface.material).flags & (slot.type == 1 ? 0x10U : 0x20U))) continue;
                    l.draw_indexed(surface.index_count, surface.first_index);
                }
            }
        }
    }
    const gfx::Texture* none[2]{};
    l.set_textures(1, 2, none);
}

void SceneRenderer::capture_shadow_buffer(std::size_t index, const std::filesystem::path& path) try {
    if (index >= shadow_buffer_layers) throw std::runtime_error("Shadow buffer index out of range");
    gfx::TextureDesc description;
    description.width = description.height = shadow_buffer_size;
    description.format = gfx::Format::rgba8;
    description.usage = gfx::Usage::staging;
    gfx::TextureLabels labels;
    labels.texture = "Create shadow buffer readback";
    const auto staging = device_->create_texture(description, nullptr, labels);
    list_.copy_region(staging.get(), 0, 0, 0, shadow_array_.get(), gfx::subresource(0, static_cast<unsigned>(index), 1), nullptr);
    device_->submit(list_);
    std::vector<std::uint8_t> pixels(std::size_t(shadow_buffer_size) * shadow_buffer_size * 4);
    device_->read(*staging, 0, [&](const std::uint8_t* data, unsigned row_pitch) {
        for (unsigned y = 0; y < shadow_buffer_size; ++y) {
            const auto* row = data + std::size_t(y) * row_pitch;
            for (unsigned x = 0; x < shadow_buffer_size; ++x) {
                // write_bmp takes BGRA like the swap chain; the buffer is RGBA.
                auto* out = pixels.data() + (std::size_t(y) * shadow_buffer_size + x) * 4;
                out[0] = row[x * 4 + 2]; out[1] = row[x * 4 + 1]; out[2] = row[x * 4 + 0]; out[3] = row[x * 4 + 3];
            }
        }
    }, "Read shadow buffer");
    write_bmp(path, shadow_buffer_size, shadow_buffer_size, pixels);
} catch (const DeviceLost&) {}

void SceneRenderer::targets() {
    target_ = swap_chain_->buffer(true, "Get swap-chain buffer", "Create render target");
    gfx::TextureDesc depth;
    depth.width = width_;
    depth.height = height_;
    depth.format = gfx::Format::d24s8;
    depth.bind = gfx::bind_depth;
    gfx::TextureLabels depth_labels;
    depth_labels.texture = "Create depth buffer";
    depth_labels.depth = "Create depth view";
    depth_ = device_->create_texture(depth, nullptr, depth_labels);
    gfx::TextureDesc alpha;
    alpha.width = width_;
    alpha.height = height_;
    alpha.format = gfx::Format::r8;
    alpha.bind = gfx::bind_target | gfx::bind_sampled;
    gfx::TextureLabels alpha_labels;
    alpha_labels.texture = "Create frame alpha target";
    alpha_labels.target = "Create frame alpha target view";
    alpha_labels.sampled = "Create frame alpha view";
    frame_alpha_ = device_->create_texture(alpha, nullptr, alpha_labels);
}

void SceneRenderer::release_targets() {
    list_.set_targets(0, nullptr, nullptr);
    device_->submit(list_);
    target_.reset();
    depth_.reset();
    frame_alpha_.reset();
    list_.flush();
    device_->submit(list_);
}

void SceneRenderer::resize(unsigned width, unsigned height) try {
    if (!width || !height) return;
    if (width > max_viewport || height > max_viewport) throw std::runtime_error("3D viewport exceeds preview limits");
    if (width == width_ && height == height_ && target_) return;
    if (lost()) return;  // port-only (132win2): the play re-creates the device, then reset_device sizes it
    release_targets();
    swap_chain_->resize(width, height, "Resize 3D viewport");
    width_ = width;
    height_ = height;
    targets();
} catch (const DeviceLost&) {}

void SceneRenderer::draw(const OrbitCamera& camera, const SceneOptions& options) try {
    // Port-only (132win2): on a removed device only the camera (last_view / last_projection) is kept.
    bool gone = lost();
    if (!target_ && !gone) {
        try {
            swap_chain_->size(width_, height_, "Read the swap chain");
            targets();
        } catch (const DeviceLost&) {  // 132win3: the camera below is still kept
            gone = true;
        }
    }
    const auto& center = camera.target;
    const float horizontal = camera.distance * std::cos(camera.pitch);
    const auto eye = XMVectorSet(center.x + horizontal * std::cos(camera.yaw),
                                 center.y + horizontal * std::sin(camera.yaw),
                                 center.z + camera.distance * std::sin(camera.pitch), 1);
    auto view = XMMatrixLookAtRH(eye, XMVectorSet(center.x, center.y, center.z, 1), XMVectorSet(0, 0, 1, 0));
    auto projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(55), frame_projection_aspect(width_, height_),
        std::max(0.005F, camera.radius * 0.001F), camera.distance + camera.radius * 4);
    Constants data{};
    XMStoreFloat4(&data.eye, eye);
    if (camera.view) {
        const auto& v = *camera.view;
        data.eye = {v.eye.x, v.eye.y, v.eye.z, 1};
        view = XMMatrixLookToRH(XMVectorSet(v.eye.x, v.eye.y, v.eye.z, 1), XMVectorSet(v.forward.x, v.forward.y, v.forward.z, 0),
            XMVectorSet(v.up.x, v.up.y, v.up.z, 0));
        // 132win10: the aspect setting's projection aspect (the target's; 4:3 when stretched, frame_fit.hpp)
        projection = XMMatrixPerspectiveFovRH(2 * v.vertical_half_fov, frame_projection_aspect(width_, height_), v.near_plane, v.far_plane);
    }
    XMStoreFloat4x4(&data.mvp, view * projection);
    {
        XMFLOAT4X4 v{}, p{};
        XMStoreFloat4x4(&v, view);
        XMStoreFloat4x4(&p, projection);
        std::memcpy(last_view_.data(), &v, sizeof v);
        std::memcpy(last_projection_.data(), &p, sizeof p);
    }
    if (gone) return;
    auto& l = list_;
    l.clear();
    skin_extents_.clear();
    skin_written_.assign(skin_written_.size(), false);
    if (shadows_) draw_shadow_buffers();
    // The surface texture (t0) is bound only when it changes (132mt1: the same state at every draw as binding it per
    // surface, with fewer commands; unknown until the first bind, null after a hook's unbind).
    const gfx::Texture* bound0 = nullptr;
    bool bound0_known = false;
    const auto bind0 = [&](const gfx::Texture* t) {
        if (bound0_known && bound0 == t) return;
        l.set_texture(0, t);
        bound0 = t;
        bound0_known = true;
    };
    const float background[]{0.025F, 0.032F, 0.045F, 1};
    l.clear_target({target_.get(), 0}, background);
    l.clear_depth(depth_.get(), gfx::CommandList::clear_depth_bit | gfx::CommandList::clear_stencil_bit, 1, 0);
    const float zero[]{0, 0, 0, 0};
    l.clear_target({frame_alpha_.get(), 0}, zero);   // 0037a5c0: RGBAQ +c8 A = 0
    const gfx::Texture* target = target_.get();
    l.set_target(target, depth_.get());
    const gfx::Viewport viewport{0, 0, float(width_), float(height_), 0, 1};
    l.set_viewport(viewport);
    data.options = {options.textured ? 1.0F : 0.0F, 0, options.vertex_lighting ? 1.0F : 0.0F, 0};
    const gfx::Buffer* constants = constants_.get();
    const auto update = [&] { l.update_buffer(constants, &data, sizeof data); };
    update();
    l.set_constants(gfx::stage_vertex | gfx::stage_pixel, 0, constants);
    l.set_program(gfx::Program::scene_model);
    l.set_vertex_buffer(vertices_.get(), sizeof(MeshVertex));
    l.set_index_buffer(indices_.get());
    l.set_raster(options.wireframe ? wire_.get() : solid_.get());
    l.set_depth(depth_enabled_.get());
    l.set_blend(blend_.get());
    l.set_sampler(0, sampler_.get());
    if (shadows_)
        for (const auto& receiver : shadows_->receivers)
            if (std::size_t(receiver.first_surface) + receiver.surface_count > mesh_.surfaces.size())
                throw std::runtime_error("Shadow receiver surfaces outside the scene mesh");
    bool texturing = options.textured;
    bool overlay_texturing = false;
    enum class Bound { main, overlay } bound = Bound::main;
    // 132md8: the skinned program and the skin model's buffers are bound (an overlay surface with MeshSurface::skin).
    bool skin_program = false;
    const SkinBuffers* skin_bound = nullptr;
    const gfx::Buffer* skin_constants_bound = nullptr;
    // The plan item's depth (SceneDrawItem::no_depth: TEST ZTST ALWAYS with the z write masked).
    const gfx::DepthState* main_depth = depth_enabled_.get();
    const gfx::DepthState* fb_depth = depth_test_only_.get();
    const auto main_state = [&] {
        data.options = {options.textured ? 1.0F : 0.0F, 0, options.vertex_lighting ? 1.0F : 0.0F, 0};
        data.alpha_test = {-1, 0, 0, 0};
        data.uv_offset = {0, 0, 0, 0};
        update();
        texturing = options.textured;
        l.set_program(gfx::Program::scene_model);
        l.set_constants(gfx::stage_vertex | gfx::stage_pixel, 0, constants);
        l.set_raster(options.wireframe ? wire_.get() : solid_.get());
        l.set_depth(main_depth);
        l.set_blend(blend_.get());
        l.set_sampler(0, sampler_.get());
        l.set_vertex_buffer(vertices_.get(), sizeof(MeshVertex));
        l.set_index_buffer(indices_.get());
        bound = Bound::main;
        skin_program = false;
        skin_bound = nullptr;
    };
    const auto draw_main_surface = [&](std::size_t s) {
            const auto& surface = mesh_.surfaces[s];
            const gfx::Texture* texture = nullptr;
            const gfx::Texture* raw_texture = nullptr;
            if (options.textured) {
                const auto index = mesh_.materials.at(surface.material).texture;
                const bool excluded = surface.material < untextured_materials_.size() && untextured_materials_[surface.material];
                if (!excluded && index < textures_.size()) {
                    // The texture object's current frame (+0x4c); frame 0 is the stored image.
                    const auto frame = index < texture_frame_index_.size() ? texture_frame_index_[index] : 0U;
                    if (frame == 0) texture = textures_[index].get();
                    else if (frame - 1 < texture_frames_[index].size()) texture = texture_frames_[index][frame - 1].get();
                    else throw std::runtime_error("Texture frame " + std::to_string(frame) + " outside the decoded animation");
                    if (frame < raw_alpha_textures_[index].size()) raw_texture = raw_alpha_textures_[index][frame].get();
                }
                if (!texture && !options.partial_textures) throw std::runtime_error("Textured draw has no decoded material texture");
                const bool want = texture != nullptr;
                if (want != texturing) {
                    data.options.x = want ? 1.0F : 0.0F;
                    update();
                    texturing = want;
                }
            }
            const auto& material = mesh_.materials.at(surface.material);
            // ALPHA_1 of the batch render-state packet (glows: 0x68 Cs*FIX + Cd).
            const auto blend = material.gs_alpha ? gs_blend_state(*material.gs_alpha) : GsBlendState{};
            // 0x48 / 0x42 (As * Cs) with texel alpha above 0x80: As exceeds 1.0
            // and the GS saturates (COLCLAMP); draw min(255, Cs * As >> 7)
            // with ONE/ONE (gs_premultiplied_source).
            const bool premultiplied = texture && raw_texture &&
                (blend.blend == GsBlend::source_alpha_add || blend.blend == GsBlend::source_alpha_subtract);
            bind0(premultiplied ? raw_texture : texture);
            // GS TEST of the model draw (0037c870 from the model flags 00377570
            // sets): header bit 0 (0x400000) ATE GREATER AREF 0x40 AFAIL
            // FB_ONLY (0037d374), others GREATER 0 AFAIL KEEP (0037d3a8 /
            // 0037d638). FB_ONLY pixels are drawn without the depth write.
            const bool fb_only = (material.model_flags & 1) != 0;
            const float aref = fb_only ? 64.0F : 0.0F;
            const float raw_flag = premultiplied ? 1.0F : 0.0F;
            if (data.alpha_test.x != aref || data.alpha_test.y != 0 || data.alpha_test.z != raw_flag) {
                data.alpha_test = {aref, 0, raw_flag, 0};
                update();
            }
            const auto uv = s < surface_uv_offsets_.size() ? surface_uv_offsets_[s] : std::array<float, 2>{};
            if (data.uv_offset.x != uv[0] || data.uv_offset.y != uv[1]) {
                data.uv_offset = {uv[0], uv[1], 0, 0};
                update();
            }
            if (blend.blend != GsBlend::source_alpha_over) {
                const gfx::BlendState* state = nullptr;
                switch (blend.blend) {
                case GsBlend::source_alpha_add: state = premultiplied ? blend_one_add_.get() : blend_alpha_add_.get(); break;
                case GsBlend::source_alpha_subtract:
                    state = premultiplied ? blend_one_subtract_.get() : blend_alpha_subtract_.get();
                    break;
                case GsBlend::fix_over: state = blend_fix_over_.get(); break;
                case GsBlend::fix_add: state = blend_add_half_.get(); break;
                case GsBlend::fix_subtract: state = blend_subtract_half_.get(); break;
                case GsBlend::source_alpha_over: break;
                }
                const float factor[4]{blend.fix, blend.fix, blend.fix, blend.fix};
                l.set_blend(state, factor);
            }
            l.draw_indexed(surface.index_count, surface.first_index);
            if (fb_only) {
                data.alpha_test.y = 1;
                update();
                l.set_depth(fb_depth);
                l.draw_indexed(surface.index_count, surface.first_index);
                l.set_depth(main_depth);
            }
            if (blend.blend != GsBlend::source_alpha_over) l.set_blend(blend_.get());
            if (!options.multipass || !texture || material.passes.size() < 2) return;
            // The later passes keep the earlier (untested) state.
            data.alpha_test = {-1, 0, 0, 0};
            data.uv_offset = {0, 0, 0, 0};
            // Later passes redraw the same strip with a new render state, as the
            // VU1 microcode re-kicks its output buffer (00377270 patches each
            // pass's MSCAL from the program table at 00538080).
            for (std::size_t k = 1; k < material.passes.size(); ++k) {
                const auto& pass = material.passes[k];
                float mode = 0;
                const gfx::BlendState* state = blend_add_half_.get();
                float factor = 0.5F;
                switch (pass.shader) {
                case 10: case 11: mode = 1; break;  // reflection map
                case 12: mode = 2; break;           // same coordinates
                case 14: mode = 3; state = blend_subtract_half_.get(); break;  // emboss
                case 2: {
                    // 132ws6: a later program-2 pass (VU1 0x156 re-run over the strip, as the base pass) with
                    // the pass texture and the ALPHA_1 of its section (BELARUS1 NI_BUCKET 0x68 FIX 0x0c..0x11
                    // Cs*FIX+Cd, MINSK doors' ENV_map 0x64 FIX 0x20 (Cs-Cd)*FIX+Cd).
                    if (!pass.gs_alpha) throw std::runtime_error("Program-2 material pass without its ALPHA_1");
                    const auto pass_blend = gs_blend_state(*pass.gs_alpha);
                    factor = pass_blend.fix;
                    switch (pass_blend.blend) {
                    case GsBlend::fix_add: break;
                    case GsBlend::fix_over: state = blend_fix_over_.get(); break;
                    case GsBlend::fix_subtract: state = blend_subtract_half_.get(); break;
                    default: throw std::runtime_error("Unsupported program-2 material pass ALPHA_1");
                    }
                    break;
                }
                default: throw std::runtime_error("Unsupported material pass shader " + std::to_string(pass.shader));
                }
                if (mode == 1 && !material.rigid_pose)
                    throw std::runtime_error("Reflection pass needs a rotation-only model placement");
                const gfx::Texture* pass_image = pass.texture < textures_.size() ? textures_[pass.texture].get() : nullptr;
                if (!pass_image) throw std::runtime_error("Material pass has no decoded texture");
                data.options.x = 1;
                data.options.w = mode;
                data.up = {material.model_up.x, material.model_up.y, material.model_up.z, 0};
                update();
                const float constant[4]{factor, factor, factor, factor};
                l.set_blend(state, constant);
                bind0(pass_image);
                l.draw_indexed(surface.index_count, surface.first_index);
            }
            data.options.w = 0;
            update();
            l.set_blend(blend_.get());
            texturing = true;
    };
    const auto draw_main = [&](std::size_t first, std::size_t count) {
        if (bound != Bound::main) main_state();
        if (first > mesh_.surfaces.size() || count > mesh_.surfaces.size() - first)
            throw std::runtime_error("Draw plan names surfaces outside the scene mesh");
        for (std::size_t s = first; s < first + count; ++s) draw_main_surface(s);
        if (!shadows_) return;
        const auto receiver_in_range = [&](const ShadowPassReceiver& receiver) {
            return receiver.first_surface >= first && receiver.first_surface - first < count;
        };
        if (std::none_of(shadows_->receivers.begin(), shadows_->receivers.end(), receiver_in_range)) return;
        // Receiver pass after the receivers' base and material passes; then
        // restore the main pipeline.
        data.alpha_test = {-1, 0, 0, 0};
        data.uv_offset = {0, 0, 0, 0};
        data.options.w = 0;
        update();
        draw_shadow_receivers(viewport, first, count);
        main_state();
    };
    const auto draw_overlay = [&](std::size_t first, std::size_t count) {
        if (count == 0) return;
        const bool overlay_buffers = overlay_vertices_ && overlay_indices_;
        if (!overlay_ || (!overlay_buffers && !has_overlay_skins()))
            throw std::runtime_error("Draw plan names overlay surfaces without an overlay mesh");
        if (first > overlay_->surfaces.size() || count > overlay_->surfaces.size() - first)
            throw std::runtime_error("Draw plan names surfaces outside the overlay mesh");
        if (bound != Bound::overlay) {
            data.options = {0, 0, 0, 0};
            data.alpha_test = {-1, 0, 0, 0};
            data.uv_offset = {0, 0, 0, 0};
            update();
            overlay_texturing = false;
            if (overlay_buffers) {
                l.set_vertex_buffer(overlay_vertices_.get(), sizeof(MeshVertex));
                l.set_index_buffer(overlay_indices_.get());
            }
            bound = Bound::overlay;
        }
        for (std::size_t s = first; s < first + count; ++s) {
            if (s >= overlay_skip_first_ && s - overlay_skip_first_ < overlay_skip_count_) continue;
            const auto& surface = overlay_->surfaces[s];
            // 132md8: a GPU-skinned surface draws its model's buffers with the skinned program (its constants in b2),
            // a CPU one the overlay's with the model program.
            if (surface.skin) {
                if (!surface.index_count) continue;
                const SkinBuffers* b = bind_skin(surface);
                if (!b) continue;
                if (!skin_program) {
                    l.set_program(gfx::Program::scene_skinned);
                    skin_program = true;
                    skin_constants_bound = nullptr;
                }
                if (skin_current_ != skin_constants_bound) {
                    l.set_constants(gfx::stage_vertex, 2, skin_current_);
                    skin_constants_bound = skin_current_;
                }
                if (b != skin_bound) {
                    l.set_vertex_buffer(b->vertices.get(), sizeof(gfx::GpuSkinVertex));
                    l.set_index_buffer(b->indices.get());
                    skin_bound = b;
                }
            } else {
                if (skin_program) {
                    l.set_program(gfx::Program::scene_model);
                    skin_program = false;
                    skin_bound = nullptr;
                    if (overlay_buffers) {
                        l.set_vertex_buffer(overlay_vertices_.get(), sizeof(MeshVertex));
                        l.set_index_buffer(overlay_indices_.get());
                    }
                }
                if (!overlay_buffers) {
                    if (surface.index_count) throw std::runtime_error("Draw plan names overlay surfaces without an overlay mesh");
                    continue;
                }
            }
            const gfx::Texture* texture = nullptr;
            const auto& overlay_material = overlay_->materials.at(surface.material);
            const auto index = overlay_material.texture;
            if (index < overlay_textures_.size()) texture = overlay_textures_[index].get();
            const float lighting = overlay_material.unlit ? 1.0F : 0.0F;
            if ((texture != nullptr) != overlay_texturing || data.options.z != lighting) {
                overlay_texturing = texture != nullptr;
                data.options.x = overlay_texturing ? 1.0F : 0.0F;
                data.options.z = lighting;
                update();
            }
            // The man model draw's TEST (GEQUAL AREF, AFAIL RGB_ONLY): the
            // passing pixels with the depth write, then the failing ones without.
            const auto in_range = [s](const OverlayGsAlpha& r) { return s >= r.first && s - r.first < r.count; };
            const OverlayGsAlpha* gs = overlay_gs_alpha_ && in_range(*overlay_gs_alpha_) ? &*overlay_gs_alpha_ : nullptr;
            for (std::size_t r = 0; !gs && r < overlay_gs_ranges_.size(); ++r)
                if (in_range(overlay_gs_ranges_[r])) gs = &overlay_gs_ranges_[r];
            const bool man_gs = gs != nullptr;
            const XMFLOAT4 test = man_gs ? XMFLOAT4{float(gs->aref), 0, 0, float(gs->vertex_alpha) + 1.0F}
                                         : XMFLOAT4{-1, 0, 0, 0};
            if (data.alpha_test.x != test.x || data.alpha_test.y != test.y || data.alpha_test.w != test.w) {
                data.alpha_test = test;
                update();
            }
            bind0(texture);
            // 132md11: a skinned part at its level of detail for the frame's size.
            std::array<unsigned, 2> range{surface.index_count, surface.first_index};
            if (surface.skin && skin_bound)
                range = skin_range(surface, *skin_bound, &data.mvp.m[0][0], 0.5F * float(width_), 0.5F * float(height_));
            l.draw_indexed(range[0], range[1]);
            if (man_gs) {
                data.alpha_test.y = 1;
                update();
                l.set_depth(depth_test_only_.get());
                l.draw_indexed(range[0], range[1]);
                l.set_depth(depth_enabled_.get());
                data.alpha_test.y = 0;
                update();
            }
        }
    };
    // Without a plan: every main surface (then the receiver passes), then the overlay.
    std::vector<SceneDrawItem> fallback;
    const auto* plan = &plan_;
    if (plan_.empty()) {
        fallback.push_back({SceneDrawItem::Source::main, 0, static_cast<std::uint32_t>(mesh_.surfaces.size())});
        if (overlay_ && ((overlay_vertices_ && overlay_indices_) || has_overlay_skins()))
            fallback.push_back({SceneDrawItem::Source::overlay, 0, static_cast<std::uint32_t>(overlay_->surfaces.size())});
        plan = &fallback;
    }
    for (const auto& item : *plan) {
        if (item.source == SceneDrawItem::Source::hook) {
            if (!draw_hook_) throw std::runtime_error("Draw plan has a hook item but no draw hook");
            // The hook draws with its own lists on the same device: everything recorded so far goes first.
            device_->submit(l);
            draw_hook_(item.first);
            // The hook's own pipeline: restore the scene's.
            l.set_target(target, depth_.get());
            l.set_viewport(viewport);
            const gfx::Texture* unbound[4]{};
            l.set_textures(0, 4, unbound);
            bound0 = nullptr;
            bound0_known = true;
            main_state();
            continue;
        }
        if (item.source == SceneDrawItem::Source::main) {
            const gfx::DepthState* want = item.no_depth ? depth_disabled_.get() : depth_enabled_.get();
            if (want != main_depth) {
                main_depth = want;
                fb_depth = item.no_depth ? depth_disabled_.get() : depth_test_only_.get();
                if (bound == Bound::main) l.set_depth(main_depth);
            }
            draw_main(item.first, item.count);
        } else if (item.no_depth) {
            throw std::runtime_error("Draw plan asks an overlay item for no depth");
        } else draw_overlay(item.first, item.count);
    }
    if (options.collision && collision_index_count_) {
        if (skin_program) l.set_program(gfx::Program::scene_model);
        data.options = {0, 1, 0, 0};
        update();
        l.set_vertex_buffer(collision_vertices_.get(), sizeof(MeshVertex));
        l.set_index_buffer(collision_indices_.get());
        l.set_raster(wire_.get());
        l.set_depth(depth_disabled_.get());
        l.draw_indexed(collision_index_count_, 0);
    }
    device_->submit(l);
} catch (const DeviceLost&) {
    list_.clear();
} catch (...) {
    list_.clear();  // nothing recorded by a failed draw is submitted later
    throw;
}

void SceneRenderer::refresh_geometry() try {
    auto vertices=buffer(mesh_.vertices.data(),mesh_.vertices.size()*sizeof(MeshVertex),gfx::bind_vertices,true);
    auto indices=buffer(mesh_.indices.data(),mesh_.indices.size()*sizeof(std::uint32_t),gfx::bind_indices);
    vertices_=std::move(vertices);indices_=std::move(indices);
} catch (const DeviceLost&) {}
void SceneRenderer::refresh_vertices(std::vector<std::pair<std::size_t, std::size_t>> ranges) {
    if (!vertices_ || !indices_ || vertices_->desc().bytes != mesh_.vertices.size() * sizeof(MeshVertex) ||
        indices_->desc().bytes != mesh_.indices.size() * sizeof(std::uint32_t)) {
        vertices_.reset();
        indices_.reset();
        refresh_geometry();
        return;
    }
    std::sort(ranges.begin(), ranges.end());
    std::size_t first = 0, end = 0;
    auto flush = [&] {
        if (end <= first) return;
        if (end > mesh_.vertices.size()) throw std::runtime_error("Vertex update outside the mesh");
        list_.update_buffer(vertices_.get(), mesh_.vertices.data() + first, (end - first) * sizeof(MeshVertex),
                            first * sizeof(MeshVertex), true);
    };
    for (const auto& [f, count] : ranges) {
        if (!count) continue;
        if (f > end || end == 0) {
            flush();
            first = f;
            end = f + count;
        } else {
            end = std::max(end, f + count);
        }
    }
    flush();
    device_->submit(list_);
}
void SceneRenderer::update_vertices(std::size_t first, std::size_t count) {
    if (first > mesh_.vertices.size() || count > mesh_.vertices.size() - first)
        throw std::runtime_error("Vertex update outside the mesh");
    if (!count || !vertices_) return;
    list_.update_buffer(vertices_.get(), mesh_.vertices.data() + first, count * sizeof(MeshVertex), first * sizeof(MeshVertex),
                        false);
    device_->submit(list_);
}
SceneRenderer::Ptr<gfx::Texture> SceneRenderer::upload_texture(const Texture& source) {
    const Texture& texture = upload_image(source);  // port-only (132md1): a mod's replacement image
    // An empty texture marks an explicitly absent image: its surfaces draw
    // untextured (base colour) when partial texturing is enabled.
    if (texture.width == 0 || texture.height == 0) return {};
    // RGBA8 levels; port-only (132md11): a mod's BC1 / BC3 blocks, decoded on a device without block textures.
    const auto upload = gfx::upload_levels(device_.get(), texture, true);
    gfx::TextureLabels labels;
    labels.texture = "Upload model texture";
    labels.sampled = "Create model texture view";
    try {
        return device_->create_texture(upload.desc, upload.levels.data(), labels);
    } catch (const DeviceLost& lost) {
        // The upload itself lost (port-only, 132win2): no image; reset_device re-makes it. A loss at its view throws.
        if (std::string_view(lost.what()).starts_with(labels.texture)) return {};
        throw;
    }
}
SceneRenderer::Ptr<gfx::Texture> SceneRenderer::upload_raw_alpha(const Texture& source) {
    // The same image with the GS texel alpha byte (Texture::raw_alpha) in
    // place of the min(255, 2 * a) display alpha; absent when no texel
    // exceeds 0x80 (the decoded alpha is then exact).
    if (source.raw_alpha.empty() || source.width == 0 || source.height == 0) return {};
    const Texture& texture = upload_image(source);  // port-only (132md1): a replacement carries its own raw alpha
    if (texture.raw_alpha.size() != 1 + texture.mips.size())
        throw std::runtime_error("Raw alpha level count mismatch");
    Texture copy = texture;
    copy.replacement.reset();
    const auto replace = [](std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& alpha) {
        if (rgba.size() != alpha.size() * 4) throw std::runtime_error("Raw alpha level size mismatch");
        for (std::size_t texel = 0; texel < alpha.size(); ++texel) rgba[texel * 4 + 3] = alpha[texel];
    };
    replace(copy.rgba, texture.raw_alpha[0]);
    for (std::size_t level = 0; level < copy.mips.size(); ++level) replace(copy.mips[level], texture.raw_alpha[level + 1]);
    return upload_texture(copy);
}
void SceneRenderer::set_overlay_textures(const std::vector<Texture>& textures) try {
    overlay_source_ = &textures;
    overlay_textures_.clear();
    for (const auto& texture : textures) overlay_textures_.push_back(upload_texture(texture));
} catch (const DeviceLost&) {}
void SceneRenderer::set_projector_textures(const std::vector<Texture>& textures) try {
    projector_source_ = &textures;
    projector_textures_.clear();
    for (const auto& texture : textures) {
        auto raw = upload_raw_alpha(texture);
        if (raw) projector_textures_.emplace_back(raw, true);
        else projector_textures_.emplace_back(texture.width && texture.height ? upload_texture(texture) : nullptr, false);
    }
} catch (const DeviceLost&) {}
void SceneRenderer::set_untextured_materials(std::vector<bool> materials) {
    untextured_materials_ = std::move(materials);
}
void SceneRenderer::set_overlay(const Mesh* mesh) {
    overlay_ = mesh;
    overlay_vertices_.reset();
    overlay_indices_.reset();
    if (overlay_) refresh_overlay();
}
const OverlaySkin& SceneRenderer::overlay_skin(const MeshSurface& surface) const {
    if (!overlay_skins_ || surface.skin > overlay_skins_->size())
        throw std::runtime_error("Overlay surface names a missing GPU skin");
    return (*overlay_skins_)[surface.skin - 1];
}
// Port-only (checkpoint 132md8, research/MODDING.md "Speed"): the programs are made by prepare_skinning (at the level's
// load when mods replace models: compiling them takes about a second) or else on the first skinned draw, the Skin
// constants on the first skinned draw (a run with no GPU-skinned part makes neither); a model's buffers on its first
// draw (immutable: its bind vertices and triangles), kept while the model lives. Each bind_skin records the skin's
// constants (b2: colour, thermal, palette rows) into list_ before its draw.
void SceneRenderer::prepare_skinning() {
    if (skin_ready_ || !gpu_skinning()) return;   // 132md9: a device without skinning makes no skinned program
    device_->prepare(gfx::Program::scene_skinned);
    device_->prepare(gfx::Program::shadow_skinned);
    skin_ready_ = true;
}
const SceneRenderer::SkinBuffers* SceneRenderer::bind_skin(const MeshSurface& surface) {
    const auto& skin = overlay_skin(surface);
    if (!skin.model) throw std::runtime_error("GPU skin has no model");
    if (skin.palette.size() > gfx::gpu_palette_limit) throw std::runtime_error("GPU skin palette past the limit");
    if (!gpu_skinning()) throw std::runtime_error("GPU skin on a device without GPU skinning (" + device_->name() + ")");
    prepare_skinning();
    constexpr std::size_t header_rows = 4;
    constexpr std::size_t block_rows = header_rows + gfx::gpu_palette_rows_per_bone * gfx::gpu_palette_limit;
    const std::size_t index = surface.skin - 1;
    if (skin_constants_.size() <= index) skin_constants_.resize(index + 1);
    if (skin_written_.size() <= index) skin_written_.resize(index + 1, false);
    auto& constants = skin_constants_[index];
    if (!constants)
        constants = device_->create_buffer({block_rows * sizeof(std::array<float, 4>), gfx::Usage::gpu, gfx::bind_constants},
                                           nullptr, "Create skin constants");
    auto found = skin_buffers_.find(skin.model.get());
    if (found != skin_buffers_.end() && found->second.model.lock() != skin.model) {
        skin_buffers_.erase(found);  // a new model at a freed one's address
        found = skin_buffers_.end();
    }
    if (found == skin_buffers_.end()) {
        for (auto k = skin_buffers_.begin(); k != skin_buffers_.end();)
            k = k->second.model.expired() ? skin_buffers_.erase(k) : std::next(k);
        const auto& model = *skin.model;
        if (model.vertices.empty() || model.triangles.empty()) return nullptr;
        const auto vertices = gfx::gpu_skin_vertices(model);
        std::size_t total = model.triangles.size();
        for (const auto& lod : model.lods) total += lod.triangles.size();
        std::vector<std::uint32_t> indices;
        indices.reserve(total * 3);
        for (const auto& t : model.triangles) indices.insert(indices.end(), t.begin(), t.end());
        SkinBuffers made;
        for (const auto& lod : model.lods) {   // 132md11: the levels after the full part
            made.lods.push_back({static_cast<unsigned>(indices.size()), static_cast<unsigned>(lod.triangles.size() * 3)});
            for (const auto& t : lod.triangles) indices.insert(indices.end(), t.begin(), t.end());
        }
        made.model = skin.model;
        made.vertices = buffer(vertices.data(), vertices.size() * sizeof(gfx::GpuSkinVertex), gfx::bind_vertices);
        made.indices = buffer(indices.data(), indices.size() * sizeof(std::uint32_t), gfx::bind_indices);
        if (!made.vertices || !made.indices) return nullptr;
        found = skin_buffers_.emplace(skin.model.get(), std::move(made)).first;
    }
    skin_current_ = constants.get();
    if (skin_written_[index]) return &found->second;
    skin_written_[index] = true;
    gfx::gpu_palette_rows(skin.palette, skin_rows_);
    skin_block_.assign(block_rows, {0, 0, 0, 0});
    skin_block_[0] = skin.color;
    skin_block_[1] = {skin.thermal ? 1.0F : 0.0F, 0, 0, 0};
    skin_block_[2] = {skin.thermal_u[0], skin.thermal_u[1], skin.thermal_u[2], 0};
    skin_block_[3] = {skin.thermal_v[0], skin.thermal_v[1], skin.thermal_v[2], 0};
    std::copy(skin_rows_.begin(), skin_rows_.end(), skin_block_.begin() + header_rows);
    list_.update_buffer(constants.get(), skin_block_.data(), skin_block_.size() * sizeof(std::array<float, 4>));
    return &found->second;
}
// Port-only (checkpoint 132md11, research/MODDING.md "Speed"): the screen scale is the projection's Jacobian at the part's
// box centre (its Frobenius norm, in pixels per world unit) times the centre's w over the nearest corner's, times the
// palette's largest axis scale (world units per model unit); any corner at or behind the eye draws the full part.
std::array<unsigned, 2> SceneRenderer::skin_range(const MeshSurface& surface, const SkinBuffers& buffers, const float* m,
                                                  float pixels_x, float pixels_y) {
    const std::array<unsigned, 2> own{surface.index_count, surface.first_index};
    if (buffers.lods.empty() || surface.first_index != 0) return own;
    const auto& skin = overlay_skin(surface);
    const auto& model = *skin.model;
    if (surface.index_count != model.triangles.size() * 3 || buffers.lods.size() != model.lods.size()) return own;
    if (skin_extents_.size() < overlay_skins_->size()) skin_extents_.resize(overlay_skins_->size());
    auto& extent = skin_extents_[surface.skin - 1];
    if (!extent.known) {
        extent.known = true;
        extent.bounded = gfx::replacement_bounds(model, skin.palette, extent.lo, extent.hi);
        extent.scale = 0;
        for (std::size_t b = 0; b < skin.palette.size() && b < model.bone_count; ++b)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float* row = &skin.palette[b].position[4 * axis];
                extent.scale = std::max(extent.scale, std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
            }
    }
    if (!extent.bounded) return own;
    const auto clip = [&](float x, float y, float z, std::size_t j) { return x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j]; };
    float nearest = std::numeric_limits<float>::infinity();
    for (std::size_t corner = 0; corner < 8; ++corner)
        nearest = std::min(nearest, clip(corner & 1 ? extent.hi[0] : extent.lo[0], corner & 2 ? extent.hi[1] : extent.lo[1],
                                         corner & 4 ? extent.hi[2] : extent.lo[2], 3));
    const float cx = 0.5F * (extent.lo[0] + extent.hi[0]), cy = 0.5F * (extent.lo[1] + extent.hi[1]),
                cz = 0.5F * (extent.lo[2] + extent.hi[2]);
    const float x = clip(cx, cy, cz, 0), y = clip(cx, cy, cz, 1), w = clip(cx, cy, cz, 3);
    if (!(nearest > 0) || !(w > 0)) return own;
    float sum = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        const float dx = pixels_x * (m[4 * k] * w - x * m[4 * k + 3]) / (w * w);
        const float dy = pixels_y * (m[4 * k + 1] * w - y * m[4 * k + 3]) / (w * w);
        sum += dx * dx + dy * dy;
    }
    const std::size_t level = gfx::replacement_lod(model, std::sqrt(sum) * (w / nearest) * extent.scale);
    if (!level) return own;
    return {buffers.lods[level - 1][1], buffers.lods[level - 1][0]};
}
// Port-only (checkpoint 132mt2): the overlay (the CPU-skinned characters, re-skinned every frame) lives in two dynamic
// buffers rewritten in place (Device::write, a whole-buffer discard) and re-made only when its arrays outgrow them;
// before, two immutable buffers were made every frame (2.3 % of the render pass, research/THREADING.md step 0).
// Pixel-neutral: the draws index the same bytes (the tail past the arrays is never read). 132md11: a buffer over four
// times the arrays' size (and past 1 MiB) is re-made at 1.5 times it: each write discards the whole buffer, so one
// large frame (a run's first mesh, made before the window's device is known, skins every replacement part on the CPU:
// 58 MB with the HD pack) cost every later frame its size in renaming (about 6 ms a frame).
void SceneRenderer::refresh_overlay() try {
    if (!overlay_) return;
    if (overlay_->vertices.empty() || overlay_->indices.empty()) { overlay_vertices_.reset(); overlay_indices_.reset(); return; }
    overlay_vertices_=rewrite_buffer(std::move(overlay_vertices_),overlay_->vertices.data(),overlay_->vertices.size()*sizeof(MeshVertex),
                                     gfx::bind_vertices,"Write overlay vertices");
    overlay_indices_=rewrite_buffer(std::move(overlay_indices_),overlay_->indices.data(),overlay_->indices.size()*sizeof(std::uint32_t),
                                    gfx::bind_indices,"Write overlay indices");
} catch (const DeviceLost&) {}
SceneRenderer::Ptr<gfx::Buffer> SceneRenderer::rewrite_buffer(Ptr<gfx::Buffer> buffer, const void* data, std::size_t size,
                                                             unsigned binding, const char* label) {
    if (!data || !size || size > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Invalid mesh GPU buffer size");
    const auto oversized = [&] { return buffer->desc().bytes > (std::size_t{1} << 20) && buffer->desc().bytes / 4 > size; };
    if (!buffer || buffer->desc().bytes < size || oversized()) {
        buffer.reset();
        const std::size_t capacity = std::min<std::size_t>(size + size / 2, std::numeric_limits<std::uint32_t>::max());
        buffer = device_->create_buffer({capacity, gfx::Usage::dynamic, binding}, nullptr, "Create overlay GPU buffer");
    }
    device_->write(*buffer, data, size, label);
    return buffer;
}
bool SceneRenderer::lost() const {
    return !device_ || device_->removed() || device_lost_pending();  // 132win3: a pending loss
}

bool SceneRenderer::present(unsigned sync_interval, bool sleep_when_occluded) {
    if (!swap_chain_) return false;
    if (device_lost_pending()) return false;  // 132win3: lost mid-frame; the owner re-creates the device
    const auto presented = swap_chain_->present(sync_interval, "Present 3D frame");
    if (presented == gfx::SwapChain::Presented::lost) return false;
    if (presented == gfx::SwapChain::Presented::occluded && sleep_when_occluded)
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 132win3: the frame pacer waits instead
    return true;
}

void SceneRenderer::capture(const std::filesystem::path& path) try {
    const auto back_buffer = swap_chain_->buffer(false, "Get capture buffer", nullptr);
    gfx::TextureDesc description = back_buffer->desc();
    description.usage = gfx::Usage::staging;
    description.bind = 0;
    gfx::TextureLabels labels;
    labels.texture = "Create frame readback";
    const auto staging = device_->create_texture(description, nullptr, labels);
    list_.copy(staging.get(), back_buffer.get());
    device_->submit(list_);
    std::vector<std::uint8_t> pixels;
    device_->read(*staging, 0, [&](const std::uint8_t* data, unsigned row_pitch) {
        if (row_pitch < width_ * 4) throw std::runtime_error("Invalid frame row pitch");
        pixels.resize(std::size_t(width_) * height_ * 4);
        for (unsigned y = 0; y < height_; ++y)
            std::memcpy(pixels.data() + std::size_t(y) * width_ * 4, data + std::size_t(y) * row_pitch, width_ * 4);
    }, "Read rendered frame");
    write_bmp(path, width_, height_, pixels);
} catch (const DeviceLost&) {}

}
