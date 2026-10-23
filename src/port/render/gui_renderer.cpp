#include "gui_renderer.hpp"
#include "texture_blocks.hpp"
#include "frame_fit.hpp"
#include "gfx_device_lost.hpp"
#include "zone_profile.hpp"

#include "disc.hpp"
#include "frontend_formats.hpp"
#include "texture_replacement.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

using HostVertex = GuiHostVertex;
static_assert(sizeof(HostVertex) == 40);

std::array<float, 4> host_colour(const std::array<std::int32_t, 4>& c) {
    std::array<float, 4> out{};
    for (int k = 0; k < 4; ++k) out[k] = static_cast<float>(std::clamp(c[k], 0, 255)) / 128.0F;
    return out;
}

// Port-only (checkpoint 132md7): the host depth of a GUI-space z in a 003836b0 run drawn with the depth buffer (a run
// with a mod's shared batch): smaller z is nearer (132f6), clip z 0.5 + z / 65536 with w 1 (|z| < 32768 drawn).
float preview_depth(float z) { return 0.5F + z / 65536.0F; }

// Checkpoint 132ws: a 4:3 backdrop picture under the wide setting is textured batches reaching both 4:3 sides (to
// within 8 GUI units: the loadout screen's halves start at -318.5) that cover 95 % of the 448 rows between them.
constexpr float wide_backdrop_reach = 312.0F;
constexpr float wide_backdrop_rows = 0.95F * 448.0F;

}  // namespace

std::uint32_t GuiDiscFonts::handle(const std::string& name) const {
    std::string n = name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "callout") return 13;
    if (n == "largefnt") return 14;
    return default_handle;  // "default", NULL / "" and unregistered names
}

const GuiFont* GuiDiscFonts::font(std::uint32_t h) const {
    const auto it = fonts.find(h);
    return it == fonts.end() ? nullptr : &it->second;
}

GuiDiscFonts load_gui_fonts(DiscImage& disc) {
    GuiDiscFonts out;
    const std::vector<std::string> hog{"GAMEDATA", "COMMON", "FONTS.HOG"};
    const std::pair<const char*, std::uint32_t> files[] = {{"DEFAULT.FNT", 12}, {"CALLOUT.FNT", 13}, {"LARGEFNT.FNT", 14}};
    for (const auto& [file, slot] : files) {
        const FontFile fnt = decode_fnt(disc.archive_member(hog, file));
        Texture image = decode_tdx(disc.archive_member(hog, fnt.texture));
        if (image.width > 0xffff) throw std::runtime_error("font texture too wide: " + fnt.texture);
        out.fonts.emplace(slot, GuiFont::from_fnt(fnt, slot, static_cast<std::uint16_t>(image.width)));
        out.images.emplace(slot, std::move(image));
    }
    return out;
}

namespace {
bool same_name(const std::string& a, const std::string& b) {
    // 00128e80 (strcasecmp): ASCII letters compare without case.
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        const auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c; };
        if (up(a[k]) != up(b[k])) return false;
    }
    return true;
}
// The HOG lookup 0036d2a0 takes the first member whose name matches without case.
std::optional<std::string> member_named(const std::vector<std::string>& names, const std::string& name) {
    for (const auto& n : names)
        if (same_name(n, name)) return n;
    return std::nullopt;
}
std::optional<std::vector<std::uint8_t>> hog_sub(const std::vector<std::uint8_t>& hog, const std::string& name,
                                                 const std::string& what) {
    if (hog.size() < 20) throw std::runtime_error("truncated " + what);
    const std::uint32_t index_size = static_cast<std::uint32_t>(hog[16]) | static_cast<std::uint32_t>(hog[17]) << 8 |
                                     static_cast<std::uint32_t>(hog[18]) << 16 |
                                     static_cast<std::uint32_t>(hog[19]) << 24;
    if (index_size < 20 || index_size > hog.size()) throw std::runtime_error("invalid " + what + " header");
    const auto index = std::span(hog).first(index_size);
    const auto names = hog_member_names(index, hog.size());
    const auto stored = member_named(names, name);
    if (!stored) return std::nullopt;
    const Extent e = hog_member(index, hog.size(), *stored);
    return std::vector<std::uint8_t>(hog.begin() + static_cast<std::ptrdiff_t>(e.offset),
                                     hog.begin() + static_cast<std::ptrdiff_t>(e.offset + e.size));
}
}  // namespace

std::optional<GuiDecoIe> GuiDecorAssets::resolve(const std::string& name) {
    auto f = files_.find(name);
    if (f == files_.end()) {
        const std::vector<std::string> ntsc{"GAMEDATA", "FRONTEND", "NTSC.HOG"};
        const auto front = disc_.archive_member_names(ntsc);
        std::vector<std::uint8_t> hog;
        if (const auto stored = member_named(front, name + ".HOG"))
            hog = disc_.archive_member(ntsc, *stored);
        else {
            try {
                hog = disc_.read(disc_.file({"GAMEDATA", "COMMON", name + ".HOG"}));
            } catch (const std::exception& e) {
                throw std::runtime_error("unhosted(0x413124): IE file " + name + ".IE (" + e.what() + ")");
            }
        }
        const auto ie = hog_sub(hog, name + ".IE", name + ".HOG");
        if (!ie) throw std::runtime_error("unhosted(0x413124): " + name + ".HOG has no " + name + ".IE");
        hogs_[name] = std::move(hog);
        f = files_.emplace(name, std::make_unique<IeFile>(decode_ie(*ie))).first;
    }
    current_ = &hogs_.at(name);
    current_name_ = name;
    return GuiDecoIe{f->second.get(), this};
}

std::int32_t GuiDecorAssets::load(const std::string& name) {
    // 00380a40: a name not ending in "tdx" (strcasecmp) answers -1; otherwise the stem (after the
    // last '\\', up to the first '.') is compared without case against the loaded textures, and a
    // match is shared (its +64 count is not modelled; nothing here unloads).
    if (name.size() < 3 || !same_name(name.substr(name.size() - 3), "tdx"))
        throw std::runtime_error("unhosted(0x380c68): texture " + name + " is not a TDX (answers -1)");
    std::string stem = name.substr(name.rfind('\\') == std::string::npos ? 0 : name.rfind('\\') + 1);
    stem = stem.substr(0, stem.find('.'));
    for (char& c : stem)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 0x20);
    const auto it = slots_.find(stem);
    if (it != slots_.end()) return it->second;
    if (!current_) throw std::runtime_error("unhosted(0x380a40): texture " + name + " outside an IE load");
    const auto bytes = hog_sub(*current_, name, current_name_ + ".HOG");
    if (!bytes) throw std::runtime_error("unhosted(0x380a40): texture " + name + " is not in " + current_name_ + ".HOG");
    const std::int32_t slot = first_slot + static_cast<std::int32_t>(slots_.size());
    images_.emplace(static_cast<std::uint32_t>(slot), decode_tdx(*bytes));
    slots_.emplace(stem, slot);
    return slot;
}

std::uint32_t GuiDecorAssets::record_flags(std::int32_t index) {
    throw std::runtime_error("unhosted(0x3f03dc): texture record +48 of slot " + std::to_string(index));
}

GuiHogTextures::GuiHogTextures(std::vector<std::uint8_t> hog, std::string label, std::int32_t first_slot)
    : hog_(std::move(hog)), label_(std::move(label)), first_slot_(first_slot) {}

std::optional<std::vector<std::uint8_t>> GuiHogTextures::member(const std::string& name) const {
    return hog_sub(hog_, name, label_);
}

std::int32_t GuiHogTextures::load(const std::string& name) {
    // 00380a40, as GuiDecorAssets::load.
    if (name.size() < 3 || !same_name(name.substr(name.size() - 3), "tdx"))
        throw std::runtime_error("unhosted(0x380c68): texture " + name + " is not a TDX (answers -1)");
    std::string stem = name.substr(name.rfind('\\') == std::string::npos ? 0 : name.rfind('\\') + 1);
    stem = stem.substr(0, stem.find('.'));
    for (char& c : stem)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 0x20);
    const auto it = slots_.find(stem);
    if (it != slots_.end()) return it->second;
    const auto bytes = hog_sub(hog_, name, label_);
    if (!bytes) throw std::runtime_error("unhosted(0x380a40): texture " + name + " is not in " + label_);
    const std::int32_t slot = first_slot_ + static_cast<std::int32_t>(slots_.size());
    images_.emplace(static_cast<std::uint32_t>(slot), decode_tdx(*bytes));
    slots_.emplace(stem, slot);
    return slot;
}

std::uint32_t GuiHogTextures::record_flags(std::int32_t index) {
    throw std::runtime_error("unhosted(0x3f03dc): texture record +48 of slot " + std::to_string(index));
}

