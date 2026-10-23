#include "particle_renderer.hpp"
#include "texture_blocks.hpp"
#include "gfx_device_lost.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

// The shaders (gfx::Program::particle_sprite / particle_clip / particle_haze / particle_copy; HLSL in
// gfx_d3d11_shaders.cpp, with the GS / VU1 notes of each). The sprites arrive in screen space (the GS XYZ2 of the
// VU1 programs); the vertex shader only passes them on. The pixel shader is the GS texture function MODULATE
// (colour = texel * RGBA / 128) and the alpha test of the particle context (ATE = 1, ATST = GREATER, AREF = 0,
// AFAIL = KEEP, set at 0037ee7c): the pixel is dropped unless (At * Af) >> 7 > 0, i.e. At * Af >= 128 with
// At = texel alpha * 255 / 2 (decode_tdx doubles the PS2 alpha) and Af = vertex alpha * 128. Checkpoint 132cp4:
// one heat-haze particle (VU1 0x1cc8) is its two GS sprites over the same corners and depth in one pass
// (ps_haze); 0037bd40's frame to ScreenFxBuf copy is ps_copy.

struct HostVertex {
    float x, y, z;  // D3D normalised device coordinates
    float u, v;
    float r, g, b, a;
};
static_assert(sizeof(HostVertex) == 36);
struct ClipVertex {
    float x, y, z, w;  // D3D clip space
    float u, v;
    float r, g, b, a;
};
static_assert(sizeof(ClipVertex) == 40);
struct HazeVertex {
    float x, y, z;  // D3D normalised device coordinates
    float u, v;     // sprite A: the particle texture
    float s, t;     // sprite B: ScreenFxBuf
    float r, g, b, a;
};
static_assert(sizeof(HazeVertex) == 44);

// Uploads `bytes` into a dynamic vertex buffer, growing it to at least `minimum` bytes. Port-only (checkpoint
// 132win2): with the device removed the upload is skipped (the draws that follow do nothing on a removed device);
// the play re-creates the device at its present and calls reset_device.
void upload(gfx::Device& device, std::shared_ptr<gfx::Buffer>& buffer, std::size_t& capacity, const void* data,
            std::size_t bytes, std::size_t minimum, const char* what) {
    if (bytes > capacity) {
        capacity = std::max(bytes, minimum);
        buffer.reset();
        try {
            buffer = device.create_buffer({capacity, gfx::Usage::dynamic, gfx::bind_vertices}, nullptr, what);
        } catch (const DeviceLost&) {
            capacity = 0;
            return;
        }
    }
    try {
        device.write(*buffer, data, bytes, what);
    } catch (const DeviceLost&) {
    }
}

using particle::Qword;

Qword bits(float a, float b, float c, float d) {
    return {std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b), std::bit_cast<std::uint32_t>(c),
            std::bit_cast<std::uint32_t>(d)};
}

// The host frame's screen space: X / 16 is the pixel column, Y / 16 the row
// (down), Z / 2^24 the D3D depth.
constexpr float kSubpixel = 16.0F, kDepthScale = 16777216.0F;

}  // namespace

ParticleRenderer::ParticleRenderer(SceneRenderer& scene) : scene_(scene) {
    try {
        create_pipeline();
    } catch (const DeviceLost&) {  // port-only (132win3): lost() until the play re-creates the device (reset_device)
    }
}

void ParticleRenderer::reset_device() {
    list_.clear();
    vertices_.reset();
    vertex_capacity_ = 0;
    for (auto& b : blend_) b.reset();
    raster_.reset();
    depth_.reset();
    sampler_.reset();
    textures_.clear();
    haze_vertices_.reset();
    copy_vertices_.reset();
    haze_capacity_ = copy_capacity_ = 0;
    haze_blend_.reset();
    no_depth_.reset();
    frame_copy_.reset();
    screen_fx_.reset();
    clip_vertices_.reset();
    clip_capacity_ = 0;
    reverse_subtract_.reset();
    handle_textures_.clear();
    create_pipeline();
    for (const auto& [def, image] : texture_images_) textures_[def] = make_view(image);
    for (const auto& [slot, image] : handle_images_) handle_textures_[slot] = make_view(image ? *image : white_texel());
}

