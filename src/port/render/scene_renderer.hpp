#pragma once
#include <optional>

#include "geometry.hpp"
#include "gfx_device.hpp"
#include "skinned_mesh.hpp"
#include "texture.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace omega {

// Explicit view (e.g. the native game camera): eye, forward and up vectors in
// world space and the vertical half angle of the perspective projection.
struct ExplicitView {
    Vec3 eye, forward, up;
    float vertical_half_fov{};
    float near_plane{0.25F}, far_plane{4000.0F};
};

struct OrbitCamera {
    Vec3 target;
    std::optional<ExplicitView> view; // overrides the orbit when present
    float radius{};
    float distance{};
    float yaw{-0.85F};
    float pitch{0.65F};
    explicit OrbitCamera(const Bounds& bounds);
    void orbit(float horizontal, float vertical);
    void zoom(float steps);
};

struct SceneOptions {
    bool textured{};
    bool wireframe{};
    bool collision{};
    // With textured: surfaces whose texture is absent or excluded draw with
    // base colours instead of failing.
    bool partial_textures{};
    // Use only the stored vertex colours (no diagnostic directional light) for
    // the main mesh; the overlay keeps the diagnostic light.
    bool vertex_lighting{};
    // With textured: draw the later passes of textured multipass materials
    // (VU1 programs 10/11 reflection map and 12 at Cd + 0.5*Cs, program 14
    // emboss at Cd - 0.5*Cs). Other pass shaders are rejected.
    bool multipass{};
};

// Character shadows (CHARACTER_SHADOWS.md sections 5 and 6, LEVEL_LIGHTS.md).
// Matrices are 16 floats in column order (column c, row k at c*4+k), applied
// to column vectors: X = M * (position, 1). Each slot's texture lookup is
// lane-wise value = offset + scale * X / X.w (u, v, then the vertex colour I
// in lane z), as the shadow VU routine computes STQ and RGBA.
struct ShadowBufferDraw {
    std::uint8_t type{};                 // 1 shadow (silhouette), 2 reflection (the character drawn normally)
    std::array<float,16> clip{};         // slot clip (world -> guarded projector clip)
    std::array<float,4> scale{}, offset{};
    float silhouette{};                  // type 1: buffer colour byte (T*a3 >> 7)
    // The caster: overlay surfaces [first_surface, first_surface + surface_count)
    // (one character of several in the overlay mesh; the default is all of them).
    std::uint32_t first_surface{}, surface_count{0xffffffffU};
};
struct ShadowPassSlot {
    std::size_t buffer{};                // index into ShadowDrawFrame::buffers (types 1 and 2)
    std::uint8_t type{};                 // 1, 2, or 3 (a projector: no buffer, its own texture)
    std::int32_t texture{-1};            // type 3: index into set_projector_textures
    std::array<float,16> clip{};         // slot clip * receiver transform (0037c870)
    std::array<float,4> scale{}, offset{}, facing{};
};
struct ShadowPassReceiver {
    std::uint32_t first_surface{}, surface_count{};   // the receiver instance's surfaces in the main mesh
    std::vector<std::vector<ShadowPassSlot>> batches; // each batch is a redraw of the receiver
};
struct ShadowDrawFrame {
    std::vector<ShadowBufferDraw> buffers;
    std::vector<ShadowPassReceiver> receivers;
};

// One step of an explicit draw order (scene_draw_order: the scene frame
// 0013e1b0's bucket walks): main-mesh surfaces [first, first + count), then
// the shadow receiver passes of the receivers whose first surface lies in
// that range (the receiver pass follows the receiver's own model draw); or
// overlay surfaces [first, first + count).
// Overlay surfaces [first, first + count) drawn with the man model draw's GS
// state (Checkpoint 118e, man_draw_gs_state): RGBAQ.A = vertex_alpha, TEST
// ATST GEQUAL aref / AFAIL RGB_ONLY, ALPHA 0x44.
struct OverlayGsAlpha {
    std::uint32_t first{}, count{};
    std::uint8_t vertex_alpha{0x80}, aref{0x80};
};