std::optional<std::string> GuiStaticHost::get_string(const std::string& key) {
    if (!strings_) throw std::runtime_error("unhosted(0x3fdb50): GetString " + key);
    return strings_(key);
}
std::uint8_t GuiStaticHost::font_height(std::uint32_t f) {
    const GuiFont* font = fonts_.font(f);
    if (!font) throw std::runtime_error("unhosted(0x36a2f0): font handle " + std::to_string(f));
    return font->height();
}
std::int32_t GuiStaticHost::font_width(std::uint32_t f, const std::string& s, std::int32_t n) {
    const GuiFont* font = fonts_.font(f);
    if (!font) throw std::runtime_error("unhosted(0x36aa50): font handle " + std::to_string(f));
    std::vector<std::uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    return gui_font_width(*font, b, n);
}
void GuiStaticHost::profile_select(std::uint32_t p) { recorded.push_back("3361b0 " + std::to_string(p)); }
void GuiStaticHost::app_2c9af0(std::int32_t v) { recorded.push_back("2c9af0 " + std::to_string(v)); }
void GuiStaticHost::app_131470(std::int32_t) { throw std::runtime_error("unhosted(0x131470)"); }
void GuiStaticHost::app_2c82f0() { throw std::runtime_error("unhosted(0x2c82f0)"); }
void GuiStaticHost::app_2c81d0() { throw std::runtime_error("unhosted(0x2c81d0)"); }
std::int32_t GuiStaticHost::popup_open(std::int32_t, const std::optional<std::string>&,
                                       const std::optional<std::string>&, const gui::Widget*, std::int32_t) {
    throw std::runtime_error("unhosted(0x2cae30)");
}
void GuiStaticHost::popup_close(std::int32_t) { throw std::runtime_error("unhosted(0x2cac80)"); }
std::int32_t GuiStaticHost::popup_query(std::int32_t) { throw std::runtime_error("unhosted(0x2caba0)"); }
gui::Widget* GuiStaticHost::factory(const std::string& cls, const std::string&) {
    throw std::runtime_error("unhosted(0x418a80): screen " + cls);
}

std::vector<std::uint8_t> load_frontend_gui(DiscImage& disc, const std::string& screen) {
    const auto hog = disc.archive_member({"GAMEDATA", "FRONTEND", "NTSC.HOG"}, screen + ".HOG");
    if (hog.size() < 20) throw std::runtime_error("truncated " + screen + ".HOG");
    const std::uint32_t index_size = static_cast<std::uint32_t>(hog[16]) | static_cast<std::uint32_t>(hog[17]) << 8 |
                                     static_cast<std::uint32_t>(hog[18]) << 16 |
                                     static_cast<std::uint32_t>(hog[19]) << 24;
    if (index_size < 20 || index_size > hog.size()) throw std::runtime_error("invalid " + screen + ".HOG header");
    const Extent e = hog_member(std::span(hog).first(index_size), hog.size(), screen + ".GUI");
    return {hog.begin() + static_cast<std::ptrdiff_t>(e.offset),
            hog.begin() + static_cast<std::ptrdiff_t>(e.offset + e.size)};
}

namespace {
// The 3D figures: GuiEquipmentModify's 002bfa90, GuiScoresEquip's 0035b390, and the agent screens'
// models (GuiAgentCreate 0028d870, GuiAgentModify 00341f00; Checkpoint 132cc6), the HUD map 0026ca70 (132hud),
// GuiAgentDisplay's 00361e70 (the personnel agent-info figure, Checkpoint 132f14).
bool is_figure_batch(const GuiDrawBatch& b) {
    return b.flush == 0x2bfa90 || b.flush == 0x35b390 || b.flush == 0x28d870 || b.flush == 0x341f00 ||
           b.flush == 0x26ca70 ||  // the HUD map (GuiMapScreen, checkpoint 132hud)
           b.flush == 0x361e70;
}
bool is_line_batch(const GuiDrawBatch& b) { return b.flush == 0x3701b0; }
bool is_ie_batch(const GuiDrawBatch& b) {
    if (b.flush == 0x371200 || b.flush == 0x3836b0 || is_figure_batch(b)) return true;
    if (b.flush == 0x26cdb4) return true;  // the IAMAP screen over the HUD map (checkpoint 132hud2)
    if (b.flush == 0x4118e0) return false;  // the movie subtitle sprites (checkpoint 132cp11)
    if (b.flush != 0x370550) throw std::runtime_error("GUI batch for an unknown flush " + std::to_string(b.flush));
    return false;
}
void append_quads(const GuiDrawBatch& b, std::vector<GuiQuad>& quads) {
    {
        std::size_t total = 0;
        for (const std::int32_t n : b.strips) {
            if (n < 0) throw std::runtime_error("GUI batch with a negative strip count");
            total += static_cast<std::size_t>(n);
        }
        if (total != b.vertices.size())
            throw std::runtime_error("GUI batch strip counts disagree with its vertices");
        std::size_t at = 0;
        for (std::size_t s = 0; s < b.strips.size(); ++s) {
            const std::size_t n = static_cast<std::size_t>(b.strips[s]);
            // A continuation strip's first two vertices are the carried copy
            // of the previous strip's last pair (0025ca40).
            const std::size_t skip = s == 0 ? 0 : std::min<std::size_t>(2, n);
            if ((n - skip) % 2 != 0) throw std::runtime_error("GUI strip with an odd vertex count");
            for (std::size_t k = at + skip; k < at + n; k += 2) {
                const GuiVertex& a = b.vertices[k];
                const GuiVertex& c = b.vertices[k + 1];
                GuiQuad q;
                q.slot = b.slot;
                q.p0 = {a.pos[0], a.pos[1]};
                q.p1 = {c.pos[0], c.pos[1]};
                q.uv0 = {a.uv[0] - 2048.0F, a.uv[1] - 2048.0F};
                q.uv1 = {c.uv[0] - 2048.0F, c.uv[1] - 2048.0F};
                q.c0 = host_colour(a.colour);
                q.c1 = host_colour(c.colour);
                quads.push_back(q);
            }
            at += n;
        }
    }
}
}  // namespace

std::vector<GuiQuad> gui_quads(const GuiDrawList& list) {
    std::vector<GuiQuad> quads;
    for (const GuiDrawBatch& b : list.batches)
        if (!is_line_batch(b) && !is_ie_batch(b)) append_quads(b, quads);
    return quads;
}

std::vector<GuiTriangleVertex> gui_triangles(const GuiDrawBatch& b, const GuiViewProjection* view) {
    if (!is_ie_batch(b)) throw std::runtime_error("gui_triangles: not a 00371200 batch");
    if (b.perspective && !view)
        throw std::runtime_error("00371200 batch under the perspective camera record (+61, 003ed240) without its projection");
    if ((is_figure_batch(b) || b.flush == 0x26cdb4) != b.clip.has_value())
        throw std::runtime_error("GUI batch: a 002bfa90 / 0035b390 / 0028d870 / 00341f00 / 0026ca70 figure batch or a 0026cdb4 IAMAP batch carries its own camera clip, no other does");
    // checkpoint 132ln4: GuiMissionList's selection bar, a four-vertex strip; 132on2h: GuiNetMessageArray's row bar
    // 00347b40 (the same instructions as 002ab2a0)
    if (b.source == 0x2ab2a0 || b.source == 0x347b40) {
        if (b.vertices.size() != 4 || b.strips != std::vector<std::int32_t>{4} || b.clip || b.perspective)
            throw std::runtime_error("002ab2a0 / 00347b40 selection bar batch of an unexpected shape");
        std::vector<GuiTriangleVertex> out;
        for (const int k : {0, 1, 2, 1, 2, 3}) {
            const GuiVertex& v = b.vertices[static_cast<std::size_t>(k)];
            GuiTriangleVertex t;
            t.pos = {v.pos[0], v.pos[1]};
            t.uv = {0.0F, 0.0F};
            t.colour = host_colour(v.colour);
            out.push_back(t);
        }
        return out;
    }
    if (b.vertices.size() % 3 != 0) throw std::runtime_error("00371200 batch with a partial triangle");
    std::size_t total = 0;
    for (const std::int32_t n : b.strips) {
        if (n < 0) throw std::runtime_error("GUI batch with a negative strip count");
        total += static_cast<std::size_t>(n);
    }
    if (total != b.vertices.size()) throw std::runtime_error("GUI batch strip counts disagree with its vertices");
    const bool textured = (b.render_mode & 1) != 0;
    std::vector<GuiTriangleVertex> out;
    out.reserve(b.vertices.size());
    const auto& m = b.matrix;
    for (const GuiVertex& v : b.vertices) {
        std::array<float, 4> p{};
        for (int c = 0; c < 4; ++c)
            p[c] = v.pos[0] * m[c] + v.pos[1] * m[4 + c] + v.pos[2] * m[8 + c] + v.pos[3] * m[12 + c];
        // A batch with its own camera clip is projected homogeneously (the clip divides by its w), so only the
        // orthographic paths need w 1 (132hud2: the centred map view's 0055c0c0 carries w 0.99999994).
        if (p[3] != 1.0F && !b.clip) throw std::runtime_error("00371200 vertex with w != 1 (a projected IE node is not hosted)");
        GuiTriangleVertex t;
        if (b.clip) {
            std::array<float, 4> c{};
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) c[r] += (*b.clip)[k][r] * p[k];
            t.pos = {c[0] * 320.0F, c[1] * 224.0F};
            t.depth = {0.5F * (c[2] + c[3]), c[3]};
        } else if (b.perspective) {
            std::array<float, 4> c{};
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) c[r] += view->clip[k][r] * p[k];
            t.pos = {c[0] * view->x_scale * 320.0F, c[1] * view->y_scale * 224.0F};
            t.depth = {0.5F * (c[2] + c[3]), c[3]};
        } else {
            t.pos = {p[0], p[1]};
        }
        t.uv = textured ? std::array<float, 2>{v.uv[0] - 2048.0F, v.uv[1] - 2048.0F} : std::array<float, 2>{0.0F, 0.0F};
        t.colour = host_colour(v.colour);
        out.push_back(t);
    }
    return out;
}

