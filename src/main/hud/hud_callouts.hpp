#pragma once
// HUD callouts (Checkpoint 130e): the labels the scripts put on objects and
// NPCs (game::SetCallout / SetCalloutLabel / ClearCallout). 00242fa0 makes a
// record in the std::list at hud +6e8 with an hscallout element and a callout
// tracker (vtable 004e05a0); the HUD update 00242330 shows or hides the
// element and runs its timer; the tracker's draw 003e97d0 projects the
// object's +c0 point (00374bc0) into the element's +20, which the hscallout
// node (+62) draws at that screen point. Every float operation follows the
// original instruction order (IEEE binary32, results flushed to zero as the
// EE does).
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace omega {

// A callout record: 0036b180(0, 0x1c, "hud.cc", 0x505) in 00242fa0.
struct HudCalloutRecord {
    std::int32_t key_0 = 0;       // +0: the object id (0016f7e0 copies *key)
    float timer_4 = 0.0F;         // +4: seconds left; <= 0 keeps the callout until it is cleared
    std::int32_t slot_8 = 0;      // +8: a3 (the tracker's +80, the +c0 subtarget)
    int element_c = -1;           // +c: the hscallout element (an IE screen index)
    int tracker_10 = -1;          // +10: the callout tracker (an index into HudObject::trackers())
    float dist_14 = 0.0F;         // +14: the squared distance to the player (00242330)
    std::uint8_t bottom_18 = 0;   // +18: t1 (an hscallout_bottom callout, drawn by 002434f0)
};

// The render camera record 00374bc0 reads (0x5381f0 + 0x1f0 + 0x110 *
// *00538c60): +0 / +4 the viewport origin, +8 / +c its width and height, +f4
// the near plane, +fc the far plane and +50 the projection (four columns).
// The viewport is the renderer's V+f80 = (0, 0, 640, 480) (the virtual
// screen 0037f740 picks for both NTSC frame buffers): the app copies it into
// the record (00165f1c..00165f58) and the gameplay frame setup 00132e60
// copies the previous record's +0..+c into the record it pushes
// (00132f44..00132fe4).
struct HudCalloutCamera {
    std::array<float, 4> viewport{};
    float near_f4 = 0.0F;
    float far_fc = 0.0F;
    std::array<std::array<float, 4>, 4> projection_50{};
};

// 00374bc0(out, camera, p): hx = +8 / 2, hy = +c / 2, hz = 0.5 (+fc - +f4);
// ox = 0.5 (-+8) + (+0 + 0.5 +8); oy = (+4 + 0.5 +c) + 0.5 (-+c) (adda.s,
// madd.s); oz = hz + +f4; v = (+50 x renderer +e80, the Y / Z swap) p
// (vmula / vmadd); with inv = 1 / v.w: (v.x (inv hx) + ox, v.y (inv hy) + oy,
// v.z (inv hz) + oz, 1.0).
std::array<float, 4> hud_project_374bc0(const HudCalloutCamera& camera, const std::array<float, 4>& p);

// Checkpoint 132map1: what the map's player callout tracker's draw 003e9460
// (vtable 004e0240 +110) reads: the map's view 0055a290 (rows; 0026d040
// writes it at 0x0026d574.., hud_map_ie_matrix) and the current camera
// record (*00538c60).
struct HudMapCalloutCamera {
    std::array<std::array<float, 4>, 4> view_55a290{};
    HudCalloutCamera record;
};
// 003e9460's point: v = p x 0055a290 (vmulax / vmadday / vmaddaz / vmaddw on
// its rows), out = 00374bc0(record, v), and out.z negated (neg.s) when it is
// below 0.0 (c.olt.s, after the projection). EE goldens:
// tests/hud_map_callout_goldens.hpp.
std::array<float, 4> hud_map_callout_place(const HudMapCalloutCamera& camera, const std::array<float, 4>& p);
// The camera record the HUD map draw 0026ca70 leaves current for the HUD
// draw's map path (0x0026cdb4..0x0026ce14 and its tail 0x0026cfd8..): the
// viewport +0..+c unchanged, +100 the zoom 0048b2b0, +f4 = 1.25, +fc the far
// plane 0048b290 and +50 the projection the IAMAP draw's recompute (003754a0,
// hud_map_projection_003754a0) leaves; the tail restores only +f0 / +f8 and
// sets +2f4 / +2f6, which 00374bc0 does not read.
HudCalloutCamera hud_map_callout_record(float zoom_48b2b0, float far_48b290, const std::array<float, 4>& viewport);

// What 00242330 reads of the object 0040c540(&key) finds: +2c (enabled), +2d
// (present), +2e (removed) and, once those pass, its virtual +e4.
struct HudCalloutObject {
    std::array<std::uint8_t, 3> root{};
    std::optional<std::array<float, 4>> position_e4;
    // Virtual +108, the life state (an actor's 001872f0, a GOB's / hotbox's
    // 00141850): read (002424b4) only for an hscallout_bottom record (+18).
    std::optional<std::uint8_t> life_108;
};
using HudCalloutLookup = std::function<std::optional<HudCalloutObject>(std::int32_t id)>;

// One HUD update's inputs to 00242330.
struct HudCalloutFrame {
    float dt = 0.0F;
    bool paused_4ffbb7 = false;   // byte 004ffbb7
    // 00241ac0: the app's message box (*004ffc04 +258, its +14 & 8) or the GUI
    // manager's +20 (+14 & 0x80) is up. With it, the GUI handle +6a4's +d4
    // (gs_d4; Checkpoint 132hud4) other than 1 hides the callout (00242398).
    bool gui_241ac0 = false;
    std::int32_t gs_d4 = 0;
    // *004ffc0c's +30 virtual +e4 (the player); nullopt when either is null.
    std::optional<std::array<float, 4>> player_e4;
    HudCalloutLookup lookup;      // 0040c540
};

// One record's pass of 00242330: the 003e8de0(+c, flag) calls in order
// (shows of them) and whether its timer ran out (00242b40(hud, rec, +8, 0,
// 0) after the iterator was advanced). element_hscallout is +c's node +70 ==
// *004f7d90; byte_6a9 / map_6a0 the HUD object's +6a9 / +6a0.
struct HudCalloutStep {
    std::array<std::uint8_t, 3> show{};
    int shows = 0;
    bool clear = false;
};
HudCalloutStep hud_callout_step(HudCalloutRecord& rec, const HudCalloutFrame& frame, bool byte_6a9, bool map_6a0,
                                bool element_hscallout);

// The camera 002434f0 reads (Checkpoint 132hud4): *(*004ffc1c + 8) once
// *004ffc14 holds one, its +50 node's +10..+40 (rows, after the node's
// virtual +58 when its byte +50 is set; row 3 the position) and +60 the
// half field of view.
struct HudCalloutEdgeCamera {
    std::array<std::array<float, 4>, 4> node_10{};
    float half_fov_60 = 0.0F;
};

// 002434f0's off-screen placement (0x243668..0x243cb4) of a callout whose
// projected point p (tracker +70) is behind the camera or outside the video
// rectangle: with t = target (the object's virtual +c0) and the camera
// position pos = node row 3, f23 = (w / 2) / tanf(half fov), f22 =
// atanf((h / 2) / f23); the heading a = (0, 1, 0, 0) M and d = t - pos with
// z = 0, both normalised (vrsqrt), give x from acosf(d . a) (inside the
// half fov f23 tanf(angle), else the edge, signed by (a x d) . z); M turned
// about z by that signed angle (00132130) gives u = its row 1 and y from
// acosf(e . u) with e = t - pos (inside f22 f23 tanf(angle), else the edge,
// signed by (u x e) . row 0). Returns p with x / y replaced. acosf outside
// [-1, 1] is the kernel's (x - x) / (x - x) (0011d034..0011d040): +FLT_MAX.
std::array<float, 4> hud_callout_offscreen(const HudCalloutEdgeCamera& camera, const std::array<float, 4>& target,
                                           std::array<float, 4> p, float w, float h);

// One record's placement in 002434f0 (0x2435e0..0x243ec8): p the tracker's
// +70, rect hud +40..+4c, text (asked after the placement; when the
// tracker +60 element's text has a font +20) the node TEXT frame's +4 and the line count x font height
// (003d04c0 x 0036a2f0, mult); target is 0040c540 + the object's virtual
// +c0 (nullopt: not found), asked only off screen. placed false: the record
// is skipped (not found, or clamped beyond 3600.0); otherwise p (+78 =
// 10.0) and the edge s2 (the caller swaps the element when s2 is not the
// index of its node among *004f7da0[0..3]).
struct HudCalloutEdgeInput {
    std::array<float, 4> p{};
    std::array<float, 4> rect{};
    std::function<std::optional<std::pair<float, std::int32_t>>()> text;
    float dist_14 = 0.0F;
    std::int32_t video_w = 640, video_h = 448;
};
struct HudCalloutEdgeResult {
    bool placed = false;
    std::array<float, 4> p{};
    int s2 = 3;
};
HudCalloutEdgeResult hud_callout_edge_step(const HudCalloutEdgeInput& in,
                                           const std::optional<HudCalloutEdgeCamera>& camera,
                                           const std::function<std::optional<std::array<float, 4>>()>& target);

// The colours 00242fa0 hands 003e7ee0 / 003d0810 (the boot snapshot's
// 00587d90 / 00587da0 / 00587db0): the element colour of a plain callout
// (143, 187, 218) / 255, the text colour of a lock-ramp callout (yellow) and
// of a negative-colour callout (white).
inline constexpr std::array<std::uint32_t, 4> hud_callout_colour_587d90{0x3f0f8f90U, 0x3f3bbbbcU, 0x3f5adadbU, 0x3f800000U};
inline constexpr std::array<std::uint32_t, 4> hud_callout_colour_587da0{0x3f800000U, 0x3f800000U, 0x00000000U, 0x3f800000U};
inline constexpr std::array<std::uint32_t, 4> hud_callout_colour_587db0{0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U};
// 00242330 hides a callout whose squared distance to the player is above
// 8100.0 (0x45fd2000: 90 units).
inline constexpr float hud_callout_max_dist_sq = 8100.0F;
// 002434f0 keeps a clamped hscallout_bottom callout only within 3600.0
// (0x45610000: 60 units) of the player (+14), and its bottom edge is
// 112.0 (0x42e00000) above the video rectangle's.
inline constexpr float hud_callout_edge_dist_sq = 3600.0F;
inline constexpr float hud_callout_edge_bottom = 112.0F;

// 002c9270's appends into the 0x7f-byte buffer 0026d940 hands 002c9170 (byte
// +0 the count, the text from +1): nothing once the count is 0x7e or more;
// else the bytes up to the NUL, and when the count reaches 0x7e a '\n' (the
// count goes past the cap) and the NUL (002c9310).
void hud_callout_name_append(std::string& buf, std::string_view s);
// 002c9270(buf, player, team, flag) after 0026d940's count byte 0: with the
// player's agent record (00185c70) and its +b70, the 00336170 name; then
// sprintf("%c[%c%s%c]", flag ? '\n' : ' ', 0x11, 0012f5d0(004ffb50, team),
// 0x10): the name slot (the "PlayerName<i>" GetString 00135148 strncpy'd
// (0x0f) into 004ffb50 + f0 + 16 team at boot) between the colour control
// bytes 0x11 / 0x10.
std::string hud_player_callout_name(const std::optional<std::string>& agent_name, std::string_view slot_name,
                                    bool newline);

} // namespace omega
