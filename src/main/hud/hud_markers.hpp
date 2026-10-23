#pragma once
// The radar blips of the HUD target markers (hud.cc / hudTargets.cc): the
// sprite +4 every marker of the targets list (hud_targets.hpp) owns, built by
// 00274750 over the radar's triangle / square nodes, coloured by the targets
// update 00274260, killed by 003e9770, stepped by the sprite step 003e9880
// (vtable 004dc810 +48) and placed by 00261360 (vtable +110, through the
// render pass's +58 = 002610f0) in the frame 00261130 builds at the start of
// the HUD draw 00244b90. The map sprite +8 (Checkpoint 132hud2) is
// HudMapSprites on the map's IAMAP screen (*(map + 8), 0026e070).
#include "hud_radar.hpp"
#include "hud_targets.hpp"
#include "ie_screen.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace omega {

using HudMatrix = std::array<std::array<float, 4>, 4>;

// The marker colours (hud.cc globals, nothing writes them outside the
// script setters SetRadar*Col, which no disc script calls).
inline constexpr std::array<float, 4> hud_marker_friendly_4ffdb0{0.5F, 1.0F, 0.5F, 1.0F};
inline constexpr std::array<float, 4> hud_marker_player_4ffdc0{0.5F, 0.5F, 1.0F, 1.0F};
inline constexpr std::array<float, 4> hud_marker_enemy_4ffdd0{1.0F, 1.0F, 0.0F, 1.0F};
inline constexpr std::array<float, 4> hud_marker_blocked_4ffde0{0.7F, 0.7F, 0.3F, 1.0F};  // 3f333333, 3e99999a
inline constexpr std::array<float, 4> hud_marker_threat_4ffdf0{1.0F, 0.0F, 0.0F, 1.0F};

// The colour 00274260 passes to 003e7ee0 for a marker: player 004ffdc0,
// friendly 004ffdb0, blocked 004ffde0; threat (002745e8..00274698): f =
// progress; unless f x DangerMag <= 0.98 (0x3f7ae148, c.ole.s), f = f x t30
// (the targets' +30, f21, before this update's meter); g = 1 - f; c_k =
// ftz(ftz(enemy_k g) + (float)(threat_k f)) (mula / madd).
std::array<float, 4> hud_marker_colour(HudMarkerColour colour, float progress, float targets_30, float danger_mag);

// What 00261130's non-identity path reads: the camera 00159560(0x4ffc14)'s
// virtual +d8 (a 4 x 4) and the object's virtual +f4 (a 4 x 4).
struct HudMarkerView {
    HudMatrix camera_d8{};
    HudMatrix object_f4{};
};

// 00261130: the radar frame M (*004f82b0, 4 rows). The object 0040c540(&
// *004f82a0) with a controller (+58) whose virtual +54 is true supplies
// view; otherwise (nullopt) M is the identity: row 3 = vf0, rows 1 / 2 / 0
// by pcpyud / pexcw from it. *004f82a0 is the player's ID: the role-1 load
// 00131400 tail-calls 002426d0, which sets *004f7db8 and jumps to its setter
// 00261350; the player controller's +54 (00175330) is 1. With a view:
// r0 = camera row 0, r1 = camera row 1, both with z = 0 and normalised
// (vmul.xyz, the 1 * x^2 + 1 * y^2 + 1 * z^2 chain, vrsqrt, vmulq.xyz),
// c = (0, 0, 1, 0); 00131f00 makes rows (r0, r1, c, object row 3); 0013ee20
// transposes the 3 x 3; unless row 3's xyz are all zero (0013edb0, c.eq.s),
// t = -row3 (0013ed90 vsub.xyz) and row3.xyz = M0 t.x + M1 t.y + M2 t.z
// (0013ed60).
HudMatrix hud_marker_matrix(const std::optional<HudMarkerView>& view);

// The engine variables 00261360 reads: game::RadarPixWid (the int
// *0048b0c0), game::RadarMaxDist (*0048b0c8), game::RadarMaxScale
// (*0048b0d0), at their boot values.
struct HudBlipVars {
    std::int32_t pix_wid = 55;
    float max_dist = hud_radar_max_dist_boot;
    float max_scale = 4.0F;
};

// What 00261360 reads from the marker's object (0040c540(+68)): its virtual
// +e4 (the position) and row 1 of its virtual +f4; the map sprite's place
// 003e9550 reads the whole +f4 (rows).
struct HudBlipObject {
    std::array<float, 4> position_e4{};
    std::array<float, 4> f4_row1{};
    HudMatrix f4{};
};