bool gui_batch_covers_frame(const std::vector<GuiTriangleVertex>& triangles) {
    if (triangles.empty()) return false;
    float x0 = triangles[0].pos[0], x1 = x0, y0 = triangles[0].pos[1], y1 = y0;
    for (const GuiTriangleVertex& t : triangles) {
        x0 = std::min(x0, t.pos[0]);
        x1 = std::max(x1, t.pos[0]);
        y0 = std::min(y0, t.pos[1]);
        y1 = std::max(y1, t.pos[1]);
    }
    return x0 <= -320.0F && x1 >= 320.0F && y0 <= -224.0F && y1 >= 224.0F;
}

void GuiRenderer::reset_device(std::shared_ptr<gfx::Device> device) {
    if (!device) throw std::runtime_error("GUI renderer: no device to reset onto");
    if (!keep_images_) throw std::runtime_error("GUI renderer: an offscreen renderer has no device to reset");
    if (!copy_images_) throw std::runtime_error("GUI renderer: no image copies to reset with (keep_image_copies off)");
    list_.clear();
    depth_image_.reset();
    depth_width_ = depth_height_ = 0;
    depth_test_.reset();
    repeat_.reset();
    blend_.reset();
    opaque_.reset();
    fade_.reset();
    brighten_.reset();
    raster_.reset();
    depth_.reset();
    sampler_.reset();
    point_.reset();
    wrap_.reset();
    white_view_.reset();
    vertices_.reset();
    vertex_capacity_ = 0;
    retained_.clear();
    textures_.clear();
    channel_restore_.clear();
    images_.clear();
    blur_copy_.reset();
    blur_small_.reset();
    wide_views_.clear();
    device_ = std::move(device);
    init_pipeline();
    for (const auto& [slot, image] : images_kept_) textures_[slot] = make_texture(image);
}

gfx::Viewport GuiRenderer::viewport(unsigned width, unsigned height, bool fit) const {
    if (!fit || !frame_fit_) return {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F};
    const FrameRect r = frame_rect(width, height);  // 132win10: the aspect setting (stretch fills the target)
    return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.width), static_cast<float>(r.height),
            0.0F, 1.0F};
}

GuiRenderer::GuiRenderer(std::shared_ptr<gfx::Device> device) : keep_images_(true), device_(std::move(device)) {
    if (!device_) throw std::runtime_error("GUI renderer: no device");
    try {
        init_pipeline();
    } catch (const DeviceLost&) {  // port-only (132win3): lost() until the owner re-creates the device
    }
}

GuiRenderer::GuiRenderer(unsigned width, unsigned height)
    : GuiRenderer(gfx::create_offscreen_device("Create GUI device"), width, height) {
    keep_images_ = false;  // its own device: not re-created by an owner (reset_device)
}

GuiRenderer::GuiRenderer(std::shared_ptr<gfx::Device> device, unsigned width, unsigned height)
    : keep_images_(true), device_(std::move(device)), width_(width), height_(height) {
    if (!device_) throw std::runtime_error("GUI renderer: no device");
    gfx::TextureDesc description;
    description.width = width;
    description.height = height;
    description.format = gfx::Format::bgra8;
    description.bind = gfx::bind_target;
    gfx::TextureLabels labels;
    labels.texture = "Create GUI target";
    labels.target = "Create GUI target view";
    image_ = device_->create_texture(description, nullptr, labels);
    init_pipeline();
}

GuiRenderer::~GuiRenderer() = default;

void GuiRenderer::init_pipeline() {
    device_->prepare(gfx::Program::gui);
    using F = gfx::BlendFactor;
    gfx::BlendDesc blending;
    auto& rt = blending.targets[0];
    rt.enable = true;
    rt.src = F::src_alpha;
    rt.dst = F::inv_src_alpha;
    rt.op = gfx::BlendOp::add;
    rt.src_alpha = F::one;
    rt.dst_alpha = F::inv_src_alpha;
    rt.op_alpha = gfx::BlendOp::add;
    rt.write_mask = gfx::mask_all;
    blend_ = device_->create_blend(blending, "Create GUI blend state");
    raster_ = device_->create_raster({false}, "Create GUI rasterizer state");
    depth_ = device_->create_depth({false, false, gfx::Compare::always}, "Create GUI depth state");
    depth_test_ = device_->create_depth({true, true, gfx::Compare::less}, "Create GUI figure depth state");
    sampler_ = device_->create_sampler({gfx::Filter::linear, gfx::Address::clamp, 0.0F}, "Create GUI sampler");
    point_ = device_->create_sampler({gfx::Filter::point, gfx::Address::clamp, 0.0F}, "Create GUI point sampler");
    // The gun preview's parts repeat their textures as the in-world gun draw does (scene_renderer).
    wrap_ = device_->create_sampler({gfx::Filter::linear, gfx::Address::clamp, 0.0F}, "Create GUI wrap sampler");
    repeat_ = device_->create_sampler({gfx::Filter::linear, gfx::Address::wrap, 0.0F}, "Create GUI figure sampler");
    rt.enable = false;
    opaque_ = device_->create_blend(blending, "Create GUI opaque blend state");
    rt.enable = true;
    {   // 0037a370 mode 3 (ALPHA 0x42): Cd - Cs * As >> 7.
        auto fade = blending;
        auto& f = fade.targets[0];
        f.enable = true;
        f.src = F::one;
        f.dst = F::one;
        f.op = gfx::BlendOp::rev_subtract;
        f.src_alpha = F::zero;
        f.dst_alpha = F::one;
        f.op_alpha = gfx::BlendOp::add;
        fade_ = device_->create_blend(fade, "Create GUI fade blend state");
        // 0037a370 slot 2 (ALPHA 0x48, the brightness quad +1d0): Cd + Cs * As >> 7.
        f.op = gfx::BlendOp::add;
        brighten_ = device_->create_blend(fade, "Create GUI brightness blend state");
    }
    // Untextured IE nodes (mode without 1): the vertex colour alone.
    const std::uint32_t white = 0xffffffffU;
    gfx::TextureDesc description;
    description.width = description.height = 1;
    description.format = gfx::Format::rgba8;
    description.usage = gfx::Usage::immutable;
    description.bind = gfx::bind_sampled;
    const gfx::Level level{&white, 4};
    gfx::TextureLabels labels;
    labels.texture = "Create GUI white texture";
    labels.sampled = "Create GUI white view";
    white_view_ = device_->create_texture(description, &level, labels);
}

void GuiRenderer::set_texture(std::uint32_t slot, const Texture& texture) try {
    streamed_.erase(slot);
    if (keep_images_ && copy_images_) images_kept_[slot] = texture;
    textures_[slot] = nullptr;
    if (!lost()) textures_[slot] = make_texture(texture);  // a lost device's image is made by reset_device
} catch (const DeviceLost&) {}

void GuiRenderer::stream_texture(std::uint32_t slot, const Texture& texture) try {
    if (lost()) return;
    if (texture.width == 0 || texture.height == 0 || texture.rgba.size() != std::size_t(texture.width) * texture.height * 4)
        throw std::runtime_error("Invalid GUI texture image");
    auto& t = textures_[slot];
    if (!t || !streamed_.count(slot) || t->desc().width != texture.width || t->desc().height != texture.height) {
        gfx::TextureDesc description;
        description.width = texture.width;
        description.height = texture.height;
        description.format = gfx::Format::rgba8;
        description.usage = gfx::Usage::gpu;
        description.bind = gfx::bind_sampled;
        const gfx::Level level{texture.rgba.data(), texture.width * 4};
        gfx::TextureLabels labels;
        labels.texture = "Create streamed GUI texture";
        labels.sampled = "Create streamed GUI texture view";
        t = device_->create_texture(description, &level, labels);
        streamed_.insert(slot);
        return;
    }
    gfx::Box box;
    box.right = texture.width;
    box.bottom = texture.height;
    gfx::CommandList list;
    list.update_texture(t.get(), box, texture.rgba.data(), texture.width * 4);
    device_->submit(list);
} catch (const DeviceLost&) {}

bool GuiRenderer::lost() const {
    // 132win3: also while a loss is pending on the session's device (a shared renderer; device_lost.hpp)
    return device_ && (device_->removed() || (keep_images_ && device_lost_pending()));
}

GuiRenderer::Ptr<gfx::Texture> GuiRenderer::make_texture(const Texture& source) {
    const Texture& texture = upload_image(source);  // port-only (132md1): a mod's replacement image
    if (texture.width == 0 || texture.height == 0
        || (!texture.block_bytes && texture.rgba.size() != std::size_t(texture.width) * texture.height * 4))
        throw std::runtime_error("Invalid GUI texture image");
    const auto upload = gfx::upload_levels(device_.get(), texture, false);   // port-only (132md11): a mod's BC1 / BC3 blocks
    gfx::TextureLabels labels;
    labels.texture = "Create GUI texture";
    labels.sampled = "Create GUI texture view";
    return device_->create_texture(upload.desc, upload.levels.data(), labels);
}

