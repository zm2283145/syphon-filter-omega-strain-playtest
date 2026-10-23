#pragma once
// D3D11 drawing of the particle sprites (Checkpoint 122d, the TORONTO1
// office fire; research/EFFECTS.md "Checkpoint 122d office fire" and
// "Checkpoint 122e"). The sprites come from particle::particle_sprites: the
// batch cull and the GS SPRITE pairs of the VU1 programs 0x5e0 / 0x8a0, in
// the screen space of the frame block frame() builds; this is the host side
// of that boundary.
//
// Host choices (documented there):
// - The frame block (VU data 1..0xe, 00377c70 on the PS2): the cull matrix
//   (data 1..4, renderer+0xf00) and the sprite matrix (5..8, renderer+0xec0)
//   are both the node world * the scene's view * projection, with the
//   projection's D3D depth row turned into the -w..w convention CLIPW tests;
//   vf5 / vf6 (camera +0x90 / +0xa0) map clip to the host target in 1/16
//   pixel (X / 16 = column, Y / 16 = row down) and Z / 2^24 = D3D depth; the
//   sprite scale qword 0xe is 00377c70's (P00 / 2, P11 / 3, P22, P33) of the
//   scene projection.
// - Texture function MODULATE: colour = texel * RGBA / 128 (alpha 0x80 = 1),
//   bilinear, clamped; the GS alpha test GREATER 0 on the modulated alpha.
// - Blending by def+38 through the GS ALPHA_1 table 0053b680 (003a3910):
//   0 (Cs - Cd) * As + Cd, 1 Cs + Cd (C = FIX 0x80), 2 Cs * As + Cd,
//   3 Cd * (1 - As) (the D3D factors clamp As to 1; the GS allows up to
//   0xff / 0x80). Other values throw.
// - Depth: tested against the scene's depth buffer (GS ZTST GEQUAL with the
//   inverted GS depth), not written; batches and sprites in submission order.
// - Checkpoint 132g, the ShatterWindow shards (0039b7b0 -> 00371200, the
//   primitive-list boundary 003712a0 as for the GUI triangles): world
//   triangles under the Shatter asset's node, transformed by node * the
//   scene's view * projection on the host (D3D11 clipping stands in for the
//   VU1 program's); st = the 0025f220 word - 2048 (as gui_decor); colour =
//   the vertex colour (0053bb30, 1.0 = 0x80); the texture is the em+170 slot
//   (set_handle_texture; none = a 1 x 1 white texel); the ALPHA word
//   0x4bc1b0..c8 picks the blend: 0x44 / 0x68 / 0x48 as above and
//   0x42 = (0 - Cs) * As + Cd (D3D reverse subtract). Same alpha test,
//   depth state and sampler as the sprites; other ALPHA words throw.
// - Checkpoint 132cp4, heat haze (VU1 0x1cc8, FIRE_TO_LG02B): copy_screen_fx
//   is 0037bd40's ScreenFxBuf (the target down to 256 x 256, bilinear), taken
//   once per frame before the particle draws (0013e924 is after the opaque
//   lists, before the effect draws). Each haze sprite pair is one host quad
//   (ps_haze in the .cpp: sprite A's frame alpha computed in the shader, as
//   sprite B passes the depth test exactly where A did). frame() sets VU data
//   0x2c / 0x2d for the host frame (visible NDC -1..1: a = b = 1; W / H the
//   target size), which stands in for the PS2 camera's projection scale.
// - Checkpoint 132cp5: ScreenFxBuf is 256 x 256 PSMCT32 (003bb1c0 with the
//   renderer's 32 bpp; 0037e780 / 00165420 make the frame PSMCT32 640 x 448),
//   its alpha 0x80 (TCC 0, DECAL); the copy samples the GS's pixel positions
//   (i / 256 of the frame); data 0x2c's a = b = 1 is derived from 0037d9a0 (the
//   PS2 clip matrix carries 1 / a, 1 / b); ALPHA_1 0x54 with Ad up to 0xff is
//   blended in ps_haze from a per-sprite copy of the target (frame_cd).
// - Checkpoint 132cp7: sprite A's alpha is also written to the scene's
//   frame-alpha target (SceneRenderer::frame_alpha_target), which the corona
//   reduce of 003781b0 reads later in the frame. play_main calls copy_screen_fx
//   and draw() from the scene-draw hooks 0x37bd40 and 0x2b1cb0.
// Checkpoint 132mt1: every GPU call goes through the gfx layer (gfx_device.hpp; the D3D11 backend gfx_d3d11.cpp).
#include "gfx_device.hpp"
#include "particle_asset.hpp"
#include "scene_renderer.hpp"
#include "texture.hpp"