// 00261360's results: +70 (003e8180 copies it into the element's +20) and
// the rotation 003e8070 gets.
struct HudBlipPlacement {
    std::array<float, 4> words_70{};
    std::array<float, 3> rotation{};
};

// 00261360 past its null checks: p = M0 x + M1 y + M2 z + M3 w (vmulax ..
// vmaddw); r = sqrt.s(ftz(px^2) + ftz(py^2)); r <= MaxDist (c.ole.s): s =
// (float)PixWid x MaxScale / (MaxDist + r (MaxScale - 1)), else s =
// (float)PixWid / r; +70 = (px s, 0, py s, 1); v = M . (f4 row 1) (vmulax /
// vmadday / vmaddz on M0..M2), a = atan2f(-v.x, v.y) (00121ab8), rotation
// (0, -a, 0). A zero divisor throws.
HudBlipPlacement hud_blip_place(const HudMatrix& m, const HudBlipObject& object, const HudBlipVars& vars);

// The blip sprite (0x90 bytes, vtable 004dc810; 003e99b0 initialises it).
struct HudBlip {
    std::uint32_t serial = 0;        // the marker that built it (HudTargetMarker::serial)
    std::int32_t marker_68 = 0;      // +68: the marker's object ID
    int element_60 = -1;             // +60
    std::uint8_t follow_64 = 1;      // +64
    std::uint8_t ending_84 = 0;      // +84 (003e9770)
    bool registered = true;          // in the traversal tree (40c9d0; 40c9a0 takes it out)
    bool released = false;           // 3cb230's deferred destructor ran
    std::array<float, 4> words_70{}; // +70 (the last placement)
    bool operator==(const HudBlip&) const = default;
};

// The blips in creation order (their IDs 0x00f3xxxx from 0040c240 sort after
// every level object, so the object loop steps them in that order after the
// player).
class HudBlips {
public:
    // One sprite call of the targets (HudSpriteEvent), on the HUD screen:
    //   build (00274750 at 00274890..00274968): node = state 1 or 2 ?
    //     *004ffd60 : enemy ? *004ffd50 : *004ffd58; 003ef010 makes the
    //     element (the node's colour); 003e99b0: +60 element, +64 1, +68 the
    //     marker's ID, +84 0, registered; a null node throws naming 003ef010;
    //   kill (003e9770): 003f44e0(+60, 0) and +84 = 1;
    //   colour (003e7ee0 on +60): hud_marker_colour, each byte ie_colour_byte.
    void apply(IeScreen& screen, const HudRadar& radar, const HudSpriteEvent& event, float targets_30,
               float danger_mag);
    // 003e9880 on every registered blip: with +64, +64 = 0 when 0040c540(+68)
    // finds nothing (the found object's virtual +c0 write to +70 is
    // overwritten by every placement); then with +84 and the element's time
    // below 0.5 (c.olt.s): 40c9a0 (unregistered), 3cb230(blip, 0.1) and +84 =
    // 0. Returns the blips unregistered, whose release the host schedules.
    std::vector<std::size_t> step(const IeScreen& screen, const std::function<bool(std::int32_t)>& found);
    // The deferred release (3cb4f0 -> the destructor 00261090 -> 003e9940):
    // 003e8eb0 on the element.
    void release(IeScreen& screen, std::size_t blip);
    // The render pass's +58 (002610f0) on every registered blip with +64:
    // 00261360, which returns when the element is null or the object is not
    // found (nullopt), then writes +70, the element's +20 and its rotation.
    void place(IeScreen& screen, const HudMatrix& m,
               const std::function<std::optional<HudBlipObject>(std::int32_t)>& object, const HudBlipVars& vars);

    const std::vector<HudBlip>& blips() const { return blips_; }
    // 003e7ee0 on the marker's +4 element with a constant colour (the
    // highlight 00272ad0's 004ffd90 / 004ffda0, the objective records'
    // 004ffda0 after 00273570 / 00273210's build); each byte ie_colour_byte.
    void set_colour(IeScreen& screen, std::uint32_t serial, const std::array<float, 4>& rgba);
    // The blip a marker currently owns (+4), or nullopt.
    std::optional<std::size_t> current(std::uint32_t serial) const;
    std::size_t live() const;

private:
    std::vector<HudBlip> blips_;
    std::map<std::uint32_t, std::size_t> current_;
};