Texture ParticleRenderer::white_texel() {
    Texture white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    return white;
}

void ParticleRenderer::create_pipeline() {
    gfx::Device& device = *scene_.device();
    device.prepare(gfx::Program::particle_sprite);
    device.prepare(gfx::Program::particle_clip);
    device.prepare(gfx::Program::particle_haze);
    device.prepare(gfx::Program::particle_copy);
    using F = gfx::BlendFactor;
    // The template's GS ALPHA_1 (def+d0, from the 003a3910 table 0053b680 by
    // def+38; Checkpoint 122e): FIX 0x80 and the ABCD byte 0x44 = (Cs - Cd) * As
    // + Cd, 0x68 = Cs * FIX/0x80 + Cd, 0x48 = Cs * As + Cd, 0x46 = (0 - Cd) * As
    // + Cd; the destination alpha is kept. Host choice: D3D11 blend factors.
    const F factors[4][2] = {{F::src_alpha, F::inv_src_alpha}, {F::one, F::one}, {F::src_alpha, F::one},
                             {F::zero, F::inv_src_alpha}};
    for (int k = 0; k < 4; ++k) {
        gfx::BlendDesc blending;
        auto& rt = blending.targets[0];
        rt.enable = true;
        rt.src = factors[k][0];
        rt.dst = factors[k][1];
        rt.op = gfx::BlendOp::add;
        rt.src_alpha = F::zero;
        rt.dst_alpha = F::one;
        rt.op_alpha = gfx::BlendOp::add;
        rt.write_mask = gfx::mask_all;
        blend_[k] = device.create_blend(blending, "Create particle blend state");
    }
    {
        // ALPHA 0x42 (ShatterWindow alphaMode 3, 0x4bc1c8): A = 0, B = Cs, C = As, D = Cd.
        gfx::BlendDesc blending;
        auto& rt = blending.targets[0];
        rt.enable = true;
        rt.src = F::src_alpha;
        rt.dst = F::one;
        rt.op = gfx::BlendOp::rev_subtract;
        rt.src_alpha = F::zero;
        rt.dst_alpha = F::one;
        rt.op_alpha = gfx::BlendOp::add;
        rt.write_mask = gfx::mask_all;
        reverse_subtract_ = device.create_blend(blending, "Create shatter blend state");
    }
    {
        // Heat haze (ps_haze blends ALPHA_1 0x54 itself from frame_cd): blending off, RGB only to the scene target;
        // sprite A's alpha to the frame-alpha target (R8, its red channel).
        gfx::BlendDesc blending;
        blending.independent = true;
        blending.targets[0].enable = false;
        blending.targets[0].write_mask = gfx::mask_rgb;
        blending.targets[1].enable = false;
        blending.targets[1].write_mask = gfx::mask_r;
        haze_blend_ = device.create_blend(blending, "Create heat-haze blend state");
    }
    raster_ = device.create_raster({false}, "Create particle rasterizer state");
    depth_ = device.create_depth({true, false, gfx::Compare::less_equal}, "Create particle depth state");
    no_depth_ = device.create_depth({false, false, gfx::Compare::less_equal}, "Create ScreenFxBuf depth state");
    sampler_ = device.create_sampler({gfx::Filter::linear, gfx::Address::clamp, 0.0F}, "Create particle sampler");
}

