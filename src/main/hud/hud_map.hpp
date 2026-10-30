#pragma once
// Checkpoint 132hud: the HUD's 3D map (HUD +6a0, map.cc; OBJECTIVE_MARKERS.md "The HUD map"). The map object
// is built by 0026e070 (its tail loads GAMEDATA\<level>\MAPTEX.HOG and MAPVUM.HOG: one cVUM_GOBJ 00395e40 per
// sub-HOG, their vt +88 bounds 003947a0 unioned) and shown by the screen GuiMapScreen (vtable 004e18c0, ctor
// 00464bf0, MAPSCRN.GUI) that the toggle 002427a0 pushes from the pad's D-pad Left edge (0x0024ec1c). Its
// state is a few globals (the camera distance 004ff084, the pitch 004ff070, the zoom 0048b2b0, ...) and the
// object's +20 view, +60 / +64 / +70 / +80 / +90 the centred object and its blend, +94 the L1 / R1 hold.
// Row-vector matrices throughout (a point p maps to p x M), as the VU products do.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace omega {

using HudMapMatrix = std::array<RootVector, 4>;

// The command centre's level order (002cc820: app +24c); -1 when the level is not in it.
std::int32_t hud_map_level_index(std::string_view level);
// 004acce0[index] (18 floats, TORONTO1 1570, TORONTO2 463, ...); throws outside 0..17.
float hud_map_level_extent(std::int32_t index);

// One map model's vt +88 bounds (003947a0: min at +0, max at +10).
struct HudMapBounds {
    RootVector minimum{}, maximum{};
};

// The map's globals (0026e070's tail, 0x0026e584..0x0026e8e0) and the reset 0026c130's.
struct HudMapGlobals {
    float scale_48b278 = 0.25F;      // 128 / 004acce0[app +24c] (0.25 without a positive entry)
    float inverse_scale_48b280 = 4.0F;
    bool scaled_4ff058 = false;      // 1 when app +24c is not -1
    float extent_48b288 = 0.0F;      // |max - min| of the unioned bounds
    float distance_4ff084 = 0.0F;    // -(extent / sin(pi / 4)): the camera's y offset (+ff080 / +ff088 stay 0)
    float zoom_min_48b2a0 = 0.0F;    // min(pi / 4, atan(150 s / |ff084|))
    float zoom_default_48b2a8 = 0.0F;
    float far_48b290 = 0.0F;         // 1 + extent + |ff084|
    float near_48b298 = 2.0F;        // max(2, |ff084| - (1 + extent))
    float pitch_4ff070 = 0.35F;      // 0026c130
    std::array<float, 3> words_4ff074{};  // 004ff074..004ff07c: 0026c130's zeroes
    float zoom_48b2b0 = 0.0F;        // the camera record's half fov
    bool sound_latch_4ff060 = false; // GS_3D_MAP_TURN playing (0026be80)
};

struct HudMap {
    HudMapGlobals g;
    HudMapMatrix view_20{};          // the camera record's view (pushed by 0026ca70)
    std::int32_t target_60 = 0;      // the centred object's ID (0: none)
    std::int32_t shown_64 = 0;       // the object the blend comes from
    RootVector position_70{};        // the centre (the object's +e4, blended)
    RootVector from_80{};
    float blend_90 = 0.0F;
    float hold_94 = 0.0F;            // L1 / R1 held: seconds until the highlight is centred
    // The marker manager's highlight +3c (00272ad0 writes, 00272ac0 reads): the highlighted record's object ID.
    std::optional<std::uint32_t> highlight_3c;
};

// 0026e070's tail from app +24c and each model's bounds (nullopt: no cVUM_GOBJ; skipped like a model whose
// min.x > max.x, 0x0026e488). The union starts at min (10, 10, 10) / max (-10, -10, -10) (0x0026e274..).
HudMapGlobals hud_map_construct(std::int32_t level_index, const std::vector<std::optional<HudMapBounds>>& models);
// 0026c130: pitch 0.35, 004ff074..7c 0, zoom the default; +60 = +64 = +90 = +94 = 0.
void hud_map_reset(HudMap& map);

// 0026be80(+6a0, pad): the sticks of Pad_GetSticks 00369980 (raw axes in [-127.5, 127.5], y down) turn the
// pitch (left y, 0.0004 per unit, [0.2618, pi / 2]) and the zoom (right y, 0.0001, [zoom min, pi / 4]).
// Returns the GS_3D_MAP_TURN message for the canvas (0x10 play, 0x11 stop, 0 none: send(.., a3 9)).
std::uint32_t hud_map_sticks(HudMap& map, const std::array<float, 2>& left, const std::array<float, 2>& right);

