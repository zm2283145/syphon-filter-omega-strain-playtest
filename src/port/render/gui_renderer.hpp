#pragma once
// Checkpoint 122a: the host 2D backend for the GUI primitive list
// (src/main/gui/gui_draw.hpp). It stands where the renderer flush 00370550
// (VIF / GIF packet build and the VU1 program) hands the batches to the GS;
// everything here is a host choice, documented in research/FRONTEND.md
// "Checkpoint 122a GUI draw":
//   - GUI space (the front-end canvas's ortho camera, 00415bb0 -> 00375390)
//     is taken as 640 x 448 with x right, y up and the origin at the centre,
//     stretched over the whole target (NDC x = x / 320, y = y / 224), or,
//     with the port-only frame fit (checkpoint 132win2, set_frame_fit), over
//     the target's centred 4:3 rectangle (frame_fit.hpp);
//   - every vertex pair (top-left, bottom-right) of a batch is one
//     axis-aligned quad, the left corners with the first vertex's colour and
//     the right ones with the second's (the fade's per-edge alpha), the
//     carried pair at the start of each strip after the first skipped;
//   - texture coordinates are the vertex u, v less 0025f220's 2048 base;
//     colour = texture x vertex / 128 (GS modulate, 0x80 = 1.0), saturated;
//   - straight alpha blending (src alpha, 1 - src alpha), no depth test,
//     no culling, batches in submission order, bilinear clamp sampling.
// Checkpoint 122b adds the IE node batches (flush 00371200, ie_draw.hpp),
// host choices documented in FRONTEND.md "Checkpoint 122b GUI decor":
//   - each three vertices are one triangle (003ed880 emits whole triangles;
//     the strip records only carry the GS break words), position = vertex x
//     batch matrix (row vector, the VU1 transform's model part; w must stay
//     1), the result's x / y in the GUI space above;
//   - textured iff the render mode has 1 (else the vertex colour alone),
//     opaque (no blending) iff it has 0x200, bilinear iff the bound
//     record's filter bits have 0x20 (003820e0: 0x120 / 0x160), else point;
//   - the per-child GS packet 0037a1e0 (00415e58) is not modelled: no
//     depth buffer, so it has nothing to reset.
// Checkpoint 132f6: a run of 002bfa90 batches (the loadout screen's figure,
// GuiDrawBatch::clip) is drawn with a host D24 depth buffer the size of the
// target, cleared at the run's start, depth LESS with writes (the GS Z test
// of the actor draw), wrapped addressing, the straight alpha blend.

#include "gfx_device.hpp"
#include "gui_decor.hpp"
#include "gui_draw.hpp"
#include "gui_runtime.hpp"
#include "texture.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// One host quad of a batch (exposed for the tests of the expansion).
struct GuiQuad {
    std::uint32_t slot = 0;
    std::array<float, 2> p0{}, p1{};    // GUI space corners (first / second vertex)
    std::array<float, 2> uv0{}, uv1{};  // texture space (0..1)
    std::array<float, 4> c0{}, c1{};    // vertex colour / 128, saturated to 0..255 / 128
};
// The quads of a list (throws on a batch with an odd vertex count or strip
// counts that disagree with its vertices).
// (00370550 batches only; 00371200 batches are gui_triangles'.)
std::vector<GuiQuad> gui_quads(const GuiDrawList& list);

// One host vertex of a 00371200 batch (or a 003836b0 batch: the HUD weapon
// preview's gun parts, Checkpoint 132h; a run of those is drawn far to near
// by mean z, as this target has no depth buffer).
struct GuiTriangleVertex {
    std::array<float, 2> pos{};     // GUI space (vertex x batch matrix)
    std::array<float, 2> uv{};      // texture space (0 when untextured)
    std::array<float, 4> colour{};  // vertex colour / 128, saturated
    std::array<float, 2> depth{0.0F, 1.0F};  // host clip z / w (0 / 1 unless perspective)
};
// Checkpoint 132r: the camera record a perspective batch (GuiDrawBatch::
// perspective, a +61 node) is drawn under. clip holds the columns of
// P x A (record +50, the Y / Z swap +e80; renderer +f00 = (P x A) x top,
// 0037d9a0): clip-space c = sum clip[k] * p[k] for the camera-space p =
// vertex x batch matrix. Host choices: c's x / y over w are the window's
// NDC (the record's viewport V+f80 spans the PS2 frame) with x times
// x_scale and y times y_scale (frame_perspective_scale, frame_fit.hpp: the
// play window keeps the 4:3 frame's view in the fit rectangle, so in a wider
// window x_scale = (4 / 3) / (width / height), in a narrower one (132win3)
// y_scale = (3 / 4) * (width / height), puts the vertex where the 3D scene
// draws the same point); D3D11 clips (z' = (c.z + c.w) / 2 in 0..w, the GL
// depth range of 003754a0) where the GS / VU1 program would.
struct GuiViewProjection {
    std::array<std::array<float, 4>, 4> clip{};
    float x_scale = 1.0F;
    float y_scale = 1.0F;
};
// Throws for a perspective batch without `view`.
std::vector<GuiTriangleVertex> gui_triangles(const GuiDrawBatch& batch, const GuiViewProjection* view = nullptr);
// Port-only (132vf1, research/VITA.md "Front-end speed (132vf1)"): the same triangles appended to `out` (the
// renderer's per-frame scratch, so a frame's draw allocates nothing once it has grown).
void gui_triangles_append(const GuiDrawBatch& batch, const GuiViewProjection* view, std::vector<GuiTriangleVertex>& out);

