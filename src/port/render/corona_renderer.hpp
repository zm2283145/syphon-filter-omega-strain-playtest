#pragma once
// Checkpoint 132cp6: D3D11 drawing of the corona sprite draw 003781b0 (renderer 005381f0; the native packet writer
// is src/main/lights/corona_draw.cpp, research/FLASHLIGHT.md §17 and FRONTEND.md "Checkpoint 132cp6"). Its callers
// are the GOB glow 00378170 (0025f270 / 0025f230) and the light corona pass 0025f4c0 (vision_corona.hpp).
//
// What 003781b0 does per call, as hosted here:
// - clip: the point under the projection with P00 / P11 divided by 1.5 (00379ac0 copy), any of the six VCLIPw bits
//   rejects: |ndc x| or |ndc y| > 1.5, or the view depth outside near..far; bit 0x40 (00379e20) is the depth at or
//   past the fog near (+1b8 = P (0, 0, +158, 1), +158 the environment +0x60);
// - fog fade: with bit 0x40 and the draw mode 005392d0 & 0x180, f1 = (128 / 255) (+180 + +190 / w), nothing at
//   f1 <= 0, clamped to 128, fade = f1 / 128;
// - the screen point in GS pixels x = int(16 gx) >> 4 (gx = 0x800 + 320 x_view / (t w), t the tangent of the
//   camera's half angle +100), the box int(40 / (t w)) capped at 0x28, the point clamped by the box into the frame;
// - the probe (FRAME_1 alpha only through ALPHA 0x6a, TEST_1 0x51001 = ZTST GEQUAL, AFAIL FB_ONLY): alpha 0 over
//   the clamped box at the nearest depth, alpha 0xff over the unclamped box at the point's depth;
// - the reduce (context 2, FBMSK 0xffffff, TEX0_2 the frame with TCC, DECAL, TEX1_2 0x60 bilinear): the clamped box
//   from UV (cx - box + 1) .. (cx + box - 1) to 32 x 32 at (0, 64), then 16, 8, 4, 2 square (UV +1 per step: each
//   pixel the average of a 2 x 2), and the 2 x 2 to the element texture's alpha (UV (1, 193): their average), so
//   the texture must be PSMCT32 (its TEX0 TCC: 003bba10 sets it for 32 bpp);
// - per element i: centre (x - ax offset[i], y - ay offset[i]) with the axis (0x800 - x) / hw, (0x800 - y) / hh
//   (00132000), half size int(half scale[i] 80 / (t w)) by int(half scale[i] 56 / (t w)), skipped outside the
//   003751c0 / 003751e0 limits; RGBA int(128 c), alpha int(128 fade c.w), skipped at <= 0; PRIM 0x156 (sprite, TME,
//   ABE, FST), MODULATE: Cs = T Cf >> 7, As = vis Af >> 7; ALPHA 0x48: Cd + Cs As >> 7 (COLCLAMP 1), no depth;
//   with the glare flag element 0 adds the full-screen sprite at the texture's centre texel, alpha
//   int(120 (1 - |axis|)^3) (when the caller's sprite byte is set).
//
// Host choices:
// - The GS frame is the virtual 0x800 +- hw by 0x800 +- 224 with hw = 240 W / H (320 at 4:3), so a GS pixel is
//   W / (2 hw) by H / 448 host pixels; t = 1 / (0.75 P11) of the scene's projection (the window keeps the PS2's 4:3
//   vertical framing, play_main). The element limits 003751c0 / 003751e0 are hw 4095 / 640 = 2047 at the 640 x 448
//   frame (0037e780: +68 = 4095 / width, +78 = 4095 / height), i.e. the GS's 4096 range.
// - The probe goes to the scene's frame-alpha target (SceneRenderer::frame_alpha_target, checkpoint 132cp7: the
//   GS frame alpha, cleared to 0 by 0037a5c0 once a frame, never written by the models under 0025fbb0's FBMSK,
//   written by the heat-haze sprite A and by every probe before this one in the frame), depth tested against the
//   scene's depth (LESS_EQUAL, D3D depth of the point under the scene's view and projection, not written). A call
//   whose box is 0 (beyond about 67 units) probes nothing and its reduce reads what is there, as on the GS.
// - The reduce samples that target bilinearly at the GS positions scaled to the host target and truncates each
//   level to 8 bits; one R32F texel per call, read by the element sprites.
// - draw() runs at its caller's scene-draw position (the SceneRenderer draw hooks 0x25f230 / 0x25f270 / 0x25f4c0).
// - The element texture is the TDX's frame 0 (rgb as decoded); its alpha is replaced by the visibility as on the GS.
// Checkpoint 132mt1: every GPU call goes through the gfx layer (gfx_device.hpp; the D3D11 backend gfx_d3d11.cpp).
#include "gfx_device.hpp"
#include "scene_renderer.hpp"
#include "texture.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace omega {

