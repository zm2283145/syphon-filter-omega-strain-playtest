#pragma once
// Checkpoint 122a: the game's 2D GUI draw path, from the widget draw
// virtuals down to the primitive list the renderer's flush receives.
//
//   00415bb0  GuiFrontendCanvas draw: visible children (flags & 2), each
//             through its +44 draw object (the widget or its decorator)
//   00417340  GuiInterfaceDecorator draw: the IE part (decorator +24, never
//             attached natively) and then the widget's draw
//   0041e470  GuiWidget draw: visible children; the focus highlight
//             0041e560 (flags & 0x100) is unhosted
//   0041c150  GuiTextWidget / GuiButtonWidget draw: base draw, the object
//             colour (003d0810), rect (cvt.w.s of +34..+40) and 003cfab0
//   003cfab0  text object draw (layout: scroll / ellipsis / single run /
//             word wrap with the +54 character fill), 003d04c0 line count
//   0036aa50  font string width, 0036a330 font string draw
//   0036afc0  font constructor (vtable 004df630), 0036ab80 .FNT load
//
// The primitive-list boundary is the renderer flush 00370550 (VIF / GIF
// packet build and the VU1 program): gui_font_draw returns the batch the
// flush would receive (the vertex arrays of V = 005381f0's open batch, the
// strip counts and the render state words), and the host draws it.
//
// Coordinates are the ortho camera space the front-end canvas sets up
// (00415bb0 -> 00375390): x right, y up, origin at the screen centre; text
// widgets draw with an identity model matrix (0041c150).

#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct FontFile;
struct IeTextObject;
struct IeDrawState;
class GuiDecor;
namespace gui {
struct Widget;
struct TextObject;
class GuiRuntime;
}  // namespace gui

// The 0x8a78-byte font object 0036afc0 constructs and 0036ab80 fills.
//   +0     texture slot (the texture manager *00539248's table +8)
//   +10    8 x float4 colour table (0036afc0: 0.5, 0.5, 0.5, 1.0); control
//          bytes 0x10..0x17 select an entry
//   +90    byte 1 (0036afc0)
//   +94    space advance (float)
//   +98    cell width, +99 line height (0036a2f0)
//   +9c    glyph count
//   +a0    glyphs: u0, u1, v0, v1 (texture space, 0..1)
//   +be0   kerning flag, +be1 count * count int8 kerning table
//   +8a74  vtable 004df630
struct GuiFont {
    static constexpr std::size_t image_size = 0x8a78;
    std::vector<std::uint8_t> image = std::vector<std::uint8_t>(image_size, 0);
    // The slot texture's width (u16 +4 of the texture's +44 record), as
    // 0036a330 / 0036aa50 read it.
    std::uint16_t tex_width = 0;

    // 0036afc0 + 0036ab80 for a decoded .FNT bound to texture slot `slot`.
    static GuiFont from_fnt(const FontFile& fnt, std::uint32_t slot, std::uint16_t tex_width);

    std::uint32_t slot() const;
    std::uint8_t height() const { return image[0x99]; }
    std::uint32_t count() const;
    float space() const;
    bool kerning() const { return image[0xbe0] != 0; }
    std::array<float, 4> colour(std::uint32_t index) const;  // +10 + 16 index
    float f(std::size_t offset) const;
    std::int8_t kern(std::int64_t index) const;  // +be1 + index (inside the image)
};

// FNV-1a 64 of the image +4..+8a74 (the goldens' font check).
std::uint64_t gui_font_image_hash(const GuiFont& font);

// 0036aa50(font, s, n): the string's width, truncated (cvt.w.s). `s` must
// hold the terminator; a read past the span throws.
std::int32_t gui_font_width(const GuiFont& font, std::span<const std::uint8_t> s, std::int32_t n);

struct GuiVertex {
    std::array<float, 4> pos{};            // x, y, z, 1
    std::array<std::int32_t, 4> colour{};  // cvt.w.s(128 * rgba) (0025c9d0)
    std::array<float, 4> uv{};             // u + 2048, v + 2048, 1, 0 (0025f220)
};