// Port-only (checkpoint 132win4): a flat (no perspective record, no camera clip), untextured 00371200
// batch whose GUI-space bounds cover the whole 640 x 448 frame (x -320..320, y -224..224). The
// original's full-frame backdrop tints are such batches: GAMEBACK's objectivescreen_background (the start
// menu, 002429f0, layer -1; also ENDBACK, SUMMARY, AWARDS) is a 662 x 504 quad, x -330.96..330.96,
// z -251.98..251.98 (IE positions, its group's translation cancels), RGBA (0.035, 0.090, 0.133, 0.75 /
// 0.85): on the PS2 it overscans the frame, so every displayed pixel of the scene behind is tinted.
bool gui_batch_covers_frame(const std::vector<GuiTriangleVertex>& triangles);

// Port-only (checkpoint 132ws): the wide aspect's map of a GUI vertex (x, y, w homogeneous, GUI units) from the
// fitted 4:3 rectangle (fit_frame) to the whole width x height target: x' = x * kx + ox * w, y' = y * ky + oy * w,
// drawn with the whole viewport, puts the vertex on the pixel the fitted viewport would (the pillarbox place). An
// anchored HUD element adds its frame_wide_anchor_shift to x first; a movie_fill zoom picture uses x + ox * w and
// y * ky / kx + oy * w (the 4:3 picture scaled to the target's width).
struct GuiWideMap {
    float kx = 1.0F, ky = 1.0F, ox = 0.0F, oy = 0.0F;
};
GuiWideMap gui_wide_map(unsigned width, unsigned height);

// Port-only (checkpoint 132ws2): the wide aspect's edge extension of 4:3 panel art (the play overlay's menus,
// GuiRenderer::set_wide_edge_extend). For each triangle (flat GUI units, w 1) reaching the 4:3 frame's left or
// right edge (x <= -319.5 / >= 319.5): its part inside the frame, mirrored at that edge and repeated (mirrored,
// plain, mirrored ...) with the period of its depth into the frame, until half_width (frame_wide_half_width) is
// covered. Returns triangles (three vertices each) outside the frame only; none for half_width <= 320.
struct GuiEdgeVertex {
    float x = 0.0F, y = 0.0F, u = 0.0F, v = 0.0F;
    float r = 0.0F, g = 0.0F, b = 0.0F, a = 0.0F;
};
// 132ws5: the wide menu draw passes max_overhang (a triangle crossing the edge by more is not mirrored) and min_width
// (a triangle narrower than it, an end bracket or a thin line, is not mirrored).
std::vector<GuiEdgeVertex> gui_wide_edge_images(const std::vector<GuiEdgeVertex>& triangles, float half_width,
                                                float max_overhang = 1e30F, float min_width = 0.0F);
// 132vf1: the same images appended to `out`.
void gui_wide_edge_images_append(const std::vector<GuiEdgeVertex>& triangles, float half_width, float max_overhang,
                                 float min_width, std::vector<GuiEdgeVertex>& out);
// Port-only (checkpoint 132ws9): the parts of the triangles inside (rows y0..y1) or outside that band, triangulated;
// and the triangles with each point's u, v moved to the texel the triangle maps at (x, y): the row y stretched.
std::vector<GuiEdgeVertex> gui_edge_rows(const std::vector<GuiEdgeVertex>& triangles, float y0, float y1, bool inside);
std::vector<GuiEdgeVertex> gui_edge_sample_row(std::vector<GuiEdgeVertex> triangles, float y);

// Port-only (checkpoint 132ws5, research/FRONTEND.md "Checkpoint 132ws5"): the wide menu draw's trim of a batch's
// triangles (flat GUI units, w 1): a triangle that crosses the 4:3 frame's left or right edge by at most `overhang`
// units (art ending just past the PS2 frame, which its overscan hid) is cut at that edge, so gui_wide_edge_images
// mirrors its inside part; one that overhangs further (art continuing past the frame: the CMDCENTR ring) is kept
// whole and drawn past the edge, as is one narrower than menu_extend_span (a highlight's end bracket: drawn whole,
// not mirrored). A vertex less than `reach` inside an edge, of a triangle at least menu_extend_span wide,
// goes to the edge (art ending a few units short of the PS2 frame, the title's and the options' backdrops, then
// meets its mirror image without a dark seam). Returns triangles (three vertices each).
std::vector<GuiEdgeVertex> gui_wide_menu_trim(const std::vector<GuiEdgeVertex>& triangles, float overhang,
                                              float reach = 0.0F);
// 132vf1: the same triangles appended to `out`.
void gui_wide_menu_trim_append(const std::vector<GuiEdgeVertex>& triangles, float overhang, float reach,
                               std::vector<GuiEdgeVertex>& out);