bool GuiRenderer::upload_vertices(const void* vertices, std::size_t count, std::size_t minimum, const char* map_label) {
    if (vertex_capacity_ < count) {
        vertex_capacity_ = std::max<std::size_t>(count, minimum);
        vertices_.reset();
        try {
            vertices_ = device_->create_buffer({vertex_capacity_ * sizeof(HostVertex), gfx::Usage::dynamic, gfx::bind_vertices},
                                               nullptr, "Create GUI vertex buffer");
        } catch (const DeviceLost&) {
            vertex_capacity_ = 0;
            return false;
        }
    }
    try {
        device_->write(*vertices_, vertices, count * sizeof(HostVertex), map_label);
    } catch (const DeviceLost&) {
        return false;
    }
    return true;
}

void GuiRenderer::draw(const GuiDrawList& list, const gfx::Texture* target, unsigned width, unsigned height,
                       const GuiViewProjection* view) try {
    OMEGA_ZONE("gr.draw");
    if (lost()) return;  // port-only (132win2): the frame is lost; the owner re-creates the device
    wide_edge_images_ = 0;
    struct Segment {
        std::uint32_t first = 0, count = 0;
        const gfx::Texture* view = nullptr;
        bool opaque = false, linear = true, wrap = false;
        bool depth = false, clear_depth = false, repeat = false;
        bool full = false;  // under the perspective camera record: the whole target with the frame fit (132win2)
        const gfx::Buffer* buffer = nullptr;  // 132md7: a retained batch's own vertices (null: the frame's buffer)
        std::uint32_t anchor = 0;             // 132ws: a retained batch's HUD element tag (wide)
        bool fit = false;  // 132ws: wide, but untagged 4:3 content: the fitted viewport clips it as the PS2 frame's edges do
    };
    std::vector<HostVertex>& vertices = frame_vertices_;
    vertices.clear();
    std::vector<Segment> segments;
    // Port-only (checkpoint 132ws): the wide aspect (frame_fit.hpp frame_wide). The frame's vertex ranges are recorded
    // with their HUD element tag (GuiDrawBatch::port_anchor) and kind, then mapped from the fitted 4:3 rectangle to the
    // whole target on the CPU (wide_map below) and drawn with the whole viewport.
    const bool wide = frame_fit_ && frame_wide(width, height);
    enum : std::uint8_t { wide_frame, wide_picture, wide_full, wide_cover, wide_aspect_picture };
    struct WideRange {
        std::size_t first = 0, end = 0;
        std::uint32_t anchor = 0;
        std::uint8_t kind = wide_frame;
        float aspect = 0.0F;   // wide_aspect_picture: GuiDrawBatch::port_picture_aspect
        MovieFit fit = MovieFit::bars;   // a picture's: movies= for a movie, movie_fill (fill / bars) otherwise
    };
    std::vector<WideRange> wide_ranges;
    std::uint32_t wide_anchor = 0;
    bool wide_pict = false, cover_run = false, wide_free = false, wide_movie = false;
    float wide_aspect = 0.0F;
    float backdrop_rows = 0.0F;  // 132ws: the GUI rows the frame-wide textured strips cover (a 4:3 backdrop picture)
    auto view_of = [&](std::uint32_t slot) {
        const auto it = textures_.find(slot);
        if (it == textures_.end())
            throw std::runtime_error("GUI draw with no image bound to texture slot " + std::to_string(slot));
        return static_cast<const gfx::Texture*>(it->second.get());
    };
    bool depth_run = false, clear_next = false, full_run = false;
    const bool movie_zoom = wide && frame_movie_zoom_setting();
    const MovieFit movie_fit = frame_movie_fit_setting();
    auto add = [&](const gfx::Texture* view, bool opaque, bool linear, std::size_t first, bool wrap = false) {
        const auto count = static_cast<std::uint32_t>(vertices.size() - first);
        if (count == 0) return;
        const std::uint8_t kind = full_run    ? wide_full
                                  : cover_run ? wide_cover
                                  : wide_pict ? (wide_aspect > 0.0F ? wide_aspect_picture : wide_picture)
                                              : wide_frame;
        const MovieFit picture_fit = wide_movie ? movie_fit : movie_zoom ? MovieFit::fill : MovieFit::bars;
        // A picture under the bars setting is the same: drawn as the pillarbox draws it (its overhang clipped).
        // 132ws2: a port_free batch (the callouts, the lock-on) is mapped over the whole target, not clipped.
        const bool fit = wide && wide_anchor == 0 && !wide_free &&
                         (kind == wide_frame || (kind == wide_picture && picture_fit == MovieFit::bars));
        if (wide && !fit) wide_ranges.push_back({first, vertices.size(), wide_anchor, kind, wide_aspect, picture_fit});
        if (!segments.empty() && !clear_next) {
            Segment& s = segments.back();
            if (s.view == view && s.opaque == opaque && s.linear == linear && s.wrap == wrap && s.depth == depth_run
                && s.repeat == depth_run && s.full == full_run && s.fit == fit && s.first + s.count == first) {
                s.count += count;
                return;
            }
        }
        segments.push_back({static_cast<std::uint32_t>(first), count, view, opaque, linear, wrap, depth_run, clear_next,
                            depth_run, full_run});
        segments.back().fit = fit;
        clear_next = false;
    };
    // Port-only (checkpoint 132ws2): a fit batch's art cut by the 4:3 frame's edges, extended into the side areas
    // (gui_wide_edge_images), drawn right after it with the same texture and sampler over the whole target.
    const auto edge_extend = [&](const GuiDrawBatch& b, std::size_t first, const gfx::Texture* tex, bool opaque, bool linear) {
        if (!wide || !wide_edge_extend_ || b.perspective || b.clip || b.port_anchor || b.port_picture || b.port_free) return;
        if (first >= vertices.size()) return;
        std::vector<GuiEdgeVertex> source;
        source.reserve(vertices.size() - first);
        for (std::size_t k = first; k < vertices.size(); ++k) {
            const HostVertex& v = vertices[k];
            if (v.w != 1.0F) return;
            source.push_back({v.x, v.y, v.u, v.v, v.r, v.g, v.b, v.a});
        }
        const auto images = gui_wide_edge_images(source, frame_wide_half_width(width, height));
        if (images.empty()) return;
        const std::size_t at = vertices.size();
        for (const GuiEdgeVertex& e : images) vertices.push_back({e.x, e.y, e.u, e.v, e.r, e.g, e.b, e.a});
        wide_edge_images_ += images.size() / 3;
        wide_free = true;
        add(tex, opaque, linear, at);
        wide_free = b.port_free;
    };
    // Checkpoint 132h: a run of 003836b0 batches (the HUD weapon preview's gun parts) is one model the
    // GS draws with its Z test; this target has no depth buffer, so the run's triangles are drawn far
    // to near by their mean GUI-space z (host choice). Checkpoint 132f6: smaller z is nearer. Every
    // 003836b0 caller's axes put the model's z row at -1 (x right, y up: the viewer sees the model's +z
    // side), and the GUNLIGHT rows (004053b0, all but the weak fill with +z) light that side in the
    // original's EQMODIFY boxes; the 132h ascending order showed the unlit -z side in front.
    std::size_t index = 0;
    const auto& batches = list.batches;
    while (index < batches.size()) {
        const GuiDrawBatch& b = batches[index];
        const std::size_t first = vertices.size();
        wide_anchor = b.port_anchor;
        wide_pict = b.port_picture;
        wide_movie = b.port_picture && b.port_movie;
        wide_aspect = b.port_picture ? b.port_picture_aspect : 0.0F;
        wide_free = b.port_free;
        // Checkpoint 132f6: a run of 002bfa90 batches (the loadout figure's parts) is one actor the GS draws
        // with its Z test under the figure's own camera: drawn here with a host depth buffer cleared at the
        // run's start (host choice), depth LESS with writes, wrapped texture addressing, the GUI blend.
        // Checkpoint 132f7: the stats screen's figures (0035b390) the same.
        if (is_figure_batch(b)) {
            OMEGA_ZONE("gr.figures");
            clear_next = true;
            depth_run = true;
            for (; index < batches.size() && batches[index].flush == b.flush; ++index) {
                const GuiDrawBatch& g = batches[index];
                const bool textured = (g.render_mode & 1) != 0;
                const std::size_t at = vertices.size();
                wide_anchor = g.port_anchor;
                for (const GuiTriangleVertex& t : gui_triangles(g, view))
                    vertices.push_back({t.pos[0], t.pos[1], t.uv[0], t.uv[1], t.colour[0], t.colour[1], t.colour[2],
                                        t.colour[3], t.depth[0], t.depth[1]});
                add(textured ? view_of(g.slot) : white_view_.get(), false, !textured || (g.filter & 0x20) != 0, at);
            }
            depth_run = false;
            clear_next = false;
            continue;
        }
        if (b.flush == 0x3836b0 && [&] {
                for (std::size_t k = index; k < batches.size() && batches[k].flush == 0x3836b0; ++k)
                    if (batches[k].shared_vertices) return true;
                return false;
            }()) {
            // Port-only (checkpoint 132md7, research/MODDING.md "Speed"): a run holding a mod's replaced part (a shared
            // batch, up to hundreds of thousands of triangles) is drawn with the host depth buffer cleared at the
            // run's start (depth LESS with writes, preview_depth of each vertex's GUI-space z) in batch order instead
            // of the mean-z order; a shared batch from its retained buffer. The order and the sampler are the run's.
            bool clear = true;
            const auto push = [&](const gfx::Texture* tv, bool linear, std::uint32_t at, std::uint32_t count, const gfx::Buffer* buffer) {
                if (count == 0) return;
                Segment s;
                s.first = at;
                s.count = count;
                s.view = tv;
                s.linear = linear;
                s.wrap = true;
                s.depth = true;
                s.clear_depth = clear;
                s.buffer = buffer;
                s.anchor = wide_anchor;
                if (wide && !buffer) wide_ranges.push_back({at, std::size_t(at) + count, wide_anchor, wide_frame});
                segments.push_back(s);
                clear = false;
            };
            for (; index < batches.size() && batches[index].flush == 0x3836b0; ++index) {
                const GuiDrawBatch& g = batches[index];
                if (g.perspective) throw std::runtime_error("003836b0 batch under a perspective camera record");
                const bool textured = (g.render_mode & 1) != 0;
                const gfx::Texture* tv = textured ? view_of(g.slot) : white_view_.get();
                wide_anchor = g.port_anchor;
                const bool linear = !textured || (g.filter & 0x20) != 0;
                if (g.shared_vertices) {
                    std::uint32_t count = 0;
                    const gfx::Buffer* buffer = retained_vertices(g, count);
                    push(tv, linear, 0, count, buffer);
                    continue;
                }
                const auto host = gui_triangles(g, view);
                const auto& m = g.matrix;
                const std::size_t at = vertices.size();
                for (std::size_t k = 0; k < host.size(); ++k) {
                    const auto& p = g.vertices[k].pos;
                    const GuiTriangleVertex& h = host[k];
                    vertices.push_back({h.pos[0], h.pos[1], h.uv[0], h.uv[1], h.colour[0], h.colour[1], h.colour[2], h.colour[3],
                                        preview_depth(p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + p[3] * m[14]), 1.0F});
                }
                push(tv, linear, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(vertices.size() - at), nullptr);
            }
            continue;
        }
        if (b.flush == 0x3836b0) {
            struct Triangle {
                float z = 0.0F;
                std::array<HostVertex, 3> v{};
                const gfx::Texture* view = nullptr;
                bool linear = true;
            };
            std::vector<Triangle> run;
            for (; index < batches.size() && batches[index].flush == 0x3836b0; ++index) {
                const GuiDrawBatch& g = batches[index];
                if (g.perspective) throw std::runtime_error("003836b0 batch under a perspective camera record");
                const bool textured = (g.render_mode & 1) != 0;
                const auto tv = gui_triangles(g, view);
                const auto& m = g.matrix;
                for (std::size_t k = 0; k < tv.size(); k += 3) {
                    Triangle t;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto& p = g.vertices[k + c].pos;
                        t.z += (p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + p[3] * m[14]) / 3.0F;
                        const GuiTriangleVertex& h = tv[k + c];
                        t.v[c] = {h.pos[0], h.pos[1], h.uv[0], h.uv[1], h.colour[0], h.colour[1], h.colour[2], h.colour[3],
                                  h.depth[0], h.depth[1]};
                    }
                    t.view = textured ? view_of(g.slot) : white_view_.get();
                    t.linear = !textured || (g.filter & 0x20) != 0;
                    run.push_back(t);
                }
            }
            std::stable_sort(run.begin(), run.end(), [](const Triangle& a, const Triangle& c) { return a.z > c.z; });
            for (const Triangle& t : run) {
                const std::size_t at = vertices.size();
                for (const HostVertex& v : t.v) vertices.push_back(v);
                add(t.view, false, t.linear, at, true);
            }
            continue;
        }
        ++index;
        if (is_line_batch(b)) {
            // Checkpoint 132cc5: GuiTextEditWidget's cursor line (0041a450, flush 003701b0). PORT-ONLY host
            // choice: the GS one-pixel line is drawn as a quad one GUI unit wide, untextured.
            if (b.vertices.size() != 2 || b.strips != std::vector<std::int32_t>{2})
                throw std::runtime_error("003701b0 batch that is not one two-vertex line");
            const GuiVertex& a = b.vertices[0];
            const GuiVertex& c = b.vertices[1];
            const auto ca = host_colour(a.colour);
            const auto cc = host_colour(c.colour);
            const HostVertex v[4] = {{a.pos[0], a.pos[1], 0, 0, ca[0], ca[1], ca[2], ca[3]},
                                     {a.pos[0] + 1.0F, a.pos[1], 0, 0, ca[0], ca[1], ca[2], ca[3]},
                                     {c.pos[0], c.pos[1], 0, 0, cc[0], cc[1], cc[2], cc[3]},
                                     {c.pos[0] + 1.0F, c.pos[1], 0, 0, cc[0], cc[1], cc[2], cc[3]}};
            for (const int k : {0, 1, 2, 2, 1, 3}) vertices.push_back(v[k]);
            add(white_view_.get(), false, true, first);
            continue;
        }
        if (is_ie_batch(b)) {
            const bool textured = (b.render_mode & 1) != 0;
            const auto tv = gui_triangles(b, view);
            for (const GuiTriangleVertex& t : tv)
                vertices.push_back({t.pos[0], t.pos[1], t.uv[0], t.uv[1], t.colour[0], t.colour[1], t.colour[2], t.colour[3],
                                    t.depth[0], t.depth[1]});
            full_run = b.perspective;
            if (frame_fit_ && (frame_cover_ || wide) && !b.perspective && !b.clip && !textured && gui_batch_covers_frame(tv)) {
                // Port-only (132win4): the backdrop tint over the whole scene. 132ws: when wide, any such batch is
                // edge-extended instead (wide_cover below: a ring such as the scope mask stays round).
                (wide ? cover_run : full_run) = true;
                ++frame_cover_draws_;
            }
            if (wide && textured && !b.perspective && !b.clip && !b.port_anchor && !b.port_picture && !tv.empty()) {
                // 132ws: a full-screen picture drawn as one batch or as frame-wide strips (the loadout screen's two halves).
                float x0 = tv[0].pos[0], x1 = x0, y0 = tv[0].pos[1], y1 = y0;
                for (const GuiTriangleVertex& t : tv) {
                    x0 = std::min(x0, t.pos[0]);
                    x1 = std::max(x1, t.pos[0]);
                    y0 = std::min(y0, t.pos[1]);
                    y1 = std::max(y1, t.pos[1]);
                }
                if (x0 <= -wide_backdrop_reach && x1 >= wide_backdrop_reach)
                    backdrop_rows += std::min(y1, 224.0F) - std::max(y0, -224.0F);
            }
            add(textured ? view_of(b.slot) : white_view_.get(), (b.render_mode & 0x200) != 0,
                !textured || (b.filter & 0x20) != 0, first);
            const bool covered = full_run || cover_run;
            full_run = cover_run = false;
            if (!covered)
                edge_extend(b, first, textured ? view_of(b.slot) : white_view_.get(), (b.render_mode & 0x200) != 0,
                            !textured || (b.filter & 0x20) != 0);
            continue;
        }
        std::vector<GuiQuad> quads;
        append_quads(b, quads);
        for (const GuiQuad& q : quads) {
            // Left edge: the first vertex's colour; right edge: the second's.
            const HostVertex tl{q.p0[0], q.p0[1], q.uv0[0], q.uv0[1], q.c0[0], q.c0[1], q.c0[2], q.c0[3]};
            const HostVertex bl{q.p0[0], q.p1[1], q.uv0[0], q.uv1[1], q.c0[0], q.c0[1], q.c0[2], q.c0[3]};
            const HostVertex tr{q.p1[0], q.p0[1], q.uv1[0], q.uv0[1], q.c1[0], q.c1[1], q.c1[2], q.c1[3]};
            const HostVertex br{q.p1[0], q.p1[1], q.uv1[0], q.uv1[1], q.c1[0], q.c1[1], q.c1[2], q.c1[3]};
            for (const HostVertex& v : {tl, tr, bl, bl, tr, br}) vertices.push_back(v);
        }
        // The subtitle sprites filter as their TEX1 pair says (MMAG, checkpoint 132cp11).
        add(view_of(b.slot), false, b.flush != 0x4118e0 || (b.filter & 0x20) != 0, first);
        edge_extend(b, first, view_of(b.slot), false, b.flush != 0x4118e0 || (b.filter & 0x20) != 0);
    }
    wide_backdrop_ = backdrop_rows >= wide_backdrop_rows;
    if (segments.empty()) return;  // 132md7: a frame of retained batches alone has no frame vertices
    const gfx::Viewport fitted = viewport(width, height, true), whole = viewport(width, height, false);
    std::map<std::uint32_t, float> wide_shift;  // 132ws: each tagged HUD element's x shift (GUI units)
    if (wide) {
        std::map<std::uint32_t, std::array<float, 2>> bounds;
        for (const WideRange& r : wide_ranges) {
            if (r.anchor == 0 || r.kind != wide_frame) continue;
            auto [it, fresh] = bounds.try_emplace(r.anchor, std::array<float, 2>{1e30F, -1e30F});
            for (std::size_t k = r.first; k < r.end; ++k) {
                const HostVertex& v = vertices[k];
                if (v.w <= 0.0F) continue;
                const float x = v.x / v.w;
                it->second[0] = std::min(it->second[0], x);
                it->second[1] = std::max(it->second[1], x);
            }
        }
        for (const auto& [tag, b] : bounds)
            if (b[0] <= b[1])
                wide_shift[tag] = frame_wide_anchor_shift(frame_wide_anchor_side(0.5F * (b[0] + b[1])), width, height,
                                                          frame_hud_margin_setting());
        const GuiWideMap m = gui_wide_map(width, height);
        for (const WideRange& r : wide_ranges) {
            if (r.kind == wide_full) continue;
            const auto it = r.anchor ? wide_shift.find(r.anchor) : wide_shift.end();
            const float shift = it == wide_shift.end() ? 0.0F : it->second;
            for (std::size_t k = r.first; k < r.end; ++k) {
                HostVertex& v = vertices[k];
                if (r.kind == wide_picture && r.fit == MovieFit::fill) {  // the 4:3 picture scaled to the target's width
                    v.x = v.x + m.ox * v.w;
                    v.y = v.y * m.ky / m.kx + m.oy * v.w;
                } else if (r.kind == wide_picture && r.fit == MovieFit::stretch) {  // 132vt2a2: over the whole target
                    v.x = v.x + m.ox * v.w;
                    v.y = v.y * m.ky + m.oy * v.w;
                } else if (r.kind == wide_aspect_picture) {
                    // 132md14: a picture of aspect A letterboxed in the canvas (height (4/3)/A of it). Across the
                    // target's width its height is T/A of the target's (T its aspect): bars scale it to fit (A < T:
                    // as tall as the target, pillarboxed); fill keeps the width (A < T: its top and bottom cut);
                    // 132vt2a2 stretch: its height scaled by A / (4/3) to the target's.
                    const float f = r.fit == MovieFit::bars
                                        ? std::min(1.0F, r.aspect * static_cast<float>(height) / static_cast<float>(width))
                                        : 1.0F;
                    v.x = v.x * f + m.ox * v.w;
                    v.y = (r.fit == MovieFit::stretch ? v.y * (r.aspect * 0.75F) * m.ky : v.y * m.ky / m.kx * f) + m.oy * v.w;
                } else if (r.kind == wide_cover) {
                    // Edge extension: a vertex on or past the frame's edge goes to the target's edge, the inner ones
                    // keep their 4:3 place (a backdrop tint covers the target; the scope mask's ring stays round).
                    const float edge = 319.5F * v.w;
                    v.x = v.x >= edge ? 320.0F * v.w : v.x <= -edge ? -320.0F * v.w : v.x * m.kx + m.ox * v.w;
                    v.y = v.y * m.ky + m.oy * v.w;
                } else {
                    v.x = (v.x + shift * v.w) * m.kx + m.ox * v.w;
                    v.y = v.y * m.ky + m.oy * v.w;
                }
            }
        }
        ++wide_draws_;
        wide_shifts_ = wide_shift;
    }
    OMEGA_ZONE("gr.upload_record");
    if (!vertices.empty() && !upload_vertices(vertices.data(), vertices.size(), 6 * 256, "Map GUI vertices")) return;

    auto& l = list_;
    l.clear();
    l.set_target(target);
    l.set_raster(raster_.get());
    l.set_depth(depth_.get());
    l.set_program(gfx::Program::gui);
    const gfx::Buffer* bound = vertices.empty() ? nullptr : vertices_.get();
    if (bound) l.set_vertex_buffer(bound, sizeof(HostVertex));
    // Segments go out in submission order.
    for (const Segment& s : segments) {
        if (const gfx::Buffer* want = s.buffer ? s.buffer : vertices_.get(); want != bound) {
            l.set_vertex_buffer(want, sizeof(HostVertex));  // 132md7: a retained batch's buffer and back
            bound = want;
        }
        if (wide && s.buffer && !s.full) {  // 132ws: a retained batch keeps the fitted viewport, moved with its element
            gfx::Viewport moved = fitted;
            const auto it = wide_shift.find(s.anchor);
            if (s.anchor && it != wide_shift.end()) moved.x += it->second * fitted.width / 640.0F;
            l.set_viewport(moved);
        } else {
            l.set_viewport(s.full || (wide && !s.fit) ? whole : fitted);
        }
        if (s.depth) {
            const gfx::Texture* dsv = depth_view(width, height);
            if (s.clear_depth) l.clear_depth(dsv, gfx::CommandList::clear_depth_bit, 1.0F, 0);
            l.set_target(target, dsv);
            l.set_depth(depth_test_.get());
        } else {
            l.set_target(target);
            l.set_depth(depth_.get());
        }
        l.set_blend(s.opaque ? opaque_.get() : blend_.get());
        l.set_sampler(0, s.repeat ? repeat_.get() : s.wrap ? wrap_.get() : s.linear ? sampler_.get() : point_.get());
        l.set_texture(0, s.view);
        l.draw(s.count, s.first);
    }
    l.set_texture(0, nullptr);
    l.set_target(target);
    l.set_depth(depth_.get());
    l.set_viewport(whole);
    {
        OMEGA_ZONE("gr.submit");
        device_->submit(l);
    }
    if (!retained_.empty()) {
        ++draw_serial_;
        for (auto it = retained_.begin(); it != retained_.end();)
            it = it->second.used + 8 < draw_serial_ ? retained_.erase(it) : std::next(it);
    }
} catch (const DeviceLost&) {
    list_.clear();
} catch (...) {
    list_.clear();
    throw;
}