// One flush 00370550: the batch 0036a330 filled.
struct GuiDrawBatch {
    std::uint32_t render_mode = 0;  // *005392d0 (0036a330 stores 5)
    std::uint32_t slot = 0;         // *00539250, bound by 003820e0
    std::uint32_t prim = 0;         // V+1048 (3: mode & 1)
    std::uint32_t strip_max = 0;    // V+104c (0x40: mode & 1)
    std::uint64_t v8 = 0;           // V+8 (6 << 32: colour + texture arrays)
    // Vertex counts of the strips 0036a330 opened (00025cd10 at entry, one
    // more by 0025ca40 each time V+1040 reaches V+104c). Every strip after
    // the first starts with a copy of the previous strip's last two vertices.
    std::vector<std::int32_t> strips;
    std::vector<GuiVertex> vertices;
    // Checkpoint 122b: the flush that receives the batch (00370550 for
    // font strings, 00371200 for IE node triangles, ie_draw.hpp), the top
    // of the matrix stack (V+a80 + 64 * V+a74, rows; a vertex p maps to
    // p x matrix) when it flushes, and for 00371200 the twelve break words
    // of each strip record (+80..+ac, 003ee128) and the bound texture's
    // filter bits (record +58 & 0x1e1, written by 003820e0; 0 untextured).
    // Checkpoint 132h: 003836b0 for a part of the HUD weapon preview (a
    // triangle list of the gun part placed by its eleven matrices, the
    // matrix the IE top after the fit transform 003e8248..003e87b8; slot a
    // host texture slot).
    std::uint32_t flush = 0x370550;
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<std::array<std::uint32_t, 12>> breaks;
    std::uint32_t filter = 0;
    // Checkpoint 132r: flushed while the current camera record is
    // perspective (+104 = 0, a +61 node's 003ed240): the matrix then maps a
    // vertex into the record's camera space, which the renderer's +f00
    // ((P x A) x top, 0037d9a0) projects with the record's +50.
    bool perspective = false;
    // Checkpoint 132f6: flush 0x2bfa90 for a part of the loadout screen's figure (002bfa90's actor draw
    // under its own camera record 0): the vertices are world positions (matrix the identity) and `clip`
    // holds the columns of that record's P x A x view (eqmodify_figure_clip_2bfa90).
    // Checkpoint 132f7: flush 0x35b390 the same for the stats screen's figures (0035b390, the same camera record).
    // Checkpoint 132cc5: flush 0x3701b0 for GuiTextEditWidget's cursor (0041a450): one untextured
    // two-vertex line (prim 2) at GUI-space positions (z 1, w 0), matrix the identity.
    std::optional<std::array<std::array<float, 4>, 4>> clip;
    // Checkpoint 132ln4: 0x2ab2a0 for GuiMissionList's selection bar (002ab2a0): four untextured vertices
    // (x, y, 1, 0) in one 00371200 strip, under the identity top. The renderer draws them as that strip's two
    // triangles (static inference: the reachable form fills the row; the unreachable outline uses 003701b0).
    std::uint32_t source = 0;
    // Port-only (checkpoint 132md7, research/MODDING.md "Speed"): a 003836b0 batch of a mod's replacement gun part
    // holds its vertices here (`vertices` empty, one strip of them), shared across frames while the part's inputs are
    // unchanged; the renderer keeps their host vertices while this pointer and `matrix` are, and draws a run with such
    // a batch with its host depth buffer. Null for every other batch (no mods: never set).
    std::shared_ptr<const std::vector<GuiVertex>> shared_vertices;
    // Port-only (checkpoint 132ws, frame_fit.hpp FrameAspect::wide): the wide draw's hints, unused by every other
    // aspect. port_anchor: non-zero for the batches of one HUD element (the HUD screen root's child + 1, or
    // port_anchor_widget + a game screen widget's index); the wide renderer moves each element to the window's edge
    // its bounds lie towards. port_picture: a movie / loading-screen picture (4:3 with bars, or zoomed: movie_fill).
    std::uint32_t port_anchor = 0;
    bool port_picture = false;
    // Port-only (checkpoint 132md14): a port_picture wider than 4:3 (a mod movie's display aspect, e.g. 16/9; its quad
    // is letterboxed in the 640 x 448 canvas). The wide renderer draws it at that aspect, as wide as the target allows
    // (bars) or across the whole width (movie_fill). 0: every other batch.
    float port_picture_aspect = 0.0F;
    // Port-only (132vt2a2): a port_picture that is a movie (frontend_disc movie_list): the wide renderer fits it by
    // frame_movie_fit_setting (movies=: fill, bars or stretch) instead of movie_fill (the loading screen's).
    bool port_movie = false;
    // Port-only (issues #5 / #25): the authored DEMOnn attract trailers always retain their native 4:3 presentation
    // under wide, while story/intro movies continue to follow frame_movie_fit_setting.
    bool port_movie_native_bars = false;
    // Port-only (checkpoint 132ws2): a HUD batch placed by the 3D view (the callouts / name tags, the lock-on
    // elements): the wide draw maps it over the whole window (Hor+ positions past the 4:3 edges stay visible),
    // with no anchor shift and no 4:3 clip. Unused by every other aspect.
    bool port_free = false;
    // Port-only (checkpoint 132ws5, research/FRONTEND.md "Checkpoint 132ws5"): the wide menu draw's hints
    // (GuiRenderer::set_wide_menu), unused otherwise. port_menu: non-zero for the batches of one element of a shown
    // screen (a child of a canvas child; numbered in draw order per frame by GuiDrawer::draw_canvas); the wide menu
    // draw spreads the header and legend elements towards the window's edges. port_text: a font string (0036a330),
    // never mirrored into the side areas.
    std::uint32_t port_menu = 0;
    bool port_text = false;
    // Port-only (checkpoint 132ws9, research/FRONTEND.md "Checkpoint 132ws9"): in the GUI rows y0..y1 (y up, 224 at
    // the top) this batch's wide edge images stretch the art's rows at y1 (upper half) and y0 (lower half) instead of
    // mirroring the rows between (the loading screen's backdrop: its baked-in ticker line is not mirrored into the
    // side areas). y1 <= y0: off; unused by every other aspect.
    struct PortEdgeBand {
        float y0 = 0.0F, y1 = 0.0F;
    } port_edge_band;
};
inline constexpr std::uint32_t port_anchor_widget = 0x10000;
// Port-only (checkpoint 132fx1): port_anchor_follow | a HUD element's tag: the batch takes that element's wide shift
// (and does not count towards its bounds). The game screen's weapon select (GAMEGUI "weaponselect", 0045ca20) follows
// the HUD's "weaponselectpanel" it extends from, so under the wide aspect the five panels stay beside the weapon
// icon at the right edge instead of keeping their 4:3 place.
inline constexpr std::uint32_t port_anchor_follow = 0x20000;

