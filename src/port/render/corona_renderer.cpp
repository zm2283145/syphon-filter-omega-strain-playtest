#include "corona_renderer.hpp"

#include "gfx_device_lost.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

// The shaders (gfx::Program::corona_probe / corona_reduce / corona_element; HLSL in gfx_d3d11_shaders.cpp):
// ps_probe: the probe's frame alpha (0 or 0xff) into the frame-alpha target.
// ps_reduce: the 32 x 32 bilinear samples of the clamped box (params: the first sample and the step, normalised),
// then the four 2 x 2 averages down to 2 x 2 and their average, each truncated to 8 bits as the GS writes them.
// ps_element: MODULATE (Cs = T Cf >> 7, As = vis Af >> 7, Cf / Af in GS units 0..0xff) and ALPHA 0x48's Cs As >> 7,
// added to Cd by the blend state (COLCLAMP: the UNORM target saturates).

struct Vertex {
    float x, y, z, w;  // D3D clip space (w = 1)
    float u, v;
    float r, g, b, a;
    float p0, p1, p2, p3;
};
static_assert(sizeof(Vertex) == 56);

void upload(gfx::Device& device, std::shared_ptr<gfx::Buffer>& buffer, std::size_t& capacity, const void* data,
            std::size_t bytes) {
    if (bytes > capacity) {
        capacity = std::max<std::size_t>(bytes, 64 * sizeof(Vertex));
        buffer.reset();
        buffer = device.create_buffer({capacity, gfx::Usage::dynamic, gfx::bind_vertices}, nullptr, "Create corona vertices");
    }
    device.write(*buffer, data, bytes, "Map corona vertices");
}

// cvt.w.s: truncation toward zero.
std::int32_t cvt(float v) {
    if (!(std::fabs(v) < 2147483520.0F)) throw std::runtime_error("003781b0: cvt.w.s of a value out of range");
    return static_cast<std::int32_t>(v);
}

}  // namespace

CoronaRenderer::CoronaRenderer(SceneRenderer& scene) : scene_(scene) {
    try {
        create_pipeline();
    } catch (const DeviceLost&) {  // port-only (132win4): lost() until the play re-creates the device (reset_device)
    }
}

void CoronaRenderer::reset_device() {
    list_.clear();
    vertices_.reset();
    vertex_capacity_ = 0;
    add_.reset();
    raster_.reset();
    probe_depth_.reset();
    no_depth_.reset();
    clamp_.reset();
    wrap_.reset();
    reduce_.reset();
    staging_.reset();
    reduce_n_ = 0;
    for (auto& [key, image] : textures_) image.view.reset();
    create_pipeline();
    for (const auto& [key, image] : images_) upload_image(key);
}

bool CoronaRenderer::lost() const {
    return !scene_.device() || scene_.device()->removed() || device_lost_pending();
}

void CoronaRenderer::create_pipeline() {
    gfx::Device& device = *scene_.device();
    device.prepare(gfx::Program::corona_probe);
    device.prepare(gfx::Program::corona_reduce);
    device.prepare(gfx::Program::corona_element);
    {
        // ALPHA 0x48 (A Cs, B 0, C As, D Cd): the shader outputs Cs As >> 7; the frame alpha is kept.
        gfx::BlendDesc blending;
        auto& rt = blending.targets[0];
        rt.enable = true;
        rt.src = gfx::BlendFactor::one;
        rt.dst = gfx::BlendFactor::one;
        rt.op = gfx::BlendOp::add;
        rt.src_alpha = gfx::BlendFactor::zero;
        rt.dst_alpha = gfx::BlendFactor::one;
        rt.op_alpha = gfx::BlendOp::add;
        rt.write_mask = gfx::mask_all;
        add_ = device.create_blend(blending, "Create corona blend state");
    }
    raster_ = device.create_raster({false}, "Create corona rasterizer state");
    probe_depth_ = device.create_depth({true, false, gfx::Compare::less_equal}, "Create corona probe depth state");
    no_depth_ = device.create_depth({false, false, gfx::Compare::less_equal}, "Create corona depth state");
    clamp_ = device.create_sampler({gfx::Filter::linear, gfx::Address::clamp, 0.0F}, "Create corona sampler");
    // CLAMP_1 0x4010 (packet 2): WMS / WMT REPEAT.
    wrap_ = device.create_sampler({gfx::Filter::linear, gfx::Address::wrap, 0.0F}, "Create corona element sampler");
}