// The map's colours (hudTargets.cc's map sprite, hud.cc's highlight; the
// booted EE snapshot): 004a8f50 an enemy marker, 004a8f40 any other,
// 004ffd90 the highlighted record's radar blip, 004ffda0 (1, 1, 1, 1) the
// objective records' and the unhighlighted record's, 004ff0b0 (1, 1, 1, 1)
// the player arrow's.
inline constexpr std::array<float, 4> hud_map_enemy_4a8f50{1.0F, 0.3F, 0.3F, 1.0F};   // 3e99999a
inline constexpr std::array<float, 4> hud_map_other_4a8f40{0.3F, 0.3F, 1.0F, 1.0F};
inline const std::array<float, 4> hud_marker_selected_4ffd90{
    std::bit_cast<float>(0x3f31a9fcU), std::bit_cast<float>(0x3eb0a3d7U), std::bit_cast<float>(0x3dd2f1aaU), 1.0F};
inline constexpr std::array<float, 4> hud_marker_white_4ffda0{1.0F, 1.0F, 1.0F, 1.0F};
inline constexpr std::array<float, 4> hud_map_player_4ff0b0{1.0F, 1.0F, 1.0F, 1.0F};

// The IAMAP nodes the map constructor 0026e070 finds under its root (each
// set_flag_61 1): *004ffd70 / *004ffd68 (both "radartrianglelong"),
// *004ffd78 "radarobjectivebig", *004ffd80 "radarobjectivesel".
struct HudMapNodes {
    int triangle_70 = -1;
    int triangle_68 = -1;
    int big_78 = -1;
    int sel_80 = -1;
};
// 0026e070's lookups (find_child under the root) and the +61 flags (the
// 003ee5e0 white colours of 78 / 80 change nothing); throws naming 0026e070
// when a node is missing.
HudMapNodes hud_map_nodes(IeScreen& iamap);

// 003e9550 (the map sprite's place, vtable 004e0360 +110): M = the object's
// virtual +f4, R = 00140c50's rotation about (1, 0, 0) by -pi/2 (0xbfc90fdb),
// P = M R (0013ea40, ni_matrix_product), the 3 x 3 entries scaled by
// *00493980 (1.0, vmul), then 003e8040 copies P into the element's +30.
std::array<float, 16> hud_map_sprite_matrix(const HudMatrix& f4);

// The map sprites (+8 of each marker record, and the map's own +c player
// arrow) on the IAMAP screen, 0x90 bytes (vtable 004e0360, 003e99b0): the
// same HudBlip state as the radar's, stepped by 003e9880 and released by
// 3cb230's 0.1 s deferred delete; placed by 003e9550 instead of 00261360.
class HudMapSprites {
public:
    // One sprite call of the targets on the map screen (00274750 at
    // 00274750..00274884): build: node = state 1 or 2 ? *004ffd78 : enemy ?
    // *004ffd68 : *004ffd70, the element's colour 004a8f50 / 004a8f40;
    // kill 003e9770; colour 00274260's (hud_marker_colour).
    void apply(IeScreen& screen, const HudMapNodes& nodes, const HudSpriteEvent& event, float targets_30,
               float danger_mag);
    // 003e7ee0 on the record's +8 element.
    void set_colour(IeScreen& screen, std::uint32_t serial, const std::array<float, 4>& rgba);
    // 00272ad0's map side for a record (serial, object ID): 003e9770 on its
    // +8, then a new sprite (003ef010 on the node, 003e99b0 with +68 = the
    // record's ID) stored at +8, coloured by nothing (the node's colour).
    void replace(IeScreen& screen, int node, std::uint32_t serial, std::int32_t id);
    // 0026dbc0's player arrow (map + c): 003ef010(*004ffd70) and
    // 003e7ee0(004ff0b0) always; with the player *004ffc0c and its +30, a
    // sprite (003e99b0, +68 = the player's ID) stored at map + c.
    void open_player(IeScreen& screen, const HudMapNodes& nodes, std::optional<std::int32_t> player_id);
    // 0026d720: with map + c, 003e9770 on it and +c = 0.
    void close_player(IeScreen& screen);
    std::vector<std::size_t> step(const IeScreen& screen, const std::function<bool(std::int32_t)>& found);
    void release(IeScreen& screen, std::size_t sprite);
    // The render pass's +58 on every registered sprite with +64: 003e9550.
    void place(IeScreen& screen, const std::function<std::optional<HudMatrix>(std::int32_t)>& object_f4);

    const std::vector<HudBlip>& sprites() const { return sprites_; }
    std::optional<std::size_t> current(std::uint32_t serial) const;
    std::optional<std::size_t> player() const { return player_c_; }
    std::size_t live() const;

private:
    void kill(IeScreen& screen, std::size_t sprite);
    std::size_t make(IeScreen& screen, int node, std::uint32_t serial, std::int32_t id, const char* address);
    std::vector<HudBlip> sprites_;
    std::map<std::uint32_t, std::size_t> current_;
    std::optional<std::size_t> player_c_;
};

} // namespace omega