const gfx::Buffer* GuiRenderer::retained_vertices(const GuiDrawBatch& b, std::uint32_t& count) {
    const auto& source = b.shared_vertices;
    if (source->size() % 3 != 0 || b.strips != std::vector<std::int32_t>{static_cast<std::int32_t>(source->size())}
        || !b.vertices.empty() || b.clip)
        throw std::runtime_error("003836b0 shared batch (132md7) of an unexpected shape");
    RetainedBatch& r = retained_[source.get()];
    r.used = draw_serial_;
    const bool textured = (b.render_mode & 1) != 0;
    if (r.buffer && r.source == source && r.matrix == b.matrix && r.textured == textured) {
        count = r.count;
        return r.buffer.get();
    }
    r.source.reset();
    const auto& m = b.matrix;
    std::vector<HostVertex> host(source->size());
    for (std::size_t k = 0; k < host.size(); ++k) {
        const GuiVertex& v = (*source)[k];
        std::array<float, 4> p{};
        for (int c = 0; c < 4; ++c) p[c] = v.pos[0] * m[c] + v.pos[1] * m[4 + c] + v.pos[2] * m[8 + c] + v.pos[3] * m[12 + c];
        if (p[3] != 1.0F) throw std::runtime_error("00371200 vertex with w != 1 (a projected IE node is not hosted)");
        const auto colour = host_colour(v.colour);
        host[k] = {p[0], p[1], textured ? v.uv[0] - 2048.0F : 0.0F, textured ? v.uv[1] - 2048.0F : 0.0F,
                   colour[0], colour[1], colour[2], colour[3], preview_depth(p[2]), 1.0F};
    }
    if (r.capacity < host.size() || !r.buffer) {
        r.buffer.reset();
        r.capacity = std::max<std::size_t>(host.size(), 3);
        r.buffer = device_->create_buffer({r.capacity * sizeof(HostVertex), gfx::Usage::dynamic, gfx::bind_vertices}, nullptr,
                                          "Create GUI retained vertex buffer");
    }
    if (!host.empty()) device_->write(*r.buffer, host.data(), host.size() * sizeof(HostVertex), "Map GUI retained vertices");
    r.source = source;
    r.matrix = b.matrix;
    r.textured = textured;
    r.count = static_cast<std::uint32_t>(host.size());
    count = r.count;
    return r.buffer.get();
}