// One 003781b0 call: position (world), the `count` elements' texture keys, colours, offsets and scales, the glare
// flag (t3 / t0), the caller's sprite byte and the half size $f12.
struct CoronaSpriteCall {
    std::array<float, 3> position{};
    std::uint32_t count{};
    std::array<std::int64_t, 9> textures{};
    std::array<std::array<float, 4>, 9> colours{};
    std::array<float, 9> offsets{};
    std::array<float, 9> scales{};
    bool glare{};
    bool glare_sprite{};
    float half{};
};

// The frame's camera terms beyond the scene's view / projection.
struct CoronaCamera {
    float near_plane{2.0F};   // 00132ef4: the scene camera's near clip
    float far_plane{};        // level_camera_far
    bool fog{};               // [005392d0] & 0x180
    float fog_near{};         // renderer +0x158 (environment +0x60)
    float fog_base{}, fog_scale{};   // renderer +0x180 / +0x190 (level_fog)
};

// What one call did (for the logs).
struct CoronaDrawn {
    std::string skipped;      // empty when the probe ran
    std::int32_t x{}, y{}, box{};
    float fade{1.0F};
    std::uint32_t elements{};  // sprites drawn
    std::int32_t glare_alpha{-1};   // -1: no glare sprite
    std::int32_t visibility{-1};    // 0..255 when read back
};

class CoronaRenderer {
public:
    explicit CoronaRenderer(SceneRenderer& scene);
    // Port-only (checkpoint 132win4): re-creates every device object on the scene's (re-created) device and
    // re-uploads the textures given so far (call after SceneRenderer::reset_device). While the device is lost
    // (device_lost.hpp) the GPU half of draw() is skipped; the CPU half (positions, boxes, fades) still runs.
    void reset_device();
    // The image for a texture key, with its TDX PSM (header +0xa).
    void set_texture(std::int64_t key, const Texture& texture, std::uint16_t psm);
    bool has_texture(std::int64_t key) const { return textures_.count(key) != 0; }
    // Draws the calls in order over the scene's last draw() (its camera, target and depth). With `read_back` the
    // visibility of each probed call is read from the GPU into the result.
    std::vector<CoronaDrawn> draw(const std::vector<CoronaSpriteCall>& calls, const CoronaCamera& camera,
                                  bool read_back);

private:
    template <class T> using Ptr = std::shared_ptr<T>;
    struct Image {
        Ptr<gfx::Texture> view;
        std::uint32_t width{}, height{};
        std::uint16_t psm{};
    };
    SceneRenderer& scene_;
    gfx::CommandList list_;
    Ptr<gfx::Buffer> vertices_;
    std::size_t vertex_capacity_ = 0;
    Ptr<gfx::BlendState> add_;
    Ptr<gfx::RasterState> raster_;
    Ptr<gfx::DepthState> probe_depth_, no_depth_;
    Ptr<gfx::SamplerState> clamp_, wrap_;
    Ptr<gfx::Texture> reduce_, staging_;   // reduce_: a target and its view
    std::uint32_t reduce_n_ = 0;
    std::map<std::int64_t, Image> textures_;
    std::map<std::int64_t, Texture> images_;  // set_texture's images, for reset_device
    void create_pipeline();
    void upload_image(std::int64_t key);
    bool lost() const;  // the scene's device was removed (GPU draws are skipped until reset_device)
    void ensure_targets(std::uint32_t calls);
};

}  // namespace omega