namespace gfx {
struct ReplacementModel;
}
// Port-only (checkpoint 132md8, research/MODDING.md "Speed"): a mod replacement part skinned on the GPU. An overlay
// surface with skin k (MeshSurface::skin) draws skin k - 1's model (its bind vertices and triangles, uploaded once
// per model) skinned by `palette` in the vertex program, coloured `color` (or, in thermal, white with the ST from the
// skinned normal and the light rows thermal_u / thermal_v: skm_thermal_uv), with the surface's material as any other.
struct OverlaySkin {
    std::shared_ptr<const gfx::ReplacementModel> model;
    std::vector<SkinPaletteBone> palette;
    std::array<float, 4> color{1, 1, 1, 1};
    bool thermal = false;
    std::array<float, 3> thermal_u{}, thermal_v{};
};

struct SceneDrawItem {
    // hook (Checkpoint 132cp7): a non-model step of the scene frame at this position; `first` is its id (the
    // original's call address), passed to the draw hook (set_draw_hook).
    enum class Source : std::uint8_t { main, overlay, hook } source{};
    std::uint32_t first{}, count{};
    // Main items only: no depth test or write (the sky draw 003bc280: TEST ZTST ALWAYS, ZBUF ZMSK).
    bool no_depth{};
};

// A display the renderer draws into (gfx_device.hpp). Shared (Checkpoint 132win, port-only: the session's one
// game window, host_window.hpp HostWindow::scene_display): the renderer makes its own target and depth views of
// the swap chain's buffer and resizes it, but never creates or releases the device. Owned (gfx::d3d11::
// create_window_display: the scene / actor tools' own window): made for this renderer alone.
// Checkpoint 132mt1: every GPU call goes through the gfx layer (gfx_device.hpp; the D3D11 backend gfx_d3d11.cpp).
using SceneDisplay = gfx::Display;