struct HudMapCamera {
    float heading_64 = 0.0F;     // the followed camera 0x004ffc14's +64
    std::int32_t player_id = 0;  // *004f82a0
    // Object_LookupById then the object's +e4; nullopt: no such object.
    std::function<std::optional<RootVector>(std::int32_t)> position;
};
// 0026d040(map, dt): the centred object (+60; missing: the player), its 0.5 s cosine blend from +80, then
// +20 = B x R x T: B the centre's -s translation, R = Rz(heading) then Rx(pitch), T row 3 (0, -ff084, 0, 1).
void hud_map_camera(HudMap& map, float dt, const HudMapCamera& camera);
// 0055a290 (0026d574..): +20 with rows 0..2 xyz times s, the IAMAP screen's 0055c0c0 during the map draw.
HudMapMatrix hud_map_ie_matrix(const HudMap& map);
// The camera record of 0026ca70 (half fov 0048b2b0, near 0048b298, far 0048b290) as clip columns of a
// model point (c = sum col[k] p[k]), y forward as 003754a0's A swap.
std::array<std::array<float, 4>, 4> hud_map_clip(const HudMap& map);
// 003754a0 on a record with half fov +100, near +f4 and far +fc: the record words +50..+ef (P the first 16,
// qword columns) with the EE FPU steps and the in-game aspect D_00539184 = 4 / 3 (EE goldens
// tests/hud_map_cull_goldens.hpp).
std::array<float, 40> hud_map_projection_003754a0(float half_fov, float near_plane, float far_plane);
// 0037d9a0's renderer +f00 for that record over the view `v` (qword columns): (P x +e80) x v with the VU
// column products, +e80 the y / z swap.
std::array<std::array<float, 4>, 4> hud_map_clip_record(float half_fov, float near_plane, float far_plane,
                                                        const HudMapMatrix& v);
// The IAMAP draw 0x0026cdb4..0x0026ce14: the current camera record gets +100 = 0048b2b0, +f4 = 1.25,
// +fc = 0048b290, +106 = 1, +104 = 0 (perspective); its view is the identity, as the screen's +61 nodes
// start from 0055c0c0 (hud_map_ie_matrix, flag 0055c100 = 1).
std::array<std::array<float, 4>, 4> hud_map_iamap_clip(const HudMap& map);
// 0026d620's +94 countdown: when it runs out, the highlighted record's +8 tracker's +68 object (nullopt: no
// highlighted record or no tracker) becomes +60.
void hud_map_hold_step(HudMap& map, float dt, std::optional<std::int32_t> highlighted_object);
inline void hud_map_hold(HudMap& map) { map.hold_94 = 0.25F; }  // 0026d710
// 0026d6d0: +94 = 0, +64 = +60, +80 = +70, +60 = the player.
void hud_map_release(HudMap& map, std::int32_t player_id);

// 0026c180 (prev) / 0026c3b0 (next) over the manager's +24 list (records head first by object ID). True:
// the highlight moved (or stayed: no match) and the commit 0026c570 follows (the caller returns 1);
// false: an empty list, or fewer than two records with a highlight (return 0, no commit).
bool hud_map_prev(HudMap& map, const std::vector<std::uint32_t>& records);
bool hud_map_next(HudMap& map, const std::vector<std::uint32_t>& records);
// 0026c8d0: the first kind-1/2 marker immediately becomes the objective target.
// 0026c830: removing the selected marker advances in the pre-removal +24 list,
// or clears it when that list contains only the marker. True means the caller
// must run cMap_ObjectiveSelected (0026c570) after updating the highlight.
bool hud_map_target_added(HudMap& map, std::uint32_t id, std::uint32_t kind);
bool hud_map_target_removed(HudMap& map, std::uint32_t id, std::uint32_t kind,
                            const std::vector<std::uint32_t>& records_before);
// 0026c7e0: the next HUD update selects the list head if no target remains.
bool hud_map_update_selection(HudMap& map, const std::vector<std::uint32_t>& records);
// 0026c570's tail (0x0026c694..0x0026c7c0): zoom = min(max(atan(s (20 + |player - target|) w / h / |ff084|), zoom),
// pi / 4) for the camera record *00538c60's viewport (+8 w, +c h).
void hud_map_fit(HudMap& map, const RootVector& target, const RootVector& player, const std::array<float, 4>& viewport);

}  // namespace omega
