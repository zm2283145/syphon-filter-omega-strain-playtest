#pragma once
// Level lights (research note LEVEL_LIGHTS.md): the light manager 00535d20 as
// the mission loader leaves it, light-object activation, and the per-frame
// light table 00373670 / 00373940 that feeds the character-shadow light
// gather (character_shadows.hpp).
//
// Loading follows 001c1a20 (pools), 001c1770 (PNT/DIR light objects) and
// 001c12a0 (ENV records; the first becomes current through 001781a0, which
// activates the objects it references). Every light field, enable flag and
// pool position is interpreter-verified against those loaders on synthetic
// records; the table rebuild against 00373670 (perspective cameras).
// Unsupported cases throw std::runtime_error.
#include "character_shadows.hpp"
#include "population.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {
// Light classes by their vtable (light +0x4); +0x34 is the range the table's
// camera test uses.
constexpr std::uint32_t level_light_vtable_base=0x004d9240;       // +0x34 001479f0 (light +0x14)
constexpr std::uint32_t level_light_vtable_released=0x004d9280;   // a destroyed light object's light (+0x34 003c8c60)
constexpr std::uint32_t level_light_vtable_spot=0x004df700;       // PNT lights (+0x2c 00371b00, +0x30 00371a60)
constexpr std::uint32_t level_light_vtable_directional=0x004da070;// DIR lights (+0x2c 00144b80, +0x30 00144980)
constexpr std::uint32_t level_light_vtable_ambient=0x004d9f10;    // ENV ambient light
constexpr std::uint32_t level_light_vtable_point=0x004d9200;      // dynamic point lights (weapons, effects)
constexpr std::uint32_t level_light_vtable_flashlight=0x004e15e0; // flashlight glare; +0x34 0045bc00: FlashDazzleSize (00497a20)