const gfx::Texture* GuiRenderer::depth_view(unsigned width, unsigned height) {
    if (!depth_image_ || depth_width_ != width || depth_height_ != height) {
        depth_image_.reset();
        gfx::TextureDesc description;
        description.width = width;
        description.height = height;
        description.format = gfx::Format::d24s8;
        description.bind = gfx::bind_depth;
        gfx::TextureLabels labels;
        labels.texture = "Create GUI depth buffer";
        labels.depth = "Create GUI depth view";
        depth_image_ = device_->create_texture(description, nullptr, labels);
        depth_width_ = width;
        depth_height_ = height;
    }
    return depth_image_.get();
}

void GuiRenderer::draw_fade(const gfx::Texture* target, unsigned width, unsigned height, std::uint8_t alpha_1d4) try {
    if (alpha_1d4 == 0) return;
    // Cs * As >> 7 with Cs = 0x80 is As, subtracted from each channel.
    draw_full_screen_quad(target, width, height, static_cast<float>(alpha_1d4) / 255.0F, fade_.get());
} catch (const DeviceLost&) {}

void GuiRenderer::draw_brightness(const gfx::Texture* target, unsigned width, unsigned height,
                                  std::uint8_t alpha_1d0) try {
    if (alpha_1d0 == 0) return;
    // Cs * As >> 7 with Cs = 0x80 is As, added to each channel.
    draw_full_screen_quad(target, width, height, static_cast<float>(alpha_1d0) / 255.0F, brighten_.get());
} catch (const DeviceLost&) {}

void GuiRenderer::draw_vision_flair(const gfx::Texture* target, unsigned width, unsigned height, std::int32_t grey,
                                   std::int32_t alpha) try {
    const std::int32_t c = std::min(255, ((grey & 0xff) * (alpha & 0xff)) >> 7);
    if (c == 0) return;
    draw_full_screen_quad(target, width, height, static_cast<float>(c) / 255.0F, brighten_.get());
} catch (const DeviceLost&) {}