void CoronaRenderer::set_texture(std::int64_t key, const Texture& texture, std::uint16_t psm) {
    if (texture.width == 0 || texture.height == 0
        || texture.rgba.size() != std::size_t(texture.width) * texture.height * 4)
        throw std::runtime_error("003781b0: invalid element texture image");
    images_[key] = texture;
    Image& entry = textures_[key];
    entry.view.reset();
    entry.width = texture.width;
    entry.height = texture.height;
    entry.psm = psm;
    if (lost()) return;  // port-only (132win4): reset_device uploads it
    try {
        upload_image(key);
    } catch (const DeviceLost&) {
    }
}

void CoronaRenderer::upload_image(std::int64_t key) {
    const Texture& texture = images_.at(key);
    gfx::TextureDesc description;
    description.width = texture.width;
    description.height = texture.height;
    description.format = gfx::Format::rgba8;
    description.usage = gfx::Usage::immutable;
    description.bind = gfx::bind_sampled;
    const gfx::Level level{texture.rgba.data(), texture.width * 4};
    gfx::TextureLabels labels;
    labels.texture = "Create corona texture";
    labels.sampled = "Create corona texture view";
    textures_.at(key).view = scene_.device()->create_texture(description, &level, labels);
}

void CoronaRenderer::ensure_targets(std::uint32_t calls) {
    gfx::Device& device = *scene_.device();
    if (!reduce_ || reduce_n_ < calls) {
        reduce_.reset();
        staging_.reset();
        reduce_n_ = std::max<std::uint32_t>(calls, 32);
        gfx::TextureDesc d;
        d.width = reduce_n_;
        d.height = 1;
        d.format = gfx::Format::r32f;
        d.bind = gfx::bind_target | gfx::bind_sampled;
        gfx::TextureLabels labels;
        labels.texture = "Create corona reduce target";
        labels.target = "Create corona reduce target view";
        labels.sampled = "Create corona reduce view";
        reduce_ = device.create_texture(d, nullptr, labels);
        d.usage = gfx::Usage::staging;
        d.bind = 0;
        gfx::TextureLabels staging_labels;
        staging_labels.texture = "Create corona read-back texture";
        staging_ = device.create_texture(d, nullptr, staging_labels);
    }
}