struct GuiDrawList {
    std::vector<GuiDrawBatch> batches;
    // Port-only (132vf1, research/VITA.md "Front-end speed (132vf1)"): the batches of an earlier frame, kept for their
    // vectors' storage. recycle() moves `batches` here; take() hands one back reset to a default batch (empty vectors
    // that keep their capacity), so a frame's draw makes no new allocations once the storage has grown.
    std::vector<GuiDrawBatch> spare;
    void recycle();
    GuiDrawBatch take();
};

// 0036a330(font, s, rgba, x, y, z, clip_l, clip_r, fade): appends the flush
// batch (none when no vertex was emitted). `s` must hold the terminator.
void gui_font_draw(GuiDrawList& out, const GuiFont& font, std::span<const std::uint8_t> s,
                   const std::array<float, 4>& rgba, float x, float y, float z, float clip_l, float clip_r,
                   float fade);

// The fields 003cfab0 / 003d04c0 read from a text object (003d0940 layout).
struct GuiTextDraw {
    std::int32_t x = 0, y = 0, w = 0, h = 0;  // +0..+c
    std::uint8_t b10 = 0;  // alignment: 0 left, 1 right, 2 centre, 3 scroll
    std::uint8_t b11 = 0;  // lines go up (bottom aligned)
    std::uint8_t b12 = 0;  // vertically centred
    std::uint8_t b58 = 0;  // single run
    std::uint8_t b5b = 0;  // ellipsis per line
    float f40 = 0.0F;      // scroll offset (mode 3)
    float f54 = 0.6F;      // character-fill threshold (003d0940: 0x3f19999a)
    std::array<float, 4> colour{};  // +30..+3c
    const GuiFont* font = nullptr;  // +20
    std::string text;               // +14
};