void ParticleRenderer::set_texture(const particle::ParticleDefinition* def, const Texture& texture) {
    texture_images_[def] = texture;
    textures_[def] = nullptr;
    if (!lost()) {
        try {
            textures_[def] = make_view(texture);
        } catch (const DeviceLost&) {  // 132win3: reset_device makes it
        }
    }
    // The largest GS texel alpha (decode_tdx doubles the PS2 alpha: 255 = 0x80).
    std::uint32_t top = 0;
    for (std::size_t k = 3; k < texture.rgba.size(); k += 4) top = std::max<std::uint32_t>(top, texture.rgba[k]);
    texture_alpha_[def] = top >= 255 ? 0x80 : top / 2;
}

void ParticleRenderer::set_handle_texture(std::int32_t slot, const Texture* texture) try {
    handle_images_[slot] = texture;
    handle_textures_[slot] = lost() ? nullptr : make_view(texture ? *texture : white_texel());
} catch (const DeviceLost&) {}

bool ParticleRenderer::lost() const {
    return scene_.device() && (scene_.device()->removed() || device_lost_pending());
}

ParticleRenderer::Ptr<gfx::Texture> ParticleRenderer::make_view(const Texture& source) {
    const Texture& texture = upload_image(source);  // port-only (132md1): a mod's replacement image
    if (texture.width == 0 || texture.height == 0
        || (!texture.block_bytes && texture.rgba.size() != std::size_t(texture.width) * texture.height * 4))
        throw std::runtime_error("Invalid particle texture image");
    const auto upload = gfx::upload_levels(scene_.device().get(), texture, false);   // port-only (132md11): BC1 / BC3 blocks
    gfx::TextureLabels labels;
    labels.texture = "Create particle texture";
    labels.sampled = "Create particle texture view";
    return scene_.device()->create_texture(upload.desc, upload.levels.data(), labels);
}

ParticleFrame ParticleRenderer::frame(const std::array<std::array<float, 4>, 4>& world) const {
    // clip = world * view * projection (row vectors), with the D3D depth row
    // turned into the GS / OpenGL convention -w <= z <= w that CLIPW tests:
    // z' = 2 z - w (column 2 = 2 * column 2 - column 3).
    const auto& view = scene_.last_view();
    const auto& projection = scene_.last_projection();
    float wv[4][4]{}, m[4][4]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) wv[r][c] += world[r][k] * view[k * 4 + c];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) m[r][c] += wv[r][k] * projection[k * 4 + c];
    ParticleFrame f;
    for (int r = 0; r < 4; ++r) f.cull[r] = bits(m[r][0], m[r][1], 2 * m[r][2] - m[r][3], m[r][3]);
    f.sprite.clip = f.cull;
    const float w = static_cast<float>(scene_.width()), h = static_cast<float>(scene_.height());
    // vf5 / vf6: z' / w in [-1, 1] -> Z = (z' / w + 1) * 2^23 = D3D depth * 2^24.
    f.sprite.scale = bits(kSubpixel * w / 2, -kSubpixel * h / 2, kDepthScale / 2, 0);
    f.sprite.offset = bits(kSubpixel * w / 2, kSubpixel * h / 2, kDepthScale / 2, 0);
    float p[16];
    std::memcpy(p, projection.data(), sizeof p);
    f.sprite.sprite_scale = particle::sprite_scale_qword(p, particle::kParticleScaleX, particle::kParticleScaleY);
    // Data 0x2c / 0x2d (0037d090). Checkpoint 132cp5 trace: 0037d9a0 stores the plain projection P to
    // stack +f00 (D_005390F0, VU data 1..4) and P' * stack+e80 * top to stack +ec0 (D_005390B0, data 5..8, the
    // clip matrix vf1..vf4), where P' is P with P[0] / a and P[5] / b (0037dc28..3c; a = 2, b = 3 pushed at
    // 003a1b88 / 003a1b90). So the PS2 x / w = ndc_x / a and pair 0x3c1's vf16 * (0.5 a, -0.5 b) gives
    // ST = 0.5 + 0.5 ndc_x, 0.5 - 0.5 ndc_y: the whole displayed frame across ScreenFxBuf's 0..1, whatever its
    // pixel size. The host clip matrix is the unscaled P, so a = b = 1 gives the same ST at any PC resolution;
    // data 0x2d's 1 / (W << 4), 1 / (H << 4) take the host target's W / H, as 00377c70 takes the renderer's.
    f.sprite.haze_clip = particle::heat_haze_clip_qword(1.0F, 1.0F);
    f.sprite.haze_uv = particle::heat_haze_uv_qword(static_cast<std::int32_t>(scene_.width()),
                                                    static_cast<std::int32_t>(scene_.height()));
    return f;
}