class SceneRenderer {
public:
    // Draws into the display, sized from its swap chain.
    SceneRenderer(const SceneDisplay& display, const Mesh& mesh, const CollisionMesh& collision,
                  const std::vector<Texture>& textures);
    // Port-only (checkpoint 132win2, device-lost recovery): re-creates every device object on a new shared
    // display (the session window's re-created device) and re-uploads the level from the CPU-side data it was
    // given: the mesh (current vertices), the constructor's textures and the last set_overlay_textures /
    // set_projector_textures vectors, which must therefore still be alive. Shared-display renderers only.
    void reset_device(const SceneDisplay& display);
    // The first half of reset_device: drops every device object, the shared swap chain and its buffer views
    // included, so the window can make a new swap chain for its HWND. Draws are skipped until reset_device.
    void release_device();
    void resize(unsigned width, unsigned height);
    // Unbinds and releases the target and depth views (the swap chain's buffers may then be resized by
    // another renderer of a shared display); resize() or the next draw() makes them again.
    void release_targets();
    void draw(const OrbitCamera& camera, const SceneOptions& options);
    // Refresh a CPU-skinned mesh after its existing vertex/index arrays change.
    void refresh_geometry();
    // Re-upload vertices [first, first + count) of the main mesh after their
    // values (not the array) change.
    void update_vertices(std::size_t first, std::size_t count);
    // Optional second mesh (e.g. a CPU-skinned character) drawn untextured after
    // the main mesh. The renderer keeps the pointer; call refresh_overlay after
    // its arrays change. nullptr removes it.
    void set_overlay(const Mesh* mesh);
    void refresh_overlay();
    // 132md8: the skins the overlay's surfaces name (kept as a pointer, read at each draw; null: none).
    void set_overlay_skins(const std::vector<OverlaySkin>* skins) { overlay_skins_ = skins; }
    // 132md8: compile the GPU skinning programs now (once; else on the first skinned draw). 132md9: nothing on a device
    // without GPU skinning (gpu_skinning()).
    void prepare_skinning();
    // 132md9 (research/RENDERER.md "GPU skinning"): the device implements the skinned programs
    // (gfx::Device::capabilities); false: every part must be skinned on the CPU (an OverlaySkin surface throws).
    bool gpu_skinning() const { return gfx::gpu_skinning(device_.get()); }
    // 132md9: whether the GPU skinning programs were made (prepare_skinning or a skinned draw); for tests.
    bool skinning_prepared() const { return skin_ready_; }
    // Materials (by index) drawn untextured even in textured mode.
    void set_untextured_materials(std::vector<bool> materials);
    // Per texture (by index): the texture object's current animation frame
    // (+0x4c, advanced by 003d1960); 0 or absent draws the stored frame.
    void set_texture_frames(std::vector<std::uint32_t> frames) { texture_frame_index_ = std::move(frames); }
    // Per main-mesh surface (VUM mesh record): the q1.xy of its VU1 packet
    // (shader 13 flag 2, UV scroll 003d1c80), subtracted from the texture
    // coordinates. Absent entries draw unshifted.
    void set_surface_uv_offsets(std::vector<std::array<float, 2>> offsets) { surface_uv_offsets_ = std::move(offsets); }
    // Images for overlay materials (by material texture index); empty = untextured.
    void set_overlay_textures(const std::vector<Texture>& textures);
    // Images of the type-3 shadow projector slots (ShadowPassSlot::texture),
    // stored with their raw GS alpha byte when the decode kept one.
    void set_projector_textures(const std::vector<Texture>& textures);
    // Overlay surfaces [first, first + count) left out of the main pass only
    // (shadow buffers still draw them): a model whose draw the game skipped
    // this frame. count 0 (the default) draws every surface.
    void set_overlay_main_skip(std::size_t first, std::size_t count) { overlay_skip_first_ = first; overlay_skip_count_ = count; }
    // The man model draw's alpha for the next draw() calls; nullopt draws opaque.
    void set_overlay_gs_alpha(std::optional<OverlayGsAlpha> alpha) { overlay_gs_alpha_ = alpha; }
    // Further overlay ranges drawn with the same GS state (the gear model draw
    // 003836b0: dropped backpacks and thrown projectiles, Checkpoint 132o2).
    void set_overlay_gs_ranges(std::vector<OverlayGsAlpha> ranges) { overlay_gs_ranges_ = std::move(ranges); }
    // The draw order of the next draw() calls. Empty (the default): every
    // main surface, then every shadow receiver pass, then the overlay.
    void set_draw_plan(std::vector<SceneDrawItem> plan) { plan_ = std::move(plan); }
    // Checkpoint 132cp7: called by draw() for each hook item with its id, between the model draws around it. The
    // hook may draw with its own pipeline over render_target() / depth_view() / frame_alpha_target(); draw()
    // restores its own state afterwards. A plan with hook items and no hook throws.
    void set_draw_hook(std::function<void(std::uint32_t)> hook) { draw_hook_ = std::move(hook); }
    // Shadows drawn by the next draw() calls: the overlay mesh (world space) is
    // rendered into one 128x128 buffer per slot, then each receiver's
    // surfaces are redrawn per slot with the receiver pass. The renderer keeps
    // the pointer; nullptr disables shadows.
    void set_shadows(const ShadowDrawFrame* frame);
    // Writes shadow buffer `index` of the last draw as a 128x128 BMP.
    void capture_shadow_buffer(std::size_t index, const std::filesystem::path& path);
    // False when the display device was removed or reset (the frame is lost; other failures throw). The sync
    // interval is the frame pacer's (132win3, frame_pacer.hpp: 0 vsync off, n a whole multiple of the rate); an
    // occluded window sleeps 30 ms unless the caller paces the loop itself.
    bool present(unsigned sync_interval = 1, bool sleep_when_occluded = true);
    void capture(const std::filesystem::path& path);
    const std::string& backend() const { return backend_; }
    // For a 2D pass over the last draw() (the GUI overlay, Checkpoint 122a).
    const std::shared_ptr<gfx::Device>& device() const { return device_; }
    const gfx::Texture* render_target() const { return target_.get(); }
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    // The last draw()'s view and projection (row vectors: clip = p * view *
    // projection, rows stored in order) and its depth buffer, for the
    // particle sprites (Checkpoint 122d).
    const std::array<float, 16>& last_view() const { return last_view_; }
    const std::array<float, 16>& last_projection() const { return last_projection_; }
    const gfx::Texture* depth_view() const { return depth_.get(); }
    // Checkpoint 132cp7: the GS frame's alpha byte (R8, the target's size). The frame clear 0037a5c0 (from the
    // flip 0037a9f0, once per frame) writes RGBAQ +c8 A = 0 (init 0037f5f0), so draw() clears it to 0. The model
    // draws never write it: the vision prepare 0025fbb0 ORs FBMSK +f8 (0xff000000 in mode 0, 0xffff00ff in mode 1,
    // 0xff00ffff in mode 2) into FRAME_1 / FRAME_2 (+90 / +98) every frame. Its hosted writers are the heat-haze
    // sprite A and the corona probe; the corona reduce reads it. One texture: its render target and its view.
    const gfx::Texture* frame_alpha_target() const { return frame_alpha_.get(); }
    const gfx::Texture* frame_alpha_view() const { return frame_alpha_.get(); }

private:
    std::array<float, 16> last_view_{}, last_projection_{};
    template <class T> using Ptr = std::shared_ptr<T>;
    const Mesh& mesh_;
    bool owned_{};   // the display was made for this renderer (its own window): no release / reset
    unsigned width_{}, height_{};
    std::string backend_;
    Ptr<gfx::Device> device_;
    Ptr<gfx::SwapChain> swap_chain_;
    gfx::CommandList list_;   // recorded by draw() and the others, submitted before they return or call out
    Ptr<gfx::Texture> target_;
    Ptr<gfx::Texture> depth_;
    Ptr<gfx::Texture> frame_alpha_;
    std::function<void(std::uint32_t)> draw_hook_;
    Ptr<gfx::Buffer> constants_;
    Ptr<gfx::Buffer> vertices_;
    Ptr<gfx::Buffer> indices_;
    Ptr<gfx::Buffer> collision_vertices_;
    Ptr<gfx::Buffer> collision_indices_;
    unsigned collision_index_count_{};
    const Mesh* overlay_{};
    std::size_t overlay_skip_first_{}, overlay_skip_count_{};
    std::optional<OverlayGsAlpha> overlay_gs_alpha_;
    std::vector<OverlayGsAlpha> overlay_gs_ranges_;
    std::vector<bool> untextured_materials_;
    std::vector<Ptr<gfx::Texture>> overlay_textures_;
    std::vector<std::pair<Ptr<gfx::Texture>, bool>> projector_textures_;   // view, raw alpha
    Ptr<gfx::Buffer> overlay_vertices_;
    Ptr<gfx::Buffer> overlay_indices_;
    // 132md8: GPU-skinned overlay surfaces (OverlaySkin): each model's buffers (made on first use, dropped with the
    // model), the Skin constants (b2) and the programs, prepared on the first skinned draw.
    struct SkinBuffers {
        std::weak_ptr<const gfx::ReplacementModel> model;
        Ptr<gfx::Buffer> vertices, indices;
        // 132md11: model.lods[k]'s (first index, index count) in `indices` (after the full part's triangles).
        std::vector<std::array<unsigned, 2>> lods;
    };
    // 132md11: per skin of this frame (by OverlaySkin index), its screen scale's inputs (gfx::replacement_bounds and the
    // palette's largest axis scale); made by the first draw that needs them, cleared by draw().
    struct SkinExtent {
        bool known = false, bounded = false;
        std::array<float, 3> lo{}, hi{};
        float scale = 0;
    };
    std::vector<SkinExtent> skin_extents_;
    const std::vector<OverlaySkin>* overlay_skins_{};
    std::map<const gfx::ReplacementModel*, SkinBuffers> skin_buffers_;
    // 132md11: the Skin constants per skin of this frame (by OverlaySkin index), recorded into list_ by the frame's first
    // draw of that skin (skin_written_, cleared by draw()) and bound by the later ones (the shadow and main passes and the
    // man draw's second half share one upload); skin_current_ is the last bind_skin's.
    std::vector<Ptr<gfx::Buffer>> skin_constants_;
    std::vector<bool> skin_written_;
    gfx::Buffer* skin_current_{};
    std::vector<std::array<float, 4>> skin_rows_, skin_block_;
    bool skin_ready_{};
    bool has_overlay_skins() const { return overlay_skins_ && !overlay_skins_->empty(); }
    const OverlaySkin& overlay_skin(const MeshSurface& surface) const;
    // The skinned surface's model buffers (null: the device was lost); its skin's constants (skin_current_) recorded into
    // list_ if this frame has not yet.
    const SkinBuffers* bind_skin(const MeshSurface& surface);
    // 132md11 (research/RENDERER.md "GPU skinning"): the index range (count, first) a skinned surface draws: an automatic
    // level of detail (gfx::replacement_lod) when the surface is its whole model and the model has levels, by the
    // part's size on screen through `clip` (row vectors: X = (p, 1) * clip; X.x / X.w, X.y / X.w times pixels_x,
    // pixels_y in pixels); else the surface's own.
    std::array<unsigned, 2> skin_range(const MeshSurface& surface, const SkinBuffers& buffers, const float* clip, float pixels_x,
                                       float pixels_y);
    Ptr<gfx::RasterState> solid_;
    Ptr<gfx::RasterState> wire_;
    Ptr<gfx::DepthState> depth_enabled_;
    Ptr<gfx::DepthState> depth_disabled_;
    Ptr<gfx::BlendState> blend_;
    Ptr<gfx::BlendState> blend_add_half_;
    Ptr<gfx::BlendState> blend_subtract_half_;
    // Base-pass GS ALPHA render states other than the default 0x44 (blend_).
    Ptr<gfx::BlendState> blend_alpha_add_, blend_alpha_subtract_, blend_fix_over_;
    // 0x48 / 0x42 with a texture whose alpha exceeds 0x80: the shader outputs
    // min(1, Cs * As / 128) (gs_premultiplied_source), blended ONE/ONE.
    Ptr<gfx::BlendState> blend_one_add_, blend_one_subtract_;
    std::vector<std::vector<Ptr<gfx::Texture>>> raw_alpha_textures_;   // [texture][frame], Texture::raw_alpha
    std::vector<SceneDrawItem> plan_;
    Ptr<gfx::SamplerState> sampler_;
    std::vector<Ptr<gfx::Texture>> textures_;
    std::vector<std::vector<Ptr<gfx::Texture>>> texture_frames_;   // frames 1.. per texture
    std::vector<std::uint32_t> texture_frame_index_;
    std::vector<std::array<float, 2>> surface_uv_offsets_;