#include <array>
#include <map>

namespace omega {

// VU data 1..4 (the cull matrix) and 5..0xe for one node world.
struct ParticleFrame {
    std::array<particle::Qword, 4> cull{};
    particle::SpriteFrame sprite;
};

class ParticleRenderer {
public:
    explicit ParticleRenderer(SceneRenderer& scene);
    // Port-only (checkpoint 132win2): re-creates every device object on the scene's (re-created) device and
    // re-uploads the textures given so far (call after SceneRenderer::reset_device). Handle-slot images are
    // re-read through the pointers set_handle_texture was given, so they must still be alive.
    void reset_device();
    // The frame block for a node world (rows r0..r3, r3 = position) under
    // the scene's last draw() camera and target size.
    ParticleFrame frame(const std::array<std::array<float, 4>, 4>& world) const;
    // The definition's texture (def+74, "<name>.tdx" from PAR.HOG).
    void set_texture(const particle::ParticleDefinition* def, const Texture& texture);
    bool has_texture(const particle::ParticleDefinition* def) const { return textures_.count(def) != 0; }
    // Draws over the scene's last draw() (its camera, target and depth).
    // Returns the number of sprites drawn.
    std::size_t draw(const std::vector<particle::ParticleSpriteBatch>& batches);
    // 0037bd40: the scene target's current image to ScreenFxBuf (heat haze).
    void copy_screen_fx();
    // Checkpoint 132g: the image for an em+170 texture slot (null: white).
    void set_handle_texture(std::int32_t slot, const Texture* texture);
    bool has_handle_texture(std::int32_t slot) const { return handle_textures_.count(slot) != 0; }
    // The ShatterWindow flushes under the node world (rows, r3 = position);
    // returns the number of triangles drawn.
    std::size_t draw_shatter(const std::vector<particle::ShatterFlush>& flushes,
                             const std::array<std::array<float, 4>, 4>& node);
    // Checkpoint 132ms2: the rain emitter's triangle flushes (00399080 ->
    // 00371200, the same primitive path as the shards) with the def+74
    // texture (003820e0). Checkpoint 132ms5b: its sprite flushes (00371170,
    // the snow and splash-cloud drops) through particle::weather_sprite_batch
    // (VU1 0x5e0 under frame(node)) and draw(), in flush order; returns the
    // triangles drawn and adds the sprites drawn to *sprites.
    std::size_t draw_weather(const std::vector<particle::WeatherFlush>& flushes,
                             const std::array<std::array<float, 4>, 4>& node, std::size_t* sprites = nullptr);

private:
    struct TriangleFlush {
        const gfx::Texture* view = nullptr;
        std::uint64_t alpha = 0;
        const std::vector<particle::ShatterVertex>* vertices = nullptr;
    };
    std::size_t draw_triangles(const std::vector<TriangleFlush>& flushes,
                               const std::array<std::array<float, 4>, 4>& node, const char* what);
    template <class T> using Ptr = std::shared_ptr<T>;
    SceneRenderer& scene_;
    gfx::CommandList list_;
    Ptr<gfx::Buffer> vertices_;
    std::size_t vertex_capacity_ = 0;
    Ptr<gfx::BlendState> blend_[4];  // by def+38
    Ptr<gfx::RasterState> raster_;
    Ptr<gfx::DepthState> depth_;
    Ptr<gfx::SamplerState> sampler_;
    std::map<const particle::ParticleDefinition*, Ptr<gfx::Texture>> textures_;
    std::map<const particle::ParticleDefinition*, std::uint32_t> texture_alpha_;  // the largest GS texel alpha
    Ptr<gfx::Buffer> haze_vertices_, copy_vertices_;
    std::size_t haze_capacity_ = 0, copy_capacity_ = 0;
    Ptr<gfx::BlendState> haze_blend_;
    Ptr<gfx::DepthState> no_depth_;
    Ptr<gfx::Texture> frame_copy_, screen_fx_;   // frame_copy_: sampled; screen_fx_: sampled and a target
    Ptr<gfx::Buffer> clip_vertices_;
    std::size_t clip_capacity_ = 0;
    Ptr<gfx::BlendState> reverse_subtract_;  // ALPHA 0x42
    std::map<std::int32_t, Ptr<gfx::Texture>> handle_textures_;
    Ptr<gfx::Texture> make_view(const Texture& texture);
    std::map<const particle::ParticleDefinition*, Texture> texture_images_;  // for reset_device
    std::map<std::int32_t, const Texture*> handle_images_;
    void create_pipeline();
    static Texture white_texel();
    bool lost() const;  // the scene's device was removed (draws are skipped until reset_device)
};

}  // namespace omega