void GuiRenderer::draw_full_screen_quad(const gfx::Texture* target, unsigned width, unsigned height, float c,
                                        const gfx::BlendState* blend) {
    if (lost()) return;
    const HostVertex tl{-320.0F, 224.0F, 0, 0, c, c, c, 1}, tr{320.0F, 224.0F, 0, 0, c, c, c, 1};
    const HostVertex bl{-320.0F, -224.0F, 0, 0, c, c, c, 1}, br{320.0F, -224.0F, 0, 0, c, c, c, 1};
    const HostVertex quad[] = {tl, tr, bl, bl, tr, br};
    if (!upload_vertices(quad, 6, 6 * 256, "Map GUI fade vertices")) return;
    auto& l = list_;
    l.clear();
    l.set_target(target);
    l.set_viewport({0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F});
    l.set_raster(raster_.get());
    l.set_depth(depth_.get());
    l.set_program(gfx::Program::gui);
    l.set_vertex_buffer(vertices_.get(), sizeof(HostVertex));
    l.set_blend(blend);
    l.set_sampler(0, point_.get());
    l.set_texture(0, white_view_.get());
    l.draw(6, 0);
    l.set_texture(0, nullptr);
    device_->submit(l);
}

void GuiRenderer::draw_host_vertices(const gfx::Texture* target, unsigned width, unsigned height,
                                     const void* vertices, std::size_t count, const gfx::BlendState* blend,
                                     const gfx::SamplerState* sampler, const gfx::Texture* view) {
    if (count == 0 || lost()) return;
    if (!upload_vertices(vertices, count, 6 * 256, "Map GUI vision vertices")) return;
    auto& l = list_;
    l.clear();
    l.set_target(target);
    l.set_viewport({0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F});
    l.set_raster(raster_.get());
    l.set_depth(depth_.get());
    l.set_program(gfx::Program::gui);
    l.set_vertex_buffer(vertices_.get(), sizeof(HostVertex));
    l.set_blend(blend);
    l.set_sampler(0, sampler);
    l.set_texture(0, view);
    l.draw(static_cast<std::uint32_t>(count), 0);
    l.set_texture(0, nullptr);
    device_->submit(l);
}

const gfx::BlendState* GuiRenderer::channel_restore(std::uint8_t write_mask) {
    auto& state = channel_restore_[write_mask];
    if (!state) {
        gfx::BlendDesc blending;
        auto& rt = blending.targets[0];
        rt.enable = false;
        rt.src = rt.src_alpha = gfx::BlendFactor::one;
        rt.dst = rt.dst_alpha = gfx::BlendFactor::zero;
        rt.op = rt.op_alpha = gfx::BlendOp::add;
        rt.write_mask = write_mask;
        state = device_->create_blend(blending, "Create GUI channel restore blend state");
    }
    return state.get();
}

bool GuiRenderer::draw_wide_background(std::string_view screen, const gfx::Texture* target, unsigned width,
                                       unsigned height) try {
    if (!frame_fit_ || std::uint64_t(width) * 3 <= std::uint64_t(height) * 4 || !mods::active()) return false;
    const auto images = mods::wide_background_images(screen);
    if (!images) return false;
    const mods::WideImage* image = mods::pick_wide_background(*images, width, height);
    if (!image || lost()) return false;
    auto cached = wide_views_.find(image->texture.get());
    if (cached == wide_views_.end()) {
        if (wide_views_.size() >= 8) wide_views_.clear();   // other installations' or screens' images
        const auto upload = gfx::upload_levels(device_.get(), *image->texture, false);
        gfx::TextureLabels labels;
        labels.texture = "Create GUI wide background texture";
        labels.sampled = "Create GUI wide background view";
        auto view = device_->create_texture(upload.desc, upload.levels.data(), labels);
        cached = wide_views_.emplace(image->texture.get(), std::make_pair(image->texture, std::move(view))).first;
    }
    const auto& view = cached->second.second;
    const auto r = mods::wide_background_rect(image->width, image->height, width, height);
    const float x0 = static_cast<float>(320.0 * (2.0 * r.x / width - 1.0));
    const float x1 = static_cast<float>(320.0 * (2.0 * (r.x + r.width) / width - 1.0));
    const HostVertex tl{x0, 224.0F, 0, 0, 1, 1, 1, 1}, tr{x1, 224.0F, 1, 0, 1, 1, 1, 1};
    const HostVertex bl{x0, -224.0F, 0, 1, 1, 1, 1, 1}, br{x1, -224.0F, 1, 1, 1, 1, 1, 1};
    const HostVertex quad[] = {tl, tr, bl, bl, tr, br};
    draw_host_vertices(target, width, height, quad, 6, opaque_.get(), sampler_.get(), view.get());
    ++wide_background_draws_;
    return true;
} catch (const DeviceLost&) {
    return false;
}

const gfx::Texture* GuiRenderer::image_view(const Texture& source) {
    auto& view = images_[&source];
    if (!view) {
        const Texture& texture = upload_image(source);  // port-only (132md1): a mod's replacement image
        if (texture.width == 0 || texture.height == 0
            || (!texture.block_bytes && texture.rgba.size() != std::size_t(texture.width) * texture.height * 4))
            throw std::runtime_error("Invalid vision noise image");
        const auto upload = gfx::upload_levels(device_.get(), texture, false);   // port-only (132md11): BC1 / BC3 blocks
        gfx::TextureLabels labels;
        labels.texture = "Create vision noise texture";
        labels.sampled = "Create vision noise view";
        view = device_->create_texture(upload.desc, upload.levels.data(), labels);
    }
    return view.get();
}

void GuiRenderer::make_blur_images(const gfx::TextureDesc& fd) {
    if (!blur_copy_ || blur_copy_->desc().width != fd.width || blur_copy_->desc().height != fd.height
        || blur_copy_->desc().format != fd.format) {
        blur_copy_.reset();
        blur_small_.reset();
        fill_tiny_.reset();
        gfx::TextureDesc cd;
        cd.width = fd.width;
        cd.height = fd.height;
        cd.format = fd.format;
        cd.bind = gfx::bind_sampled;
        gfx::TextureLabels copy_labels;
        copy_labels.texture = "Create vision blur copy";
        copy_labels.sampled = "Create vision blur copy view";
        blur_copy_ = device_->create_texture(cd, nullptr, copy_labels);
        gfx::TextureDesc fx_buf;
        fx_buf.width = fx_buf.height = 256;
        fx_buf.format = fd.format;
        fx_buf.bind = gfx::bind_sampled | gfx::bind_target;
        gfx::TextureLabels fx_labels;
        fx_labels.texture = "Create ScreenFxBuf";
        fx_labels.sampled = "Create ScreenFxBuf view";
        fx_labels.target = "Create ScreenFxBuf target";
        blur_small_ = device_->create_texture(fx_buf, nullptr, fx_labels);
    }
}

GuiWideMap gui_wide_map(unsigned width, unsigned height) {
    GuiWideMap m;
    if (width == 0 || height == 0) return m;
    const FrameRect r = fit_frame(width, height);
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    m.kx = static_cast<float>(r.width) / w;
    m.ky = static_cast<float>(r.height) / h;
    m.ox = 320.0F * (2.0F * static_cast<float>(r.x) + static_cast<float>(r.width) - w) / w;
    m.oy = -224.0F * (2.0F * static_cast<float>(r.y) + static_cast<float>(r.height) - h) / h;
    return m;
}

std::vector<GuiEdgeVertex> gui_wide_edge_images(const std::vector<GuiEdgeVertex>& triangles, float half_width) {
    std::vector<GuiEdgeVertex> out;
    const float extra = half_width - 320.0F;
    if (!(extra > 0.0F)) return out;
    const auto lerp = [](const GuiEdgeVertex& p, const GuiEdgeVertex& q, float t) {
        const auto l = [t](float a, float b) { return a + (b - a) * t; };
        return GuiEdgeVertex{l(p.x, q.x), l(p.y, q.y), l(p.u, q.u), l(p.v, q.v), l(p.r, q.r), l(p.g, q.g), l(p.b, q.b), l(p.a, q.a)};
    };
    for (std::size_t k = 0; k + 2 < triangles.size(); k += 3) {
        const GuiEdgeVertex* t = &triangles[k];
        for (const float s : {-1.0F, 1.0F}) {
            const float edge = 320.0F * s;
            // The depth into the frame (0 on the edge, > 0 inside); the triangle must reach the edge.
            const auto depth = [&](const GuiEdgeVertex& p) { return (p.x - edge) * -s; };
            if (!(std::min({depth(t[0]), depth(t[1]), depth(t[2])}) <= 0.5F)) continue;
            // The part inside the frame (depth >= 0): one Sutherland-Hodgman pass.
            std::vector<GuiEdgeVertex> poly;
            for (std::size_t i = 0; i < 3; ++i) {
                const GuiEdgeVertex& p = t[i];
                const GuiEdgeVertex& q = t[(i + 1) % 3];
                const float dp = depth(p), dq = depth(q);
                if (dp >= 0.0F) poly.push_back(p);
                if ((dp < 0.0F) != (dq < 0.0F)) poly.push_back(lerp(p, q, dp / (dp - dq)));
            }
            if (poly.size() < 3) continue;
            float period = 0.0F;
            for (const GuiEdgeVertex& p : poly) period = std::max(period, depth(p));
            if (period < 0.5F) continue;
            const int images = std::min(64, static_cast<int>(std::ceil(extra / period)));
            for (int n = 1; n <= images; ++n) {
                std::vector<GuiEdgeVertex> img = poly;
                for (GuiEdgeVertex& p : img) {
                    const float d = std::max(depth(p), 0.0F);
                    const float out_by = (n % 2 != 0) ? static_cast<float>(n - 1) * period + d : static_cast<float>(n) * period - d;
                    p.x = edge + s * out_by;
                }
                for (std::size_t i = 1; i + 1 < img.size(); ++i) out.insert(out.end(), {img[0], img[i], img[i + 1]});
            }
        }
    }
    return out;
}