void ParticleRenderer::copy_screen_fx() try {
    if (lost()) return;  // port-only (132win2): a removed device's frame is lost (reset_device)
    // 0037bd40 at 0013e924 (after the opaque lists): the frame to the 256 x 256 ScreenFxBuf (renderer +10cc)
    // with one bilinear sprite. Host: the target copied, then drawn down with D3D11 linear sampling.
    gfx::Device& device = *scene_.device();
    const gfx::Texture* frame = scene_.render_target();
    const gfx::TextureDesc& fd = frame->desc();
    if (fd.samples != 1) throw std::runtime_error("The ScreenFxBuf copy (0037bd40) of a multisampled target");
    if (!frame_copy_ || frame_copy_->desc().width != fd.width || frame_copy_->desc().height != fd.height
        || frame_copy_->desc().format != fd.format) {
        frame_copy_.reset();
        screen_fx_.reset();
        gfx::TextureDesc cd;
        cd.width = fd.width;
        cd.height = fd.height;
        cd.format = fd.format;
        cd.bind = gfx::bind_sampled;
        gfx::TextureLabels copy_labels;
        copy_labels.texture = "Create ScreenFxBuf frame copy";
        copy_labels.sampled = "Create ScreenFxBuf frame copy view";
        frame_copy_ = device.create_texture(cd, nullptr, copy_labels);
        gfx::TextureDesc fx;
        fx.width = fx.height = 256;
        fx.format = fd.format;
        fx.bind = gfx::bind_sampled | gfx::bind_target;
        gfx::TextureLabels fx_labels;
        fx_labels.texture = "Create ScreenFxBuf";
        fx_labels.sampled = "Create ScreenFxBuf view";
        fx_labels.target = "Create ScreenFxBuf target";
        screen_fx_ = device.create_texture(fx, nullptr, fx_labels);
    }
    auto& l = list_;
    l.clear();
    const gfx::TargetView none_target{};
    l.set_targets(1, &none_target, nullptr);
    l.copy(frame_copy_.get(), frame);
    // The copy sprite: UV (0, 0)..(W, H) over XYZ (0, 0)..(256, 256) (no half-pixel offsets), so the GS
    // samples ScreenFxBuf pixel i at U = i * W / 256. D3D samples at pixel centres (i + 0.5), so the quad's UV
    // runs from -0.5 / 256 to 255.5 / 256 to sample the same frame positions (i / 256 of the width).
    constexpr float lo = -0.5F / 256.0F, hi = 255.5F / 256.0F;
    const HostVertex tl{-1, 1, 0, lo, lo, 1, 1, 1, 1}, tr{1, 1, 0, hi, lo, 1, 1, 1, 1};
    const HostVertex bl{-1, -1, 0, lo, hi, 1, 1, 1, 1}, br{1, -1, 0, hi, hi, 1, 1, 1, 1};
    const HostVertex quad[6]{tl, tr, bl, bl, tr, br};
    device.submit(l);  // the copy before the upload below (immediate)
    upload(device, copy_vertices_, copy_capacity_, quad, sizeof quad, sizeof quad, "ScreenFxBuf vertices");
    l.set_target(screen_fx_.get());
    l.set_viewport({0.0F, 0.0F, 256.0F, 256.0F, 0.0F, 1.0F});
    l.set_raster(raster_.get());
    l.set_depth(no_depth_.get());
    l.set_blend(nullptr);
    l.set_program(gfx::Program::particle_copy);
    l.set_vertex_buffer(copy_vertices_.get(), sizeof(HostVertex));
    l.set_sampler(0, sampler_.get());
    l.set_texture(0, frame_copy_.get());
    l.draw(6, 0);
    l.set_texture(0, nullptr);
    l.set_targets(1, &none_target, nullptr);
    device.submit(l);
} catch (const DeviceLost&) {
    list_.clear();
}