// Port-only (checkpoint 132ws5): the wide menu draw's spread of a screen's header and legend. `bounds`: each screen
// element's (GuiDrawBatch::port_menu) x0, x1, y0, y1 (flat GUI units, y up). An element lying in the header band
// (y0 >= menu_header_y) or the legend band (y1 <= menu_legend_y), narrower than menu_spread_width and not reaching a
// 4:3 edge (a band backdrop extended into the sides) is grouped with the band's neighbours less than menu_spread_gap
// apart (an icon and its label); each group moves by c * (half_width / 320 - 1), c its centre, so the row spreads
// in proportion to the target's width. Everything else (the centred content) does not move. Returns tag -> shift.
// The draw gives a batch lying in a band its own tag when its element does not spread (gui_wide_menu_spreads false:
// ACNTLOGN's "Accept" label is part of the login form's element while its Square icon is an element of its own;
// ONLIPREF's d-pad icon the other way round), so a label and its icon move together. Such a tag carries
// menu_split_tag, and in the header band a group of split tags alone does not move (132ws8: the title menu's NEW
// AGENT, the top item of the list element reaching up into the band, stays in its column; DOSSIER's "Agent:" label
// moves with the name). The legend's split icons and labels spread alone as before.
inline constexpr std::uint32_t menu_split_tag = 0x80000000U;
inline constexpr float menu_header_y = 168.0F;
inline constexpr float menu_legend_y = -165.0F;
inline constexpr float menu_spread_width = 360.0F;
inline constexpr float menu_spread_gap = 12.0F;
inline constexpr float menu_trim_overhang = 16.0F;
inline constexpr float menu_edge_reach = 4.0F;
inline constexpr float menu_extend_span = 24.0F;
std::map<std::uint32_t, float> gui_wide_menu_shifts(const std::map<std::uint32_t, std::array<float, 4>>& bounds,
                                                    float half_width);
// Whether bounds (x0, x1, y0, y1) lie in the header or legend band and are narrow enough to spread (the test above).
bool gui_wide_menu_spreads(const std::array<float, 4>& b);

// The three fonts the application registers at boot (002cb934 / 002cb95c /
// 002cb984 -> 004185a0): "default" (DEFAULT.FNT, the manager's default +10),
// "callout" (CALLOUT.FNT) and "largefnt" (LARGEFNT.FNT), all from
// GAMEDATA/COMMON/FONTS.HOG with the TDX each .FNT names. Host choice: the
// handles are the boot texture slots 12 / 13 / 14 the fonts bind; the
// texture width is the decoded TDX's (256 / 256 / 512).
class DiscImage;
struct GuiDiscFonts {
    std::map<std::uint32_t, GuiFont> fonts;    // handle -> font
    std::map<std::uint32_t, Texture> images;   // slot -> image
    static constexpr std::uint32_t default_handle = 12;
    // 00418420(manager, name): NULL / "" or an unregistered name -> the
    // default font; otherwise the lower-cased name's registration.
    std::uint32_t handle(const std::string& name) const;
    const GuiFont* font(std::uint32_t handle) const;
};
GuiDiscFonts load_gui_fonts(DiscImage& disc);
// FRONTEND/NTSC.HOG > <screen>.HOG > <screen>.GUI (the 0036e1b0 open of
// "<SCREEN>.GUI" the manager's 00419110 performs, from the disc).
std::vector<std::uint8_t> load_frontend_gui(DiscImage& disc, const std::string& screen);

// Checkpoint 122b: the decorator elements' IE files and textures (host
// choice for the file system the IE manager's 00413124 open "%s.IE"
// (0036e1b0) and the texture manager's name lookup 00380a40 read, both of
// which see every mounted HOG):
//   - <NAME>.IE from FRONTEND/NTSC.HOG > <NAME>.HOG when NTSC.HOG has that
//     member, else from COMMON/<NAME>.HOG (GAME.IE, the HUD's); none:
//     throws naming 00413124. Member names match without case, first match
//     (the HOG lookup 0036d2a0 with strcasecmp 00128e80; the IEs name
//     "dpad.TDX" where SHELL.HOG stores DPAD.TDX);
//   - a node texture "<T>.TDX" from the HOG the IE came from (missing:
//     throws naming 00380a40), decoded by decode_tdx; one slot per stem
//     (00380a40: after the last '\', up to the first '.', without case),
//     numbered from first_slot in load order (the boot fonts hold 12..14);
//     a name not ending in "tdx" (00380a40 answers -1) throws naming 00380c68;
//   - the slot record's +48 word is not modelled (record_flags_known false:
//     a textured node's +63 stays unresolved, never read by the draw).
class GuiDecorAssets final : public IeTextureService {
public:
    explicit GuiDecorAssets(DiscImage& disc) : disc_(disc) {}
    static constexpr std::int32_t first_slot = 0x10;
    std::optional<GuiDecoIe> resolve(const std::string& upper_name);
    std::int32_t load(const std::string& name) override;
    std::uint32_t record_flags(std::int32_t index) override;
    bool record_flags_known() const override { return false; }
    const std::map<std::uint32_t, Texture>& images() const { return images_; }
    // Port-only (132vp1, research/VITA.md "Playability, memory and coverage"): the PS Vita's front end drops its
    // decoded pictures and HOG copies at the level hand-off (its renderer is released there too); a slot keeps its
    // number and is decoded again from its HOG on its stem's next load or by restore_images (the front end's
    // resume). release_images answers the bytes freed.
    std::size_t release_images();
    void restore_images();

private:
    std::vector<std::uint8_t> read_hog(const std::string& name);
    const std::vector<std::uint8_t>& hog(const std::string& name);
    void restore_slot(std::uint32_t slot);
    DiscImage& disc_;
    std::map<std::string, std::unique_ptr<IeFile>> files_;
    std::map<std::string, std::vector<std::uint8_t>> hogs_;  // IE name -> its HOG
    const std::vector<std::uint8_t>* current_ = nullptr;     // the HOG of the IE being loaded
    std::string current_name_;
    std::map<std::string, std::int32_t> slots_;
    std::map<std::uint32_t, Texture> images_;
    std::map<std::uint32_t, std::pair<std::string, std::string>> sources_;  // slot -> (IE name, TDX member)
    std::set<std::uint32_t> released_;
};