void GuiRenderer::draw_wide_fill(const gfx::Texture* target, unsigned width, unsigned height) try {
    if (lost() || !target || !frame_fit_ || !frame_wide(width, height)) return;
    const gfx::TextureDesc& fd = target->desc();
    if (fd.samples != 1) throw std::runtime_error("The wide side fill on a multisampled target");
    make_blur_images(fd);
    if (!fill_tiny_) {
        gfx::TextureDesc tiny;
        tiny.width = tiny.height = 32;
        tiny.format = fd.format;
        tiny.bind = gfx::bind_sampled | gfx::bind_target;
        gfx::TextureLabels labels;
        labels.texture = "Create wide fill image";
        labels.sampled = "Create wide fill view";
        labels.target = "Create wide fill target";
        fill_tiny_ = device_->create_texture(tiny, nullptr, labels);
    }
    const FrameRect r = fit_frame(width, height);
    const float u0 = static_cast<float>(r.x) / static_cast<float>(fd.width);
    const float u1 = static_cast<float>(r.x + r.width) / static_cast<float>(fd.width);
    const float v0 = static_cast<float>(r.y) / static_cast<float>(fd.height);
    const float v1 = static_cast<float>(r.y + r.height) / static_cast<float>(fd.height);
    const auto quad = [](std::vector<HostVertex>& out, float x0, float x1, float s0, float s1, float t0, float t1, float c) {
        const HostVertex tl{x0, 224.0F, s0, t0, c, c, c, 1}, tr{x1, 224.0F, s1, t0, c, c, c, 1};
        const HostVertex bl{x0, -224.0F, s0, t1, c, c, c, 1}, br{x1, -224.0F, s1, t1, c, c, c, 1};
        out.insert(out.end(), {tl, tr, bl, bl, tr, br});
    };
    const gfx::TargetView none_target{};
    list_.clear();
    list_.set_targets(1, &none_target, nullptr);
    list_.copy(blur_copy_.get(), target);
    device_->submit(list_);
    std::vector<HostVertex> v;
    quad(v, -320.0F, 320.0F, u0, u1, v0, v1, 1.0F);  // the fitted 4:3 rectangle to 256 x 256
    draw_host_vertices(blur_small_.get(), 256, 256, v.data(), v.size(), opaque_.get(), sampler_.get(), blur_copy_.get());
    list_.set_targets(1, &none_target, nullptr);
    device_->submit(list_);
    v.clear();
    quad(v, -320.0F, 320.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F);  // and on to 32 x 32
    draw_host_vertices(fill_tiny_.get(), 32, 32, v.data(), v.size(), opaque_.get(), sampler_.get(), blur_small_.get());
    list_.set_targets(1, &none_target, nullptr);
    device_->submit(list_);
    // The side areas in the whole target's GUI units (x / half width), mirrored at the 4:3 rectangle's edges.
    const GuiWideMap m = gui_wide_map(width, height);
    const float left = -320.0F, inner_l = -320.0F * m.kx + m.ox, inner_r = 320.0F * m.kx + m.ox;
    const float side_l = (inner_l - left) / (640.0F * m.kx), side_r = (320.0F - inner_r) / (640.0F * m.kx);
    const float dim = 0.45F;
    v.clear();
    if (side_l > 0.0F) quad(v, left, inner_l, std::min(side_l, 1.0F), 0.0F, 0.0F, 1.0F, dim);
    if (side_r > 0.0F) quad(v, inner_r, 320.0F, 1.0F, 1.0F - std::min(side_r, 1.0F), 0.0F, 1.0F, dim);
    draw_host_vertices(target, width, height, v.data(), v.size(), opaque_.get(), sampler_.get(), fill_tiny_.get());
} catch (const DeviceLost&) {
    list_.clear();
}

std::size_t GuiRenderer::draw_vision_layers(const gfx::Texture* target, unsigned width, unsigned height,
                                            const VisionLayers& layers) try {
    if (lost()) return 0;
    const auto quad = [](std::vector<HostVertex>& out, float x0, float y0, float x1, float y1,
                         const std::array<float, 4>& c) {
        const HostVertex tl{x0, y0, 0, 0, c[0], c[1], c[2], c[3]}, tr{x1, y0, 1, 0, c[0], c[1], c[2], c[3]};
        const HostVertex bl{x0, y1, 0, 1, c[0], c[1], c[2], c[3]}, br{x1, y1, 1, 1, c[0], c[1], c[2], c[3]};
        out.insert(out.end(), {tl, tr, bl, bl, tr, br});
    };
    std::vector<HostVertex> v;
    // The mask restore: the channels FBMSK kept from the scene and the quad hold the clear colour (black).
    std::uint8_t masked = 0;
    if (layers.fbmsk & 0x000000ffU) masked |= gfx::mask_r;
    if (layers.fbmsk & 0x0000ff00U) masked |= gfx::mask_g;
    if (layers.fbmsk & 0x00ff0000U) masked |= gfx::mask_b;
    if (masked) {
        quad(v, -320.0F, 224.0F, 320.0F, -224.0F, {0.0F, 0.0F, 0.0F, 1.0F});
        draw_host_vertices(target, width, height, v.data(), v.size(), channel_restore(masked), point_.get(),
                           white_view_.get());
    }
    // 0025ec20: the noise tiles.
    std::size_t tiles = 0;
    if (layers.noise_w > 0.0F && layers.noise_h > 0.0F) {
        if (!layers.noise) throw std::runtime_error("The vision noise tiles (0025ec20) without the noise image");
        v.clear();
        const std::array<float, 4> c{1.0F, 1.0F, 1.0F, layers.noise_alpha};
        for (float y = 224.0F; y > -224.0F; y -= layers.noise_h)
            for (float x = -320.0F; x < 320.0F; x += layers.noise_w) {
                quad(v, x, y, x + layers.noise_w, y - layers.noise_h, c);
                ++tiles;
            }
        draw_host_vertices(target, width, height, v.data(), v.size(), blend_.get(), sampler_.get(),
                           image_view(*layers.noise));
    }
    // 0025e750: the scan band.
    v.clear();
    quad(v, -320.0F, layers.band_y0, 320.0F, layers.band_y1, {0.0F, 0.0F, 0.0F, layers.band_alpha});
    draw_host_vertices(target, width, height, v.data(), v.size(), blend_.get(), point_.get(), white_view_.get());
    // 0037b770: the frame down to ScreenFxBuf (256 x 256) and back, bilinear both ways.
    const gfx::TextureDesc& fd = target->desc();
    if (fd.samples != 1) throw std::runtime_error("The vision blur (0037b770) on a multisampled target");
    make_blur_images(fd);
    const gfx::TargetView none_target{};
    list_.clear();
    list_.set_targets(1, &none_target, nullptr);
    list_.copy(blur_copy_.get(), target);
    device_->submit(list_);
    v.clear();
    quad(v, -320.0F, 224.0F, 320.0F, -224.0F, {1.0F, 1.0F, 1.0F, 1.0F});
    draw_host_vertices(blur_small_.get(), 256, 256, v.data(), v.size(), opaque_.get(), sampler_.get(), blur_copy_.get());
    list_.set_targets(1, &none_target, nullptr);
    device_->submit(list_);
    draw_host_vertices(target, width, height, v.data(), v.size(), opaque_.get(), sampler_.get(), blur_small_.get());
    return tiles;
} catch (const DeviceLost&) {
    list_.clear();
    return 0;
}

void GuiRenderer::clear(const std::array<float, 4>& rgba) try {
    if (!image_) throw std::runtime_error("GuiRenderer::clear needs the offscreen target");
    list_.clear();
    list_.clear_target({image_.get(), 0}, rgba.data());
    device_->submit(list_);
} catch (const DeviceLost&) {}

void GuiRenderer::draw(const GuiDrawList& list) {
    if (!image_) throw std::runtime_error("GuiRenderer::draw needs the offscreen target");
    draw(list, image_.get(), width_, height_);
}

void GuiRenderer::capture(const std::filesystem::path& path) try {
    if (!image_) throw std::runtime_error("GuiRenderer::capture needs the offscreen target");
    gfx::TextureDesc description = image_->desc();
    description.usage = gfx::Usage::staging;
    description.bind = 0;
    gfx::TextureLabels labels;
    labels.texture = "Create GUI readback";
    const auto staging = device_->create_texture(description, nullptr, labels);
    list_.clear();
    list_.copy(staging.get(), image_.get());
    device_->submit(list_);
    std::vector<std::uint8_t> pixels(std::size_t(width_) * height_ * 4);
    device_->read(*staging, 0, [&](const std::uint8_t* data, unsigned row_pitch) {
        for (unsigned y = 0; y < height_; ++y)
            std::memcpy(pixels.data() + std::size_t(y) * width_ * 4, data + std::size_t(y) * row_pitch, width_ * 4);
    }, "Read GUI frame");
    write_bmp(path, width_, height_, pixels);
} catch (const DeviceLost&) {}

}  // namespace omega