// A 0036a330 call 003cfab0 makes.
struct GuiFontCall {
    std::string text;
    std::array<float, 4> rgba{};
    float x = 0, y = 0, z = 0, clip_l = 0, clip_r = 0, fade = 0;
    std::size_t batch_end = 0;  // the output list's batch count after the call
};

// 003d04c0(obj): the line count.
std::int32_t gui_text_line_count(const GuiTextDraw& t);
// 003cfab0(obj) after its clock / fade / scroll state part (ported as
// ie_text_draw_state, Checkpoint 119h). A layout that cannot advance (the
// game loops forever) or would overrun the 0x100-byte line buffer throws.
void gui_text_draw(GuiDrawList& out, const GuiTextDraw& t, std::vector<GuiFontCall>* calls = nullptr);
// 003cf3a0(obj, &x, &y, index): the draw position of the text's character
// `index` (003cfab0's layout up to the line that holds it, then the width of
// the line's first characters); false where it returns 0 (no font, scroll
// mode 3, an index past the text). An ellipsis-per-line object (+5b) throws.
bool gui_text_cursor(const GuiTextDraw& t, std::int32_t index, float& x, float& y);

// 0041c150 between the base draw and 003cfab0: the object's rect (+0..+c)
// and, when +59 (fade) is clear, colour (+30..+3c, 003d0810) from the
// widget's flags (+14), rect (+34..+40) and colour (+60..+6c).
void gui_text_widget_object(std::uint16_t flags, const std::array<float, 4>& rect, const std::array<float, 4>& rgba,
                            bool fading, std::array<std::int32_t, 4>& obj_rect, std::array<float, 4>& obj_colour);

// The 003cfab0 fields of an interface-element text object (the HUD's
// ticker / notification texts, IeTextObject) after its state part has run
// (ie_text_draw_state, Checkpoint 119h). The bytes the model does not track
// (+11, +12, +5b) hold the 003d0940 constructor's zero. Only objects with a
// text (+14 set by 003d08b0) are meaningful; an unset text maps to "".
GuiTextDraw gui_text_from_ie(const IeTextObject& obj, const GuiFont* font);

// GuiWeaponDisplay's draw 00440580 (checkpoint 132f3b) after its children
// (0041e470), for a drawn model (+4c set, model +417 clear, +54 != 1.0 and
// bounds min.x <= max.x): the stack top it pushes (`top`, built from the
// current top, the widget rect +34..+40 and the model bounds +3e0 / +3f0)
// and the matrix a2 it passes to 003836b0(model, 2, a2, 1) (the turn +60
// about the bounds centre). Each step is the VU product (second x first)
// of the original, flushed; the 0037a1e0 GS packet before it is not
// modelled.
struct WeaponDisplayMatrices {
    std::array<float, 16> top{};
    std::array<float, 16> a2{};
    std::array<float, 16> model{};  // 003836b0's 003837a8 product: top = (top, a2) (the parts' matrix)
    std::uint8_t alpha_416 = 0;  // model +416: 0x80 when +54 is 0, else cvt.w(64 * (1 - +54))
};
WeaponDisplayMatrices weapon_display_matrices(const std::array<float, 16>& top, const std::array<float, 4>& rect,
                                              const std::array<float, 4>& lo, const std::array<float, 4>& hi,
                                              const std::array<float, 16>& rot, float fade);