// Checkpoint 126c: the textures of an IE read from one mounted HOG (the
// loading screen's LOADING.HOG, 003f96e0 / 00241b70), as GuiDecorAssets::load
// over that HOG: a TDX member by name without case (missing: throws naming
// 00380a40), one slot per stem numbered from first_slot; the record +48
// word is not modelled.
class GuiHogTextures final : public IeTextureService {
public:
    GuiHogTextures(std::vector<std::uint8_t> hog, std::string label, std::int32_t first_slot = 0x10);
    // A member of the HOG by name without case (the 0036d2a0 lookup), or nullopt.
    std::optional<std::vector<std::uint8_t>> member(const std::string& name) const;
    std::int32_t load(const std::string& name) override;
    std::uint32_t record_flags(std::int32_t index) override;
    bool record_flags_known() const override { return false; }
    const std::map<std::uint32_t, Texture>& images() const { return images_; }

private:
    std::vector<std::uint8_t> hog_;
    std::string label_;
    std::int32_t first_slot_;
    std::map<std::string, std::int32_t> slots_;
    std::map<std::uint32_t, Texture> images_;
};

// A GUI runtime host for screens that are only drawn (the front-end capture
// and the play overlay). The services a screen asks for while idle answer
// as at boot: fonts as 00418420 over the boot registrations, strings through
// `strings` (003fdb50; none given: throws), no memory card, no demo, sound
// accepted unplayed (the front end's 00465c50 result 1). The title screen's
// wake 002882b0 selects the current profile (003361b0: the profile
// manager's reset, +8ac / +8bc / +8cc) and sets the application state
// (002c9af0(app, 0)); nothing drawn reads either, so they are recorded in
// `recorded`, not applied (host choice). Services a choice or popup reaches
// (screen factory, popups, the other app calls) throw naming their address.
class GuiStaticHost : public gui::GuiHost {
public:
    using Strings = std::function<std::optional<std::string>(const std::string& key)>;
    GuiStaticHost(const GuiDiscFonts& fonts, Strings strings) : fonts_(fonts), strings_(std::move(strings)) {}
    std::int32_t ms = 0;
    std::vector<std::string> recorded;
    std::optional<std::string> get_string(const std::string& key) override;
    std::uint32_t font(const std::string& name) override { return fonts_.handle(name); }
    std::uint32_t default_font() override { return GuiDiscFonts::default_handle; }
    std::uint8_t font_height(std::uint32_t f) override;
    std::int32_t font_width(std::uint32_t f, const std::string& s, std::int32_t n) override;
    std::int32_t ie_lookup(const std::string&) override { return -1; }
    std::int32_t demo_mode() override { return 0; }
    std::int32_t time_ms() override { return ms; }
    std::uint8_t save_present() override { return 0; }
    void card_refresh() override {}
    std::int32_t card_space_needed(std::int32_t) override { return 0; }
    std::int32_t profile_size(std::uint32_t) override { return 0; }
    std::int32_t current_profile() override { return 0; }
    void profile_select(std::uint32_t) override;
    void app_2c9af0(std::int32_t) override;
    void app_131470(std::int32_t) override;
    void app_2c82f0() override;
    void app_2c81d0() override;
    std::int32_t popup_open(std::int32_t, const std::optional<std::string>&, const std::optional<std::string>&,
                            const gui::Widget*, std::int32_t) override;
    void popup_close(std::int32_t) override;
    std::int32_t popup_query(std::int32_t) override;
    std::uint8_t sound(std::uint32_t, const gui::Widget*, bool) override { return 1; }
    void pad_reset() override {}
    void text_object(const gui::TextObject&) override {}
    void set_text(const std::string&) override {}
    gui::Widget* factory(const std::string& cls, const std::string&) override;
    void release(gui::Widget*) override {}

private:
    const GuiDiscFonts& fonts_;
    Strings strings_;
};