std::size_t ParticleRenderer::draw(const std::vector<particle::ParticleSpriteBatch>& batches) try {
    if (lost()) return 0;
    struct Segment {
        std::uint32_t first = 0, count = 0;
        const gfx::Texture* view = nullptr;
        int blend = 0;   // -1: heat haze (haze_vertices)
    };
    std::vector<HostVertex> vertices;
    std::vector<HazeVertex> haze_vertices;
    std::vector<gfx::Box> haze_boxes;  // each haze sprite's target pixels (frame_cd)
    std::vector<Segment> segments;
    const float w = static_cast<float>(scene_.width()), h = static_cast<float>(scene_.height());
    const auto ndc_x = [&](std::int32_t x) { return x / kSubpixel / w * 2 - 1; };
    const auto ndc_y = [&](std::int32_t y) { return 1 - y / kSubpixel / h * 2; };
    for (const auto& b : batches) {
        if (b.stop == 0x1cc8) {
            const auto it = textures_.find(b.def);
            if (it == textures_.end()) throw std::runtime_error("heat-haze draw: no texture for the batch's definition");
            if (!screen_fx_) throw std::runtime_error("heat-haze draw: ScreenFxBuf (0037bd40) was not copied");
            if (!frame_copy_) throw std::runtime_error("heat-haze draw: no frame copy for Cd (0037bd40)");
            const auto first = static_cast<std::uint32_t>(haze_vertices.size());
            for (const auto& s : b.sprites) {
                const auto px = [&](std::int32_t v, float size) {
                    return static_cast<unsigned>(std::clamp(v / kSubpixel, 0.0F, size));
                };
                const unsigned left = px(std::min(s.xy_first[0], s.xy_second[0]), w),
                               right = static_cast<unsigned>(std::ceil(std::clamp(
                                   std::max(s.xy_first[0], s.xy_second[0]) / kSubpixel, 0.0F, w))),
                               top = px(std::min(s.xy_first[1], s.xy_second[1]), h),
                               bottom = static_cast<unsigned>(std::ceil(std::clamp(
                                   std::max(s.xy_first[1], s.xy_second[1]) / kSubpixel, 0.0F, h)));
                haze_boxes.push_back({left, top, 0, std::max(left, right), std::max(top, bottom), 1});
                const float r = s.rgba[0] / 128.0F, g = s.rgba[1] / 128.0F, bl = s.rgba[2] / 128.0F,
                            a = s.rgba[3] / 128.0F;
                const float x0 = ndc_x(s.xy_first[0]), y0 = ndc_y(s.xy_first[1]);
                const float x1 = ndc_x(s.xy_second[0]), y1 = ndc_y(s.xy_second[1]);
                const float z = static_cast<float>(s.z) / kDepthScale;
                const float u0 = s.stq_first[0] / s.stq_first[2], v0 = s.stq_first[1] / s.stq_first[2];
                const float u1 = s.stq_second[0] / s.stq_second[2], v1 = s.stq_second[1] / s.stq_second[2];
                const float s0 = s.uv_first[0], t0 = s.uv_first[1], s1 = s.uv_second[0], t1 = s.uv_second[1];
                const HazeVertex tl{x0, y0, z, u0, v0, s0, t0, r, g, bl, a};
                const HazeVertex tr{x1, y0, z, u1, v0, s1, t0, r, g, bl, a};
                const HazeVertex bl_{x0, y1, z, u0, v1, s0, t1, r, g, bl, a};
                const HazeVertex br{x1, y1, z, u1, v1, s1, t1, r, g, bl, a};
                for (const HazeVertex& v : {tl, tr, bl_, bl_, tr, br}) haze_vertices.push_back(v);
            }
            const auto count = static_cast<std::uint32_t>(haze_vertices.size()) - first;
            if (count) segments.push_back({first, count, it->second.get(), -1});
            continue;
        }
        int blend = -1;
        for (int k = 0; k < 4; ++k)
            if (b.alpha == particle::kParticleAlphaTable[k]) blend = k;
        // Checkpoint 132ms5b: a weather sprite flush may carry 0x4bc1c8 (alphaMode 3, the reverse subtract).
        if (b.alpha == 0x0000008000000042ull) blend = 4;
        if (blend < 0) {
            char text[96];
            std::snprintf(text, sizeof text, "particle draw: GS ALPHA_1 %016llx (def+d0) is not hosted",
                          static_cast<unsigned long long>(b.alpha));
            throw std::runtime_error(text);
        }
        const auto it = textures_.find(b.def);
        if (it == textures_.end()) throw std::runtime_error("particle draw: no texture for the batch's definition");
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (const auto& s : b.sprites) {
            const float r = s.rgba[0] / 128.0F, g = s.rgba[1] / 128.0F, bl = s.rgba[2] / 128.0F,
                        a = s.rgba[3] / 128.0F;
            const float x0 = ndc_x(s.xy_first[0]), y0 = ndc_y(s.xy_first[1]);
            const float x1 = ndc_x(s.xy_second[0]), y1 = ndc_y(s.xy_second[1]);
            const float z = static_cast<float>(s.z) / kDepthScale;
            // STQ: the GS texture coordinate is (S / Q, T / Q).
            const float u0 = s.stq_first[0] / s.stq_first[2], v0 = s.stq_first[1] / s.stq_first[2];
            const float u1 = s.stq_second[0] / s.stq_second[2], v1 = s.stq_second[1] / s.stq_second[2];
            const HostVertex tl{x0, y0, z, u0, v0, r, g, bl, a};
            const HostVertex tr{x1, y0, z, u1, v0, r, g, bl, a};
            const HostVertex bl_{x0, y1, z, u0, v1, r, g, bl, a};
            const HostVertex br{x1, y1, z, u1, v1, r, g, bl, a};
            for (const HostVertex& v : {tl, tr, bl_, bl_, tr, br}) vertices.push_back(v);
        }
        const auto count = static_cast<std::uint32_t>(vertices.size()) - first;
        if (count) segments.push_back({first, count, it->second.get(), blend});
    }
    if (vertices.empty() && haze_vertices.empty()) return 0;
    gfx::Device& device = *scene_.device();
    if (!vertices.empty())
        upload(device, vertices_, vertex_capacity_, vertices.data(), vertices.size() * sizeof(HostVertex),
               6 * 512 * sizeof(HostVertex), "Particle vertex buffer");
    if (!haze_vertices.empty())
        upload(device, haze_vertices_, haze_capacity_, haze_vertices.data(), haze_vertices.size() * sizeof(HazeVertex),
               6 * 64 * sizeof(HazeVertex), "Heat-haze vertex buffer");
    const gfx::Texture* target = scene_.render_target();
    auto& l = list_;
    l.clear();
    l.set_target(target, scene_.depth_view());
    l.set_viewport({0.0F, 0.0F, static_cast<float>(scene_.width()), static_cast<float>(scene_.height()), 0.0F, 1.0F});
    l.set_raster(raster_.get());
    l.set_depth(depth_.get());
    l.set_sampler(0, sampler_.get());
    int bound = 0;   // 1: the sprite pipeline, -1: the heat-haze pipeline
    for (const Segment& s : segments) {
        const int kind = s.blend < 0 ? -1 : 1;
        if (kind != bound) {
            bound = kind;
            if (kind > 0) l.set_vertex_buffer(vertices_.get(), sizeof(HostVertex));
            else l.set_vertex_buffer(haze_vertices_.get(), sizeof(HazeVertex));
            l.set_program(kind > 0 ? gfx::Program::particle_sprite : gfx::Program::particle_haze);
            l.set_texture(1, kind > 0 ? nullptr : screen_fx_.get());
            l.set_blend(kind > 0 ? nullptr : haze_blend_.get());
            if (kind < 0 && !scene_.frame_alpha_target())
                throw std::runtime_error("0037d3d8: the scene has no frame-alpha target for sprite A");
            const gfx::TargetView targets[2]{{target, 0}, {kind > 0 ? nullptr : scene_.frame_alpha_target(), 0}};
            l.set_targets(kind > 0 ? 1 : 2, targets, scene_.depth_view());
        }
        if (kind > 0) l.set_blend(s.blend == 4 ? reverse_subtract_.get() : blend_[s.blend].get());
        l.set_texture(0, s.view);
        if (kind > 0) {
            l.draw(s.count, s.first);
            continue;
        }
        // One haze sprite at a time: its target pixels (with every earlier sprite drawn) to frame_cd first.
        for (std::uint32_t v = s.first; v < s.first + s.count; v += 6) {
            const gfx::Box& box = haze_boxes[v / 6];
            if (box.right <= box.left || box.bottom <= box.top) continue;
            l.set_texture(2, nullptr);
            l.copy_region(frame_copy_.get(), 0, box.left, box.top, target, 0, &box);
            l.set_texture(2, frame_copy_.get());
            l.draw(6, v);
        }
    }
    const gfx::Texture* none[3]{};
    l.set_textures(0, 3, none);
    if (bound < 0) l.set_target(target, scene_.depth_view());
    device.submit(l);
    return (vertices.size() + haze_vertices.size()) / 6;
} catch (const DeviceLost&) {
    list_.clear();
    return 0;
} catch (...) {
    list_.clear();
    throw;
}