// GuiEquipmentModify's weapon box 002bf4b0(screen, model, widget): the same fit as 00440580 without its
// z offset (the rect corners sit at the top's row3.z) and without a2 (003836b0 gets the identity
// 0055c9f0): the pushed top = (axes, row3 + rect centre - bounds centre * s) times diag(s, s, s, 1),
// s = min(w / extent.x, h / extent.y). The caller gates on +417 and min.x <= max.x.
std::array<float, 16> equipment_box_matrix(const std::array<float, 16>& top, const std::array<float, 4>& rect,
                                           const std::array<float, 4>& lo, const std::array<float, 4>& hi);
// Checkpoint 132f6: the loadout figure's camera, 002bfa90's camera record 0 (005381f0 + 0x1f0): near 1,
// far 40 (+fc), half fov 0.5236 (+100), perspective (+104 = 0), the 640 x 480 viewport, and the camera
// world at sp+380 = (0, 5, 1.5) turned by the screen's +70 Euler angles (0, 0, pi), inverted (00133300 /
// 00133200) and pushed as the matrix stack top. The returned columns map a world point p to clip space
// c = sum col[k] * p[k] = P x A x view: view = Rz(pi)^T (p - (0, 5, 1.5)), A the record's y / z swap
// (+e80), P = (1 / tan(fov), (4 / 3) / tan(fov), (far + near) / (far - near), -2 far near / (far - near))
// in 003754a0's GL form (the dumped record's +50: 1.73205, 2.3094, 1.05128, -2.05128).
std::array<std::array<float, 4>, 4> eqmodify_figure_clip_2bfa90();

// The widget-tree walk 00415bb0 performs over the native GUI runtime
// (read-only). Fonts are the runtime host's handles (GuiHost::font); the
// per-object clock / scroll state (003cfab0's state part: +40..+5a) lives
// here, keyed by the widget, and is reset as 003d08b0 does when the
// object's text changes.
class GuiDrawer {
public:
    using FontLookup = std::function<const GuiFont*(std::uint32_t handle)>;
    explicit GuiDrawer(FontLookup fonts) : fonts_(std::move(fonts)) {}

    // Checkpoint 122b: the decorator elements (00417340's IE part) and the
    // renderer state V (005381f0) they draw with; null: no element drawn.
    void set_decor(GuiDecor* decor, IeDrawState* state) {
        decor_ = decor;
        st_ = state;
    }