// Checkpoint 132mt1: every GPU call goes through the gfx layer (gfx_device.hpp; the D3D11 backend gfx_d3d11.cpp).
// The vertex of gfx::Program::gui (HLSL in gfx_d3d11_shaders.cpp): position = (x / 320, y / 224, z, w), colour =
// saturate(texel * vertex colour).
struct GuiHostVertex {
    float x, y, u, v;
    float r, g, b, a;
    float z = 0.0F, w = 1.0F;  // clip z / w: 0 / 1 except for a perspective batch (Checkpoint 132r)
};

class GuiRenderer {
public:
    // Draws into a caller's render target on an existing device (the 3D
    // scene's back buffer).
    explicit GuiRenderer(std::shared_ptr<gfx::Device> device);
    // Its own device (hardware, else WARP) and an offscreen BGRA target.
    GuiRenderer(unsigned width, unsigned height);
    // Port-only (checkpoint 132mt3): an offscreen BGRA target on a caller's device (no device of its own: making a
    // D3D device can wait minutes on the Windows loader when the machine is saturated; research/THREADING.md).
    GuiRenderer(std::shared_ptr<gfx::Device> device, unsigned width, unsigned height);
    GuiRenderer(const GuiRenderer&) = delete;
    GuiRenderer& operator=(const GuiRenderer&) = delete;
    ~GuiRenderer();