// A light record (pool entry). Pool strides: ambient 0x40, directional 0x50,
// point 0x40, spot 0xe0; fields past a pool's stride do not exist there
// (direction only from 0x50, cone cosines only in the spot pool).
struct LevelLight {
    std::uint32_t vtable{};           // +0x4; 0 for a never-constructed entry
    std::int32_t id{-1};              // +0x8: -1 free; PNT: the object id; others: pool index + 1
    float intensity{};                // +0xc (0 when the record's enable byte is clear)
    float inner{},outer{};            // +0x10, +0x14
    float weight{};                   // +0x18 = 1/((r*r + g*g) + b*b)
    bool enabled{};                   // +0x1c
    std::uint8_t aux{};               // +0x1d
    std::uint8_t type{};              // +0x1e: 1 ambient, 2 directional, 4 point, 0x10 spot, 0x40 flashlight
    ShadowVector colour{},position{},direction{};   // +0x20, +0x30, +0x40
    float cone_outer{},cone_inner{};  // +0x50, +0x54 (cosines)
    // Spot pool only: the colour-patch state (00371c30 / 00371b00 / 00371a60).
    // The pools are zeroed (00374270), so it starts clear.
    std::uint8_t patches_on_58{};     // +0x58: the patches were last applied (byte)
    std::uint32_t patch_models_5c{};  // +0x5c: the registered slots
    struct PatchSlot { std::uint32_t model{}; std::uint32_t index{}; };   // +0x60 + 8*s: model (0 = empty), patch set index
    std::array<PatchSlot,16> patch_slots{};
};
enum class LevelLightPool : std::uint8_t { ambient, directional, point, spot };
struct LevelLightRef { LevelLightPool pool{}; std::size_t index{}; };
// A registered light object (kind 6, 0040c9d0): every PNT record and the first
// four DIR records, in that order.
struct LevelLightObject { std::uint32_t id{}; LevelLightRef light; };
struct LevelLights {
    // Manager +0x0/+0x10, +0x4/+0x14, +0x8/+0x18, +0xc/+0x1c. Absent pools are empty.
    std::vector<LevelLight> ambient,directional,point,spot;
    std::vector<LevelLightObject> objects;
    std::vector<PopulationEnvironment> environments;   // registered ENV objects (kind 7), file order
    std::optional<std::size_t> current_environment;    // environment controller +0xc (004ffbd4)
    // The engine variable FlashDazzleSize (storage 00497a20, default 8.0;
    // FLASHLIGHT.md 4.1): the range the glare light's vcall +0x34 returns.
    float flash_dazzle_size{8.0F};
    // The manager's byte +0x42 (00535d62) as the light setters outside the gather raise it: the pool take
    // 00373e70's constructor tail, enable 00144b80 and disable 00144980 on a state change, the position
    // vcall +28 00144b10 when it moves. The host hands it to the gather's ModelLightGather::dirty_42.
    std::uint8_t dirty_42{0};
    // The 003767e0(model, index, on) calls the PNT light's enable 00371b00 / disable 00371a60 made, in order,
    // for the host to apply to the model (toggle_color_patch) and then clear.
    struct PatchCall { std::uint32_t model{}; std::uint32_t index{}; bool on{}; std::uint32_t light{}; };
    std::vector<PatchCall> patch_calls;
    // The light objects the first ENV activated at load (001781a0: virtual +5c, whose 003ce980 sets the
    // object's +2d), in its reference order.
    std::vector<std::uint32_t> environment_activated;
    std::vector<LevelLight>& pool(LevelLightPool p);
    const std::vector<LevelLight>& pool(LevelLightPool p) const;
    LevelLight& light(const LevelLightRef& r);
    const LevelLight& light(const LevelLightRef& r) const;
};
// The PNT:/DIR:/ENV: sections a loader sees; nullopt = the tag is absent.
struct LevelLightSources {
    std::uint32_t version{70};                         // stream +0
    std::optional<std::vector<PopulationPointLight>> point;
    std::optional<std::vector<PopulationDirectionalLight>> directional;
    std::optional<std::vector<PopulationEnvironment>> environments;
    // Run 001c1470 (the PNT link pass of the full level load 001c4190). The
    // loader goldens ran 001c1a20 / 001c1770 / 001c12a0 alone, without it.
    bool point_links{false};
};
LevelLightSources population_light_sources(const Population& population);
// A loaded model's colour patch sets as 00377920 registers them: after
// 00377050 it calls 00371c30(00535d20, key, model, i) for each entry i of the
// header +0x58 table. `model` is the host's nonzero handle for the model.
struct LevelLightPatchModel { std::uint32_t model{}; std::vector<std::uint32_t> keys; };
// 00371c30 (LightMgr_Register): the first spot-pool light whose +0x8 equals
// `key` (none: nothing) takes (model, index) in its first empty slot of 16,
// +0x5c counting them; the scan stops (nothing stored) at a slot already
// holding `model`, so a model registers one index per light, and with all 16
// slots taken nothing is stored. Returns whether a slot was taken.
bool register_level_light_patch(LevelLights& lights,std::uint32_t key,std::uint32_t model,std::uint32_t index);
// 001c1a20, 001c1770, 001c12a0 (with 001781a0 for the first ENV record):
// - pools: spot = PNT count; directional = DIR count, point 24, ambient 1 (none
//   of these without DIR:); no pools at all without PNT:;
// - every PNT light enabled; every DIR light disabled; DIR records past the
//   fourth are parsed into a temporary object whose light goes back to the pool
//   (id -1, disabled, vtable 004d9280), so the next one reuses that entry;
// - the models' colour patches register (00377920 from the TER: loader
//   001c2cb0 and the GOB: loader, both inside 001c3940), in `patch_models`
//   order;
// - 001c1470 (sources.point_links): every PNT record whose link words (bytes 8..15) are neither -1
//   deactivates its own light object (virtual +60 00179b70 -> light vcall
//   +0x30 00371a60: +0x58 is still clear, so the colours stay, and +0x1c is
//   cleared); the link's zone association (00173d60) is the zone map's;
// - the first ENV takes the ambient light and becomes current; its four object
//   references (category = the ENV's id category, index < 0x10000, category <
//   0x100 as 0040c480 requires) activate the light objects they name, which
//   enables their lights (light vcall +0x2c; a PNT light's 00371b00 first
//   applies its registered patches, which are already on, and sets +0x58).
// Throws without ENV: (001c12a0 then reads its count past the end of the file),
// on an ENV reference naming an ENV object or two objects, and on cone angles
// outside game_cosf's kernel range.
LevelLights load_level_lights(const LevelLightSources& sources,const std::vector<LevelLightPatchModel>& patch_models={});
// Light-object activation (vcall +0x5c: 00179bb0 PNT / 00179200 DIR) and
// deactivation (+0x60: 00179b70 / 001791c0): the object's light vcall +0x2c
// or +0x30. PNT (spot pool, 00371b00 / 00371a60): when +0x58 differs from
// the request and +0x5c is nonzero, each nonempty slot's 003767e0(model,
// index, on) is queued in patch_calls and +0x58 follows; then +0x1c follows,
// raising dirty_42 on a change. DIR (00144b80 / 00144980): +0x1c, raising
// dirty_42 on a change. The zone tracker's requests (00224330 / 00224180)
// reach the PNT objects the link pass made zone members. Throws for an id
// that names no light object or more than one.
void activate_level_light_object(LevelLights& lights,std::uint32_t id);
void deactivate_level_light_object(LevelLights& lights,std::uint32_t id);
bool is_level_light_object(const LevelLights& lights,std::uint32_t id);
// The light vcall +0x2c (on) / +0x30 (off) itself: the spot pool's 00371b00 /
// 00371a60 as above (the queued calls carry `object_id`), the other pools'
// +0x1c and dirty_42 only.
void switch_level_light(LevelLights& lights,const LevelLightRef& ref,bool on,std::uint32_t object_id);
// The camera far distance of the frame setup (00132ff0..0013300c): 12 plus
// the current environment's +0x64. Throws without a current environment.
float level_camera_far(const LevelLights& lights);
// The far-distance global 0053834c (AI view radius 0016dfa0, corpse removal
// 00159020): 00133574..00133648 stores the current environment's +0x64 there
// while the environment controller is not blending (its +60 goes to 00538348).
// Throws without a current environment.
float level_far_distance(const LevelLights& lights);
// The draw mode word 005392d0 the frame setup stores (00132e60..00132ed4)
// while the environment controller is not blending: 0x75, plus 0x100 (fog
// byte +0x7c set) or 0x80 when the current environment's +0x6c is above 0.
// Throws without a current environment. (The blending branch interpolates
// controller +0x38/+0x3c and is not modelled.)
std::uint32_t level_frame_mode(const LevelLights& lights);
// Renderer fog terms +0x180 (base) / +0x190 (scale) from 001339b0: with
// a = 255*(1-d0), b = 255*(1-d1), n = +0x158, f = +0x15c:
//   base = ((a+b) + ((b-a)*(f+n))/(f-n)) / 2,  scale = ((f*n)*(a-b)) / (f-n),
// so base + scale/w is a at w = n and b at w = f (the fog factor 003d3700
// clamps it to 0..255; 255 = no fog). Throws when f == n or a result is not finite.
struct LevelFog { float base{},scale{}; };
LevelFog renderer_fog(float near_distance,float far_distance,float near_density,float far_density);
// The frame's fog terms (00133574..00133648, the world update 00133420 while
// the environment controller is not blending): renderer +0x150 = current
// environment +0x68, +0x154 = +0x6c, +0x158 = +0x60, +0x15c = +0x64, then
// 001339b0. Throws without a current environment. (The blending branch
// interpolates controller +0x28..+0x3c by +0x1c and is not modelled; nor is
// the world +0x67 branch at 00133444, which skips this setup.)
LevelFog level_fog(const LevelLights& lights);
// 0037f740: the virtual screen (table 004bc1d0: 1280x720, 640x512, 640x480,
// 640x360, 320x240) is the last entry, walking down from the first, that
// still holds the frame buffer (width <= w and height <= h); the aspect
// +0xf94 = w / h. The frame buffer comes from BOOT_VIDEOMODE in SFO_GAME.INI
// (00165630): "PAL" 640x512, "PROSCAN" 640x480, otherwise 640x448; the
// options code (00165420, modes 0/2/4) uses the same sizes. Both NTSC sizes
// give 640x480, aspect 4/3. Throws for sizes the walk does not resolve
// within the table (larger than 1280x720, or not positive).
float renderer_aspect(int frame_width,int frame_height);