std::size_t ParticleRenderer::draw_shatter(const std::vector<particle::ShatterFlush>& flushes,
                                           const std::array<std::array<float, 4>, 4>& node) {
    std::vector<TriangleFlush> list;
    for (const auto& f : flushes) {
        const auto it = handle_textures_.find(f.texture);
        if (it == handle_textures_.end()) throw std::runtime_error("shatter draw: no texture for the em+170 slot");
        list.push_back({it->second.get(), f.alpha, &f.vertices});
    }
    return draw_triangles(list, node, "shatter draw");
}

std::size_t ParticleRenderer::draw_weather(const std::vector<particle::WeatherFlush>& flushes,
                                           const std::array<std::array<float, 4>, 4>& node, std::size_t* sprites) {
    // In flush order: runs of sprite flushes (00371170, weather_sprite_batch under the node's frame block) and
    // runs of triangle flushes (00371200).
    std::size_t triangles = 0;
    std::vector<TriangleFlush> list;
    std::vector<particle::ParticleSpriteBatch> batches;
    const auto flush_triangles = [&] {
        if (!list.empty()) triangles += draw_triangles(list, node, "weather draw");
        list.clear();
    };
    const auto flush_sprites = [&] {
        if (batches.empty()) return;
        const std::size_t n = draw(batches);
        if (sprites) *sprites += n;
        batches.clear();
    };
    std::optional<particle::SpriteFrame> sprite_frame;
    for (const auto& f : flushes) {
        if (!textures_.count(f.def)) throw std::runtime_error("weather draw: no texture for the def+74 bind (003820e0)");
        if (f.sprites) {
            flush_triangles();
            if (!sprite_frame) sprite_frame = frame(node).sprite;
            batches.push_back(particle::weather_sprite_batch(f, *sprite_frame));
            continue;
        }
        flush_sprites();
        list.push_back({textures_.find(f.def)->second.get(), f.alpha, &f.vertices});
    }
    flush_sprites();
    flush_triangles();
    return triangles;
}