    // The image bound to a texture-manager slot (a font's +0, a decorator
    // node's +18).
    void set_texture(std::uint32_t slot, const Texture& texture);
    // Port-only (132vt1, research/VITA.md "Movies"): a picture that changes every frame (a movie's) rewritten in place
    // in one texture of the slot, instead of a new texture per picture (set_texture). Not kept for reset_device.
    void stream_texture(std::uint32_t slot, const Texture& texture);
    // Port-only (checkpoint 132win5): a renderer whose owner re-creates it and uploads its images again
    // after a device loss (the front end's make_screen) need not keep copies for reset_device, which then
    // throws. Saves a copy of each image and movie picture on a screen load.
    void keep_image_copies(bool on) {
        copy_images_ = on;
        if (!on) images_kept_.clear();
    }
    // A slot's image released (the texture manager freed it).
    void drop_texture(std::uint32_t slot) {
        textures_.erase(slot);
        images_kept_.erase(slot);
    }
    // Port-only (checkpoint 132md12, research/MODDING.md "Wide backgrounds"; the hook the wide aspect calls, research/
    // FRONTEND.md "Wide backgrounds from mods"): a mod's native wide background for a screen key ("loading.toronto1",
    // "frontend.<screen>": mods::wide_background_images, the image nearest the target's aspect) drawn opaque and
    // bilinear over the width x height target at the target's height, centred (mods::wide_background_rect), so the
    // caller then draws the 4:3 screen over it and skips its own side fill. Only with the frame fit on and a target
    // wider than 4:3. Returns whether it drew: false, with nothing read or drawn, without mods or an image for the screen.
    bool draw_wide_background(std::string_view screen, const gfx::Texture* target, unsigned width, unsigned height);
    std::uint64_t wide_background_draws() const { return wide_background_draws_; }
    // Port-only (checkpoint 132win2): the frame fit. Off (the default, and every offscreen 640 x 448 capture):
    // the GUI's 640 x 448 space covers the whole target, as before. On (the game window: front end, movies,
    // loading screen, play HUD): draw() puts it in the centred 4:3 rectangle of the target (frame_fit.hpp),
    // pillarboxed or letterboxed, never stretched; the caller clears the bars black. Batches under the
    // perspective camera record (GuiViewProjection, the lock marker) keep the whole target, as they follow the
    // 3D scene, which fills the window (its x_scale is the target's), and the full-screen quads (fade,
    // brightness, vision layers) cover the whole target as the scene does.
    void set_frame_fit(bool on) { frame_fit_ = on; }
    bool frame_fit() const { return frame_fit_; }
    // Port-only (checkpoint 132win4): over a scene that fills the window (the play overlay), a batch that
    // covers the frame (gui_batch_covers_frame: the start menu's and level end's backdrop tint) is drawn
    // over the whole target, as on the PS2 it covers every pixel of the scene; the menus stay in the fit
    // rectangle. Only with the frame fit on, so the offscreen 640 x 448 captures are unchanged.
    void set_frame_cover(bool on) { frame_cover_ = on; }
    bool frame_cover() const { return frame_cover_; }
    std::uint64_t frame_cover_draws() const { return frame_cover_draws_; }
    // Port-only (checkpoint 132ws): the wide aspect (frame_fit.hpp frame_wide: the wide setting, the frame fit on
    // and a target wider than 4:3). draw() then maps the frame from the fitted 4:3 rectangle to the whole target
    // (gui_wide_map, the same pixels as pillarbox) except:
    //   - a HUD element (GuiDrawBatch::port_anchor, the HUD screen root's children and their texts) whose bounds'
    //     centre is left / right of the frame_wide_anchor_band moves to the target's left / right edge less the
    //     HUD margin (frame_wide_anchor_shift), at its PS2 size; a retained batch moves its fitted viewport;
    //   - every flat untextured batch covering the frame (gui_batch_covers_frame: backdrop tints, dark overlays,
    //     full-screen flashes, the scope mask) is edge-extended: its vertices on or past the frame's edge go to
    //     the target's edge, the inner ones keep their 4:3 place (a tint covers the target, a ring stays round);
    //   - a picture batch (GuiDrawBatch::port_picture: the loading screen) stays 4:3 with bars, or with
    //     movie_fill zoom is scaled to the target's width (top and bottom cut). A movie (GuiDrawBatch::port_movie,
    //     132vt2a2) follows frame_movie_fit_setting instead: fill (the zoom's mapping, the default), bars, or stretch
    //     (the 4:3 picture over the whole target). Attract demos use the same fit choice; an explicit
    //     port_movie_native_bars override remains available to callers requiring a 4:3 draw.
    //     A picture with GuiDrawBatch::port_picture_aspect (132md14: a mod movie wider than 4:3, letterboxed in the
    //     canvas) is drawn at that aspect across the target's width (bars: scaled to fit when narrower than the target;
    //     stretch: over the whole target);
    //   - 132ws2: a port_free batch (the HUD's callouts / name tags and lock-on elements, placed by the 3D view) is
    //     mapped over the whole target unclipped, so a marker past the 4:3 edge lands on its Hor+ place.
    // Everything else (menu content, 4:3 artwork, a picture under bars) is drawn in the fitted rectangle exactly
    // as pillarbox draws it, clipped at the 4:3 edges, so nothing that overhangs the PS2 frame leaks into the
    // sides (132ws5: under set_wide_menu, the front end's, it is drawn unclipped; see there). A 4:3 backdrop drawn as textured strips that together cover the frame counts as one backdrop
    // (wide_backdrop). wide_draws counts the draws that applied it.
    std::uint64_t wide_draws() const { return wide_draws_; }
    // The last wide draw's anchored HUD elements: tag -> x shift (GUI units; 0 for one that stays centred).
    const std::map<std::uint32_t, float>& wide_shifts() const { return wide_shifts_; }
    // Whether the last wide draw had a 4:3 backdrop picture: a flat textured batch, not a HUD element nor a
    // picture, whose bounds cover the frame (the play's loadout screen). The caller then adds draw_wide_fill.
    bool wide_backdrop() const { return wide_backdrop_; }
    // Port-only (checkpoint 132ws): the wide aspect's side fill for a 4:3 picture (132ws5: the front end only where no edge image was drawn): the
    // target's fitted 4:3 rectangle copied, shrunk (256 x 256 then 32 x 32, bilinear: a strong blur), and drawn
    // dimmed into the side areas left and right of it, mirrored at the rectangle's edges, so a 4:3 background
    // extends without being stretched. No-op unless frame_wide(width, height) with the frame fit on. Needs a
    // single-sample target that can be copied (as the vision blur). A caller first tries draw_wide_background
    // (a mod's native wide image, drawn before the screen) and skips this fill when that returned true.
    void draw_wide_fill(const gfx::Texture* target, unsigned width, unsigned height);
    // Port-only (132vp4, research/VITA.md "Render scale"): a scaled scene (SceneRenderer::scene_view) drawn over the
    // whole target with bilinear filtering, opaque, before the HUD / GUI draw at the target's full size.
    void draw_upscale(const gfx::Texture* target, unsigned width, unsigned height, const gfx::Texture* scene);
    // Port-only (checkpoint 132ws2): under the wide aspect, the 4:3 panel art the frame's edges cut (flat batches drawn
    // in the fitted rectangle, textured or untextured, IE or sprite: not covers, HUD elements, pictures or
    // perspective / clip batches) is extended into the side areas with gui_wide_edge_images, each image drawn right
    // after its batch (same texture and sampler, the whole viewport). Off by default; the play overlay sets it.
    // Wide menus use coherent background expansion instead of edge mirroring. wide_edge_images counts the triangles
    // the last draw added.
    void set_wide_edge_extend(bool on) { wide_edge_extend_ = on; }
    bool wide_edge_extend() const { return wide_edge_extend_; }
    std::size_t wide_edge_images() const { return wide_edge_images_; }
    // Port-only (checkpoint 132ws5, research/FRONTEND.md "Checkpoint 132ws5"): the front end's wide menus (the front
    // end and the Vita set it; the play overlay does not). Under the wide aspect draw() then also:
    //   - expands a flat textured backdrop group covering at least 95% of the 4:3 frame across the target width;
    //   - draws other flat content unclipped at its 4:3 place, so art continuing past the frame remains visible;
    //   - spreads the header and legend elements (GuiDrawBatch::port_menu) with gui_wide_menu_shifts.
    // Menus never mirror a moving element at the 4:3 boundary.
    // wide_menu_shifts: the last draw's moved elements (tag -> x shift, GUI units).
    void set_wide_menu(bool on) { wide_menu_ = on; }
    bool wide_menu() const { return wide_menu_; }
    const std::map<std::uint32_t, float>& wide_menu_shifts() const { return wide_menu_shifts_; }
    // Port-only (checkpoint 132win2): device-lost recovery. Re-creates every GPU object on a new device and
    // context (the shared game window's, after HostWindow::recover_device) and uploads the slot images again
    // from the copies set_texture keeps for a renderer on a caller's device; the lazily made images (vision
    // noise, depth, blur) are made again on use. Draws after it give the same pixels as before the loss.
    void reset_device(std::shared_ptr<gfx::Device> device);
    void draw(const GuiDrawList& list, const gfx::Texture* target, unsigned width, unsigned height,
              const GuiViewProjection* view = nullptr);
    // 0037a9f0's full-screen fade quad (the video object's +1d4, written by
    // the world fader alpha 0025e5b0) in 0037a370 mode 3, ALPHA_1 (GS 0x42)
    // with the ALPHA value of slot 3 of the table 004bc1b0 (0x80_00000042:
    // A = 0, B = Cs, C = As, D = Cd, so Cd - (Cs * As >> 7)) and the quad's
    // colour 0x80: Cd - As per channel, COLCLAMP to 0, black at As = 255.
    // Host: blend REV_SUBTRACT, ONE / ONE, with the source colour As / 255
    // on the UNORM target (exact).
    void draw_fade(const gfx::Texture* target, unsigned width, unsigned height, std::uint8_t alpha_1d4);
    // The quad 0037a9f0 draws before that one when the video object's +1d0
    // (005383c0, the brightness option: cvt.w.s(191.25 * options +1c) & 0xff
    // from Game_MainUpdate 00167ec8 / 00168268 / 001684f0 / 00168c34 and
    // 0045a3a0 bit 0x400) is nonzero: 0037a370 slot 2, 0x80_00000048 (A = Cs,
    // B = 0, C = As, D = Cd, so Cd + (Cs * As >> 7)) with colour 0x80: Cd + As
    // per channel, COLCLAMP to 255. Host: blend ADD, ONE / ONE, source As / 255.
    void draw_brightness(const gfx::Texture* target, unsigned width, unsigned height, std::uint8_t alpha_1d0);
    // The vision effects' full-screen quad (0025f930 in the scene frame, vision_fx.hpp): 0037a1c0 with the colour
    // (R, G, B, A) in 0037a370 slot 2 (ALPHA 0x48, Cd + (Cs * As >> 7), COLCLAMP to 255), the flash white-out.
    // Host: the brightness blend with the source channel min(255, C * A >> 7) / 255 (R = G = B in mode 0).
    void draw_vision_flair(const gfx::Texture* target, unsigned width, unsigned height, std::int32_t grey,
                           std::int32_t alpha);
    // Checkpoint 132cp4: the rest of 0025f930 in vision modes 1 / 2 (vision_fx.hpp VisionFxDraw), drawn after the
    // scene and the vision quad (draw_vision_flair) went to the target with every channel written:
    //   - the frame-buffer mask: the original draws the scene and the quad under FBMSK `fbmsk` (0xffff00ff night:
    //     green only; 0xff00ffff thermal: blue only) over a frame cleared to black (0037f574: RGB 0), so the
    //     masked channels hold 0 when the mask is restored. Host: one opaque black quad written only to the
    //     masked channels (exact, as every blend is per channel);
    //   - 0025ec20's noise tiles: `noise` (the current animation frame) in tiles of noise_w x noise_h GUI units
    //     from (-320, 224) right and down while x < 320 / y > -224, UV 0..1 per tile, colour (1, 1, 1,
    //     noise_alpha) (1 = 0x80), ALPHA 0x44 (the straight alpha blend), bilinear (host: the GUI sampler);
    //   - 0025e750's band: untextured (0, 0, 0, band_alpha) from y0 to y1 across the screen, ALPHA 0x44;
    //   - 0037b770's blur: the target copied (0037bd40) to a 256 x 256 image ("ScreenFxBuf") with one bilinear
    //     sprite, then drawn back over the target (0037b810) bilinear and opaque. Host: D3D11 linear sampling at
    //     the texel centres (the GS sprite's own sample offsets are not modelled).
    // `noise` may be null only when noise_w / noise_h are 0 (no tiles). Returns the tile count.
    struct VisionLayers {
        std::uint32_t fbmsk = 0xff000000U;
        const Texture* noise = nullptr;
        float noise_w = 0.0F, noise_h = 0.0F, noise_alpha = 0.0F;
        float band_y0 = 0.0F, band_y1 = 0.0F, band_alpha = 0.0F;
    };
    std::size_t draw_vision_layers(const gfx::Texture* target, unsigned width, unsigned height,
                                   const VisionLayers& layers);
    // Offscreen target only.
    void clear(const std::array<float, 4>& rgba);
    void draw(const GuiDrawList& list);
    void capture(const std::filesystem::path& path);

private:
    template <class T> using Ptr = std::shared_ptr<T>;
    void init_pipeline();
    Ptr<gfx::Texture> make_texture(const Texture& texture);
    bool lost() const;  // the device was removed (draws are skipped until reset_device)
    gfx::Viewport viewport(unsigned width, unsigned height, bool fit) const;
    bool frame_fit_ = false;
    bool frame_cover_ = false;
    std::uint64_t frame_cover_draws_ = 0;
    std::uint64_t wide_draws_ = 0;
    std::map<std::uint32_t, float> wide_shifts_;
    bool wide_backdrop_ = false;
    bool wide_edge_extend_ = false;      // 132ws2
    std::size_t wide_edge_images_ = 0;   // 132ws2
    bool wide_menu_ = false;             // 132ws5
    std::map<std::uint32_t, float> wide_menu_shifts_;
    bool copy_images_ = true;  // 132win5: keep_image_copies
    bool keep_images_ = false;  // a renderer on a caller's device keeps its slot images (reset_device)
    std::map<std::uint32_t, Texture> images_kept_;
    void draw_full_screen_quad(const gfx::Texture* target, unsigned width, unsigned height, float colour,
                               const gfx::BlendState* blend);
    void draw_host_vertices(const gfx::Texture* target, unsigned width, unsigned height, const void* vertices,
                            std::size_t count, const gfx::BlendState* blend, const gfx::SamplerState* sampler,
                            const gfx::Texture* view);
    // The vertices to the dynamic buffer (grown to at least `count`); false when a lost device dropped them.
    bool upload_vertices(const void* vertices, std::size_t count, std::size_t minimum, const char* map_label);
    const gfx::BlendState* channel_restore(std::uint8_t write_mask);
    const gfx::Texture* image_view(const Texture& texture);
    // Checkpoint 132f6: the 002bfa90 figure runs' host depth buffer (D24, the target's size).
    const gfx::Texture* depth_view(unsigned width, unsigned height);
    Ptr<gfx::Texture> depth_image_;
    unsigned depth_width_ = 0, depth_height_ = 0;
    Ptr<gfx::DepthState> depth_test_;
    Ptr<gfx::SamplerState> repeat_;  // the figure parts' wrapped addressing
    Ptr<gfx::Device> device_;
    gfx::CommandList list_;
    Ptr<gfx::BlendState> blend_;
    Ptr<gfx::BlendState> opaque_;
    Ptr<gfx::BlendState> fade_;
    Ptr<gfx::BlendState> brighten_;
    Ptr<gfx::RasterState> raster_;
    Ptr<gfx::DepthState> depth_;
    Ptr<gfx::SamplerState> sampler_;
    Ptr<gfx::SamplerState> point_;
    Ptr<gfx::SamplerState> wrap_;  // the 003836b0 gun preview batches
    Ptr<gfx::Texture> white_view_;
    Ptr<gfx::Texture> gs_untextured_view_;
    Ptr<gfx::Buffer> vertices_;
    std::size_t vertex_capacity_ = 0;
    // Port-only (checkpoint 132md7, research/MODDING.md "Speed"): the host vertices of the shared 003836b0 batches
    // (GuiDrawBatch::shared_vertices, a mod's replaced gun part) in buffers of their own, per source, rebuilt when the
    // batch matrix or source changes; dropped a few draws after their last use. Empty without mods.
    struct RetainedBatch {
        std::shared_ptr<const std::vector<GuiVertex>> source;
        std::array<float, 16> matrix{};
        bool textured = false;
        Ptr<gfx::Buffer> buffer;
        std::size_t capacity = 0;
        std::uint32_t count = 0;
        std::uint64_t used = 0;
    };
    std::map<const void*, RetainedBatch> retained_;
    std::uint64_t draw_serial_ = 0;
    const gfx::Buffer* retained_vertices(const GuiDrawBatch& batch, std::uint32_t& count);
    std::map<std::uint32_t, Ptr<gfx::Texture>> textures_;
    std::set<std::uint32_t> streamed_;  // stream_texture's slots
    std::vector<GuiHostVertex> frame_vertices_;  // port-only: draw()'s vertices, kept so a frame does not reallocate
    // The vision layers (Checkpoint 132cp4): the write-mask blends, the noise frames and the blur images.
    std::map<std::uint8_t, Ptr<gfx::BlendState>> channel_restore_;
    std::map<const Texture*, Ptr<gfx::Texture>> images_;
    std::vector<GuiHostVertex> vision_vertices_;  // retained: night vision's tiles and quads
    // 132md12: the wide backgrounds' images (kept with their source, so a pointer is never reused while cached).
    std::map<const Texture*, std::pair<std::shared_ptr<const Texture>, Ptr<gfx::Texture>>> wide_views_;
    std::uint64_t wide_background_draws_ = 0;
    Ptr<gfx::Texture> blur_copy_, blur_small_;
    Ptr<gfx::Texture> fill_tiny_;  // 132ws: draw_wide_fill's 32 x 32 image
    // The vision blur's copy / ScreenFxBuf images for a target (made on first use or a target of another shape).
    void make_blur_images(const gfx::TextureDesc& target);
    // offscreen
    unsigned width_ = 0, height_ = 0;
    Ptr<gfx::Texture> image_;
};

}  // namespace omega