    const ShadowDrawFrame* shadows_{};
    Ptr<gfx::Texture> shadow_array_;   // 32 layers, a target per layer, one view of the array
    Ptr<gfx::Buffer> shadow_constants_;
    Ptr<gfx::BlendState> blend_opaque_, blend_shadow_subtract_, blend_shadow_reflect_;
    Ptr<gfx::DepthState> depth_test_only_;
    Ptr<gfx::SamplerState> shadow_sampler_;
    void create_shadow_resources();
    void draw_shadow_buffers();
    void draw_shadow_receivers(const gfx::Viewport& viewport, std::size_t first_surface = 0,
                               std::size_t surface_count = SIZE_MAX);
    Ptr<gfx::Texture> upload_raw_alpha(const Texture& texture);

    Ptr<gfx::Buffer> buffer(const void* data, std::size_t size, unsigned binding, bool updatable = false);
    // 132mt2: a dynamic buffer of at least `size` bytes (`buffer` when it is large enough) with its first bytes rewritten.
    Ptr<gfx::Buffer> rewrite_buffer(Ptr<gfx::Buffer> buffer, const void* data, std::size_t size, unsigned binding, const char* label);
    Ptr<gfx::Texture> upload_texture(const Texture& texture);
    void targets();
    void initialize(const CollisionMesh& collision, const std::vector<Texture>& textures);
    bool lost() const;  // the device was removed: draws are skipped until reset_device (port-only, 132win2)
    // The CPU-side sources reset_device re-uploads (shared-display renderers).
    const std::vector<Texture>* textures_source_{};
    const std::vector<Texture>* overlay_source_{};
    const std::vector<Texture>* projector_source_{};
    std::vector<MeshVertex> collision_points_;
    std::vector<std::uint32_t> collision_index_data_;
};

}