std::vector<CoronaDrawn> CoronaRenderer::draw(const std::vector<CoronaSpriteCall>& calls, const CoronaCamera& camera,
                                              bool read_back) {
    std::vector<CoronaDrawn> result(calls.size());
    if (calls.empty()) return result;
    const float W = static_cast<float>(scene_.width()), H = static_cast<float>(scene_.height());
    const auto& view = scene_.last_view();
    const auto& projection = scene_.last_projection();
    if (!(projection[5] > 0.0F)) throw std::runtime_error("003781b0: the scene projection has no vertical scale");
    // The virtual GS frame: hh = 224 (the 448-line frame), hw = 240 W / H (320 at 4:3).
    const float hwf = 240.0F * W / H, hhf = 224.0F;
    const std::int32_t hw = static_cast<std::int32_t>(hwf), hh = 224;
    const float t = 1.0F / (0.75F * projection[5]);   // tan of the camera's half angle +100
    const std::int32_t limit_x = static_cast<std::int32_t>(hwf * (4095.0F / (2.0F * hwf)));   // 003751c0
    const std::int32_t limit_y = static_cast<std::int32_t>(hhf * (4095.0F / 448.0F));        // 003751e0
    const auto ndc_x = [&](float g) { return (g - 2048.0F) / hwf; };
    const auto ndc_y = [&](float g) { return -(g - 2048.0F) / hhf; };
    const auto host_u = [&](float g) { return (W * 0.5F + (g - 2048.0F) * W / (2.0F * hwf)) / W; };
    const auto host_v = [&](float g) { return (H * 0.5F + (g - 2048.0F) * H / (2.0F * hhf)) / H; };

    struct Probe {
        std::size_t call;
        std::uint32_t first;   // 6 + 6 probe vertices, then 6 reduce vertices
    };
    struct Segment {
        std::uint32_t first, count;
        const gfx::Texture* view;
    };
    std::vector<Vertex> vertices;
    std::vector<Probe> probes;
    std::vector<Segment> segments;
    std::vector<Vertex> element_vertices;
    const auto quad = [](std::vector<Vertex>& out, float x0, float y0, float x1, float y1, float z, float u0, float v0,
                         float u1, float v1, const std::array<float, 4>& c, const std::array<float, 4>& p) {
        const Vertex tl{x0, y0, z, 1, u0, v0, c[0], c[1], c[2], c[3], p[0], p[1], p[2], p[3]};
        const Vertex tr{x1, y0, z, 1, u1, v0, c[0], c[1], c[2], c[3], p[0], p[1], p[2], p[3]};
        const Vertex bl{x0, y1, z, 1, u0, v1, c[0], c[1], c[2], c[3], p[0], p[1], p[2], p[3]};
        const Vertex br{x1, y1, z, 1, u1, v1, c[0], c[1], c[2], c[3], p[0], p[1], p[2], p[3]};
        for (const Vertex& v : {tl, tr, bl, bl, tr, br}) out.push_back(v);
    };

    for (std::size_t n = 0; n < calls.size(); ++n) {
        const auto& call = calls[n];
        auto& out = result[n];
        if (call.count == 0) {
            out.skipped = "no elements";
            continue;
        }
        if (call.count > 9) throw std::runtime_error("003781b0: more than nine elements");
        // clip = p * view * projection (row vectors).
        float vp[4]{}, clip[4]{};
        const float p[4]{call.position[0], call.position[1], call.position[2], 1.0F};
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) vp[c] += p[k] * view[k * 4 + c];
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) clip[c] += vp[k] * projection[k * 4 + c];
        const float w = clip[3];
        if (!(w >= camera.near_plane) || !(w <= camera.far_plane)) {
            out.skipped = "clipped in depth";
            continue;
        }
        const float nx = clip[0] / w, ny = clip[1] / w;
        if (std::fabs(nx) > 1.5F || std::fabs(ny) > 1.5F) {
            out.skipped = "clipped (1.5 squeeze)";
            continue;
        }
        float fade = 1.0F;
        if (w >= camera.fog_near && camera.fog) {
            float f1 = std::bit_cast<float>(0x3f008081U) * (camera.fog_base + camera.fog_scale * (1.0F / w));
            if (f1 <= 0.0F) {
                out.skipped = "fogged out";
                continue;
            }
            if (!(f1 < 128.0F)) f1 = 128.0F;
            fade = f1 / 128.0F;
        }
        out.fade = fade;
        const float gx = 2048.0F + nx * hwf, gy = 2048.0F - ny * hhf;
        const std::int32_t x = cvt(16.0F * gx) >> 4, y = cvt(16.0F * gy) >> 4;
        const float sp370 = 80.0F / (t * w), sp374 = 56.0F / (t * w);
        std::int32_t box = cvt(0.5F * sp370);
        if (!(box < 0x29)) box = 0x28;
        std::int32_t cx = x, cy = y;
        if (cx < 0x800 - hw + box) cx = 0x800 - hw + box;
        if (hw + 0x800 - box < cx) cx = hw + 0x800 - box;
        if (cy < 0x800 - hh + box) cy = 0x800 - hh + box;
        if (hh + 0x800 - box < cy) cy = hh + 0x800 - box;
        out.x = x;
        out.y = y;
        out.box = box;
        const float depth = clip[2] / w;
        Probe probe{n, static_cast<std::uint32_t>(vertices.size())};
        const std::array<float, 4> zero{0, 0, 0, 0}, one{0, 0, 0, 1};
        quad(vertices, ndc_x(float(cx - box)), ndc_y(float(cy - box)), ndc_x(float(cx + box)), ndc_y(float(cy + box)),
             0.0F, 0, 0, 0, 0, zero, zero);
        quad(vertices, ndc_x(float(x - box)), ndc_y(float(y - box)), ndc_x(float(x + box)), ndc_y(float(y + box)),
             std::clamp(depth, 0.0F, 1.0F), 0, 0, 0, 0, one, zero);
        // The reduce's level-32 sample k at UV (cx - box + 1) + k (2 box - 2) / 32 (GS coordinates).
        const float u0 = float(cx - box + 1), u1 = float(cx + box - 1), v0 = float(cy - box + 1), v1 = float(cy + box - 1);
        const std::array<float, 4> params{host_u(u0), host_v(v0), (host_u(u1) - host_u(u0)) / 32.0F,
                                          (host_v(v1) - host_v(v0)) / 32.0F};
        quad(vertices, -1, 1, 1, -1, 0, 0, 0, 0, 0, zero, params);
        probes.push_back(probe);

        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        const float ax = (2048.0F - fx) / static_cast<float>(hw), ay = (2048.0F - fy) / static_cast<float>(hh);
        const float f21 = call.half * sp370, f20 = call.half * sp374;
        const float slot = static_cast<float>(probes.size() - 1);
        for (std::uint32_t i = 0; i < call.count; ++i) {
            const std::int32_t ex = cvt(fx - ax * call.offsets[i]), ey = cvt(fy - ay * call.offsets[i]);
            const std::int32_t ew = cvt(f21 * call.scales[i]), eh = cvt(f20 * call.scales[i]);
            if (ex < 0x800 - limit_x + ew || limit_x + 0x800 - ew < ex || ey < 0x800 - limit_y + eh
                || limit_y + 0x800 - eh < ey)
                continue;
            const auto& col = call.colours[i];
            const std::int32_t cr = cvt(128.0F * col[0]), cg = cvt(128.0F * col[1]), cb = cvt(128.0F * col[2]);
            const std::int32_t ca = cvt(128.0F * (fade * col[3]));
            if (ca <= 0) continue;
            const auto it = textures_.find(call.textures[i]);
            if (it == textures_.end())
                throw std::runtime_error("003781b0: element texture " + std::to_string(call.textures[i]) + " is not set");
            if (it->second.psm != 0)
                throw std::runtime_error("003781b0: element texture " + std::to_string(call.textures[i]) + " is PSM "
                                         + std::to_string(it->second.psm)
                                         + ", not PSMCT32 (the visibility is written to its alpha, TCC by 003bba10)");
            // RGBAQ bytes (sx: the low byte as the GS takes it).
            const auto byte = [](std::int32_t v) { return static_cast<float>(static_cast<std::uint32_t>(v) & 0xff); };
            const std::array<float, 4> colour{byte(cr), byte(cg), byte(cb), byte(ca)};
            const std::array<float, 4> index{slot, 0, 0, 0};
            const auto first = static_cast<std::uint32_t>(element_vertices.size());
            quad(element_vertices, ndc_x(float(ex - ew)), ndc_y(float(ey - eh)), ndc_x(float(ex + ew)),
                 ndc_y(float(ey + eh)), 0, 0, 0, 1, 1, colour, index);
            ++out.elements;
            if (call.glare && i == 0) {
                const float len = std::sqrt(ax * ax + ay * ay);
                const float f2 = 1.0F - len;
                const std::int32_t glare_alpha = cvt(120.0F * (f2 * (f2 * (f2 * f2))));
                out.glare_alpha = glare_alpha;
                if (call.glare_sprite) {
                    const float cu = float(it->second.width >> 1) / float(it->second.width);
                    const float cv = float(it->second.height >> 1) / float(it->second.height);
                    const std::array<float, 4> glare_colour{byte(cr), byte(cg), byte(cb), byte(glare_alpha)};
                    quad(element_vertices, ndc_x(float(0x800 - hw)), ndc_y(float(0x800 - hh)), ndc_x(float(0x800 + hw)),
                         ndc_y(float(0x800 + hh)), 0, cu, cv, cu, cv, glare_colour, index);
                    ++out.elements;
                }
            }
            const auto count = static_cast<std::uint32_t>(element_vertices.size()) - first;
            if (!segments.empty() && segments.back().view == it->second.view.get()
                && segments.back().first + segments.back().count == first)
                segments.back().count += count;
            else
                segments.push_back({first, count, it->second.view.get()});
        }
    }
    if (probes.empty()) return result;
    // Port-only (132win4): the GPU half is skipped while the device is lost, or dropped when a call loses it.
    if (lost()) return result;
    try {
        ensure_targets(static_cast<std::uint32_t>(probes.size()));
        const auto element_base = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), element_vertices.begin(), element_vertices.end());

        gfx::Device& device = *scene_.device();
        upload(device, vertices_, vertex_capacity_, vertices.data(), vertices.size() * sizeof(Vertex));
        auto& l = list_;
        l.clear();
        l.set_vertex_buffer(vertices_.get(), sizeof(Vertex));
        l.set_raster(raster_.get());
        const gfx::Texture* none[2]{nullptr, nullptr};
        if (!scene_.frame_alpha_target() || !scene_.frame_alpha_view())
            throw std::runtime_error("003781b0: the scene has no frame-alpha target");
        for (std::size_t k = 0; k < probes.size(); ++k) {
            const auto& probe = probes[k];
            // The probe: the frame alpha with the scene's depth (tested, not written).
            l.set_textures(0, 2, none);
            l.set_target(scene_.frame_alpha_target(), scene_.depth_view());
            l.set_viewport({0.0F, 0.0F, W, H, 0.0F, 1.0F});
            l.set_blend(nullptr);
            l.set_depth(probe_depth_.get());
            l.set_program(gfx::Program::corona_probe);
            l.draw(12, probe.first);
            // The reduce into texel k.
            l.set_target(reduce_.get());
            l.set_viewport({static_cast<float>(k), 0.0F, 1.0F, 1.0F, 0.0F, 1.0F});
            l.set_depth(no_depth_.get());
            l.set_program(gfx::Program::corona_reduce);
            l.set_sampler(0, clamp_.get());
            l.set_texture(0, scene_.frame_alpha_view());
            l.draw(6, probe.first + 12);
        }
        l.set_textures(0, 2, none);
        // The element sprites over the scene target, no depth.
        l.set_target(scene_.render_target());
        l.set_viewport({0.0F, 0.0F, W, H, 0.0F, 1.0F});
        l.set_blend(add_.get());
        l.set_depth(no_depth_.get());
        l.set_program(gfx::Program::corona_element);
        l.set_sampler(0, wrap_.get());
        for (const auto& s : segments) {
            const gfx::Texture* views[2]{s.view, reduce_.get()};
            l.set_textures(0, 2, views);
            l.draw(s.count, element_base + s.first);
        }
        l.set_textures(0, 2, none);
        l.set_blend(nullptr);
        if (read_back) l.copy(staging_.get(), reduce_.get());
        device.submit(l);
        if (read_back) {
            device.read(*staging_, 0, [&](const std::uint8_t* data, unsigned) {
                const auto* texels = reinterpret_cast<const float*>(data);
                for (std::size_t k = 0; k < probes.size(); ++k)
                    result[probes[k].call].visibility = static_cast<std::int32_t>(texels[k]);
            }, "Map corona read-back");
        }
    } catch (const DeviceLost&) {
        list_.clear();
        for (auto& r : result) r.visibility = -1;
    }
    return result;
}

}  // namespace omega