// Camera record fields 00373670 reads (renderer +0x1f0 + 0x110*k) and the
// renderer aspect +0xf94. Gameplay values (00132e60): half angle = the game
// camera's +0x60 (PlayerCameraState::rig.half_fov_60), near 2.0, far
// level_camera_far, perspective.
struct LevelLightCamera {
    float half_angle{};               // C+0x100
    float near_plane{2.0F};           // C+0xf4
    float far_plane{};                // C+0xfc
    float aspect{640.0F/480.0F};      // renderer +0xf94: renderer_aspect(640, 448), the NTSC frame buffer (0037f740)
    bool orthographic{false};         // C+0x104 (not supported)
};
// 003754a0: the renderer perspective for a camera record.
ShadowMatrix level_camera_projection(const LevelLightCamera& camera);
// 00373670 / 00373940 for a perspective camera: the point pool, then the
// spot pool, then the directional pool, each in pool order; enabled lights
// enter as class 1 (type 2), 3 (view position inside the clip volume of
// projection*axis, the depth bit ignored) or 2 (within R + 0.5*range of the
// view-space point (0, D/2, 0), R = (D/2)/cosf(theta), D = far). `view` is
// the frame's view matrix (shadow_rigid_inverse of the camera transform).
// Throws for an orthographic camera and for a non-directional enabled light
// whose range method (vcall +0x34) is not one of the two known ones.
struct LevelLightTableRow { LevelLightRef light; std::uint8_t cls{}; };
std::vector<LevelLightTableRow> rebuild_level_light_table(const LevelLights& lights,const ShadowMatrix& view,const LevelLightCamera& camera);
// The table and the light records as build_shadow_frame takes them. The
// gather reads +0x40 for types 2 and 0x10 and +0x50/+0x54 for type 0x10;
// a table light of such a type in a pool whose stride ends before those
// fields (spot type outside the spot pool, directional type in the point
// pool) throws.
struct LevelLightTable {
    std::vector<LevelLightTableRow> rows;     // set.table[k] is rows[k]
    ShadowLightSet set;                       // set.lights: point, spot, directional pools flattened in that order
    std::vector<LevelLightRef> lights;        // set.lights[k] comes from lights[k]
};
LevelLightTable build_level_light_table(const LevelLights& lights,const ShadowMatrix& view,const LevelLightCamera& camera);
// The table a level's first world update reads, before its first render
// rebuilds it (the stealth pass 00133768 -> 00133ab0 and the particle light
// sample 003a1760 run in the world update, 0013e820 -> 00373670 after it):
// no entries. The manager's +0x4c is 0 at boot and after any front-end
// rebuild 00373670 over the pools a teardown freed (00374020 zeroes them and
// leaves +0x4c); the level loader's pool allocations 00374270 and its light
// enables do not touch it (EE goldens tests/light_table_lifecycle_goldens.hpp).
// The pools are the level's.
LevelLightTable level_start_light_table(const LevelLights& lights);
}