    // 00415bb0(canvas) at draw clock now_ms (ie_text_clock_ms).
    void draw_canvas(GuiDrawList& out, const gui::GuiRuntime& gui, std::int32_t now_ms);
    // The +44 draw of one widget (00417340 / its class's +30).
    void draw_widget(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    // GuiWeaponDisplay's model part of 00440580 (the host owns the models:
    // its bounds, +417 / +416 and 003836b0); unset, a display with a model
    // throws naming 00440580.
    std::function<void(GuiDrawList& out, IeDrawState* st, const gui::Widget& w)> weapon_display_draw;
    // Checkpoint 132hud: GuiMapScreen's draw 00464a90 between the "backdark" decorator and 0041e470: the HUD
    // map 0026ca70(+4c) (hud_map.hpp). Unset, a map screen throws naming 0026ca70.
    std::function<void(GuiDrawList& out, IeDrawState* st, const gui::Widget& screen)> map_screen_draw;
    // GuiCharacterDisplay's +48 callback (display.cd_callback): 0035df30 -> 0035b390(*00533760), the
    // GuiScoresEquip screen's four skinned display actors (camera half-fov 0.5236, near 1, far 40;
    // 00373670's light table); or GuiEquipmentModify's 002c2340 (the figure 002bfa90) and 002c2290 /
    // 002c21c0 (the weapon boxes 002bf4b0). Unset, a display with its callback installed throws naming
    // 0035b390.
    std::function<void(GuiDrawList& out, IeDrawState* st, const gui::Widget& screen, const gui::Widget& display)>
        character_display_draw;
    // Checkpoint 132f14: GuiAgentDisplay's draw 00361e70 after 0041e470: its actor (the agent's +a0
    // packing) through the +50 camera turned by +80..+88 (agent_camera_view). Unset, it throws naming 00361e70.
    std::function<void(GuiDrawList& out, IeDrawState* st, const gui::Widget& display)> agent_display_draw;
    // Checkpoint 132ws6: GuiEquipmentModify's 002c0380 calls 00373670(00535d20) before 0041e470: the level's light
    // table rebuilt in the GUI camera (the host's; level_lights.hpp loadout_light_camera). Unset: not rebuilt.
    std::function<void()> light_table_rebuild;
    // The game screen (GuiGameScreen, vtable 004e1620, HUD +6a4: GAMEGUI's root as 004187c0 makes it) whose
    // +30 is 0045d8d0 instead of 0041e470. Null: no widget takes that draw.
    const gui::Widget* game_screen = nullptr;
    // Port-only (checkpoint 132fx1, wide aspect only; 0 otherwise): the anchor tag (port_anchor_follow | the HUD
    // "weaponselectpanel" tag) given to the weapon select's batches (its widget and 0045c0f0's round counts).
    std::uint32_t port_weapon_select_anchor = 0;

private:
    struct TextState {
        std::string text;
        float f40 = 0.0F, f44 = 0.0F;
        std::uint8_t b5a = 0;
    };
    void draw_text_widget(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    void draw_edit_cursor(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w);  // 0041a450
    void draw_text_list(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    void draw_ml_text(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    // 0034d8c0 (GuiRatings): the children, then each grid row's first unearned rating's percentage
    // through the +dc text object at that bitmap's rect, under a pushed identity top (132f13).
    void draw_ratings(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    // 0045c0f0: the weapon select's round counts (checkpoint 132hud).
    void draw_game_screen_counts(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w,
                                 std::int32_t now_ms);
    void draw_subtitle(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    void draw_bitmap(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    void draw_text_array(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    // Checkpoint 132ln4: 00423670 as GuiMissionList's +30, with its row draw +64 002ab2a0 (the selection bar of the
    // focused list's row) and cell draw +68 002aad40 (the lobby record's column text).
    void draw_mission_list(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w, std::int32_t now_ms);
    void draw_net_message_array(GuiDrawList& out, const gui::GuiRuntime& gui, const gui::Widget& w,
                                std::int32_t now_ms);
    void draw_selection_bar(GuiDrawList& out, const gui::Widget& w, float y, float row_w, float row_h,
                            std::uint32_t source);
    // font: the font to draw with instead of the object's handle (0043d8b0's
    // colour-0 swap).
    void draw_object(GuiDrawList& out, const gui::Widget* key, const gui::TextObject& o,
                     const std::array<std::int32_t, 4>& rect, const std::array<float, 4>& colour, std::int32_t now_ms,
                     const GuiFont* font = nullptr);
    FontLookup fonts_;
    // GuiMLTextWidget +50 and its object's colour as GuiBriefingPane's draw
    // 00455db0 sets them (0043e1a0) on its "value_briefing".
    std::map<const gui::Widget*, std::array<float, 4>> ml_colour_;
    GuiDecor* decor_ = nullptr;
    IeDrawState* st_ = nullptr;
    std::map<const gui::Widget*, TextState> text_;
    // Port-only (132ws5): draw_widget's depth below the canvas and the last screen element tag given
    // (GuiDrawBatch::port_menu; never reset, so the tags of one list stay distinct).
    int menu_depth_ = 0;
    std::uint32_t menu_tag_ = 0;
};

}  // namespace omega