std::size_t ParticleRenderer::draw_triangles(const std::vector<TriangleFlush>& flushes,
                                             const std::array<std::array<float, 4>, 4>& node, const char* what) try {
    if (lost()) return 0;
    struct Segment {
        std::uint32_t first = 0, count = 0;
        const gfx::Texture* view = nullptr;
        const gfx::BlendState* blend = nullptr;
    };
    // clip = p * node * view * projection (row vectors, the D3D depth row as is).
    const auto& view = scene_.last_view();
    const auto& projection = scene_.last_projection();
    float wv[4][4]{}, m[4][4]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) wv[r][c] += node[r][k] * view[k * 4 + c];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) m[r][c] += wv[r][k] * projection[k * 4 + c];
    std::vector<ClipVertex> vertices;
    std::vector<Segment> segments;
    for (const auto& f : flushes) {
        const gfx::BlendState* blend = nullptr;
        for (int k = 0; k < 3; ++k)
            if (f.alpha == particle::kParticleAlphaTable[k]) blend = blend_[k].get();
        if (f.alpha == 0x0000008000000042ull) blend = reverse_subtract_.get();
        if (!blend) {
            char text[128];
            std::snprintf(text, sizeof text, "%s: GS ALPHA_1 %016llx (0x4bc1b0) is not hosted", what,
                          static_cast<unsigned long long>(f.alpha));
            throw std::runtime_error(text);
        }
        if (f.vertices->size() % 3 != 0)
            throw std::runtime_error(std::string(what) + ": a flush that is not whole triangles");
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (const auto& v : *f.vertices) {
            const auto& p = v.position;
            float clip[4]{};
            for (int c = 0; c < 4; ++c) clip[c] = p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + p[3] * m[3][c];
            vertices.push_back({clip[0], clip[1], clip[2], clip[3], v.st[0] - 2048.0F, v.st[1] - 2048.0F, v.colour[0],
                                v.colour[1], v.colour[2], v.colour[3]});
        }
        const auto count = static_cast<std::uint32_t>(vertices.size()) - first;
        if (count) segments.push_back({first, count, f.view, blend});
    }
    if (vertices.empty()) return 0;
    gfx::Device& device = *scene_.device();
    if (vertices.size() > clip_capacity_) {
        clip_capacity_ = std::max<std::size_t>(vertices.size(), 3 * 512);
        clip_vertices_.reset();
        clip_vertices_ = device.create_buffer({clip_capacity_ * sizeof(ClipVertex), gfx::Usage::dynamic, gfx::bind_vertices},
                                              nullptr, "Create shatter vertex buffer");
    }
    try {
        device.write(*clip_vertices_, vertices.data(), vertices.size() * sizeof(ClipVertex), "Map shatter vertices");
    } catch (const DeviceLost&) {
        return 0;
    }
    const gfx::Texture* target = scene_.render_target();
    auto& l = list_;
    l.clear();
    l.set_target(target, scene_.depth_view());
    l.set_viewport({0.0F, 0.0F, static_cast<float>(scene_.width()), static_cast<float>(scene_.height()), 0.0F, 1.0F});
    l.set_raster(raster_.get());
    l.set_depth(depth_.get());
    l.set_program(gfx::Program::particle_clip);
    l.set_vertex_buffer(clip_vertices_.get(), sizeof(ClipVertex));
    l.set_sampler(0, sampler_.get());
    for (const Segment& s : segments) {
        l.set_blend(s.blend);
        l.set_texture(0, s.view);
        l.draw(s.count, s.first);
    }
    l.set_texture(0, nullptr);
    device.submit(l);
    return vertices.size() / 3;
} catch (const DeviceLost&) {
    list_.clear();
    return 0;
} catch (...) {
    list_.clear();
    throw;
}

}  // namespace omega
