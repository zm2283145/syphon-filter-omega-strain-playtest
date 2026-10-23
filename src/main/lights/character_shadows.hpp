#pragma once
// Character shadows and floor reflections: the per-frame projector slots of the
// shadow manager 00543790 (research note CHARACTER_SHADOWS.md, milestones 1-3).
// Every float operation follows the original instruction order and is
// bit-exact with the EE interpreter model (IEEE binary32, round to nearest,
// denormal results flushed to zero). Inputs the original would turn into
// meaningless values (zero-length directions, a zero fog w, a slot list past
// its 24 entries) throw std::runtime_error.
//
// Frame order (0013e828..0013e874): reset casters 003d5360 and receivers
// 003d4300, walk the object lists with 0013f1a0 (receivers 003d40d0, casters
// 003d5260), then 003d37e0 (light entries 00372c50, ground entry 003d5370,
// ShadowBuf holders reset to -1, slots 003d4810 per caster with receiver
// assignment 003d3d50). Type-3 projectors (the blood decals: 002792f0 ->
// 003d4350) are add_shadow_projector, called after the frame is built.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {
using ShadowVector=std::array<float,4>;
// A matrix as the game stores it: four column qwords, v' = sum_k column[k]*v[k].
struct ShadowMatrix{std::array<ShadowVector,4> column{};};

// ---- primitives -----------------------------------------------------------
// vmulax/vmadday/vmaddaz/vmaddw: ((c0*v.x + c1*v.y) + c2*v.z) + c3*v.w.
ShadowVector shadow_transform(const ShadowMatrix& m,const ShadowVector& v);
// Column k of the product is shadow_transform(a, b.column[k]).
ShadowMatrix shadow_multiply(const ShadowMatrix& a,const ShadowMatrix& b);
// 00133200: transpose the 3x3 part; a nonzero translation t becomes R^T*(-t)
// (w kept). Used on the current view matrix to get the camera position.
ShadowMatrix shadow_rigid_inverse(const ShadowMatrix& m);
// Renderer +0xe80 (00539070), written once at 0037ec24: the Y/Z swap.
ShadowMatrix shadow_axis_matrix();
// 003d2b20: view matrix of a projector at `eye` looking along `direction`
// (+Y forward). Throws for a zero-length direction.
ShadowMatrix shadow_look_at(const ShadowVector& eye,const ShadowVector& direction);
// 003d2970 arguments: f12 half-angle, f13/f14 u range, f15/f16 v range,
// f17 near, f18 far, f19 x scale, stack +0 y scale, stack +8 intensity.
// The slot builder passes (0,1), (0,1), 1, 1.
struct ShadowPerspectiveInput{
    float half_angle{};
    float u0{0.0F},u1{1.0F},v0{0.0F},v1{1.0F};
    float near_plane{},far_plane{};
    float x_scale{1.0F},y_scale{1.0F};
    float intensity{};
};
// matrix: m00 = 2n/(xs*(2n*tan)), m11 = 2n/(ys*(2n*tan)), column 2 = (0,0,(f+n)/(f-n),1),
// column 3 = (0,0,-(2f*n)/(f-n),0). scale = (|u1-u0|/2, -|v1-v0|/2, -64, 1),
// offset = (|u1-u0|/2, |v1-v0|/2, 64, 0); intensity > 0 sets scale.z = 0 and
// offset.z = 128*intensity.
struct ShadowPerspective{ShadowMatrix matrix; ShadowVector scale{},offset{};};
ShadowPerspective shadow_perspective(const ShadowPerspectiveInput& input);
// 003d3700 after its renderer refresh 0037d9a0: w = (fog_matrix*position).w,
// result clamp(fog_base + fog_scale/w, 0, 255). fog_matrix is renderer +0xf00
// (camera projection * +0xe80 * camera view), fog_base/fog_scale are renderer
// +0x180/+0x190. A zero w gives div.s's +-FLT_MAX (0 / 0: +FLT_MAX), so the
// clamp yields 0 or 255.
float shadow_fog_factor(const ShadowMatrix& fog_matrix,float fog_base,float fog_scale,const ShadowVector& position);
// 003d2d10, type-1 buffer render: silhouette ambient level
// a3 = trunc(1 + (fog*(1/255)) * (127*strength)), with fog = shadow_fog_factor
// at the actor's transform translation.
std::int32_t shadow_silhouette_level(float strength,float fog);
// 00379e20: clip judgement of one point: bits 1/2 +x/-x, 4/8 +y/-y, 0x10/0x20
// +z/-z against |w| (VCLIPw), plus 0x40 unless z < depth_reference (renderer +0x1b8).
int shadow_clip_code(const ShadowMatrix& clip,const ShadowVector& point,float depth_reference);
// 00379b70: the eight corners spanned by two points (xyz, w = 1). 0x80 when all
// corners share an outside bit; otherwise 4 if a corner is behind (-z), else 1
// (all inside, or only bit 0x40) or 2, each with 0x10 added when a corner has 0x40.
int shadow_box_clip_code(const ShadowMatrix& clip,const ShadowVector& minimum,const ShadowVector& maximum,float depth_reference);

// ---- manager state ----------------------------------------------------------
constexpr std::uint32_t shadow_mode_shadows=0x20;       // mode word 005392d0
constexpr std::uint32_t shadow_mode_reflections=0x40;
constexpr std::uint8_t shadow_entry_point=0,shadow_entry_directional=1,shadow_entry_ground=2;
constexpr std::uint8_t shadow_slot_shadow=1,shadow_slot_reflection=2,shadow_slot_projector=3;
constexpr std::size_t shadow_max_casters=4,shadow_max_entries=6,shadow_max_slots=24,shadow_max_receivers=512;
constexpr std::size_t shadow_receiver_slot_limit=24,shadow_batch_size=4;

struct ShadowSettings{
    std::uint32_t mode{0x75};          // 005392d0 (level setup 00132ed0 stores 0x75|0x80 or 0x75|0x100)
    int entry_cap{2};                  // manager +0x0: 6 from 003d57a0, 2 at level setup 00132efc
    int caster_cap{4};                 // manager +0x4: casters per frame and lights examined per caster
    int receiver_radius{80};           // manager +0xc (feet)
    float reflection_strength{0.33F};  // manager +0x11744 (0x3ea8f5c3)
    bool require_visible_casters{false}; // byte 005721c8 read by 0013f1a0 (the multiplayer flag: players only)
};
// Caster record entry (record +0x10 + 0x30*k). Slot building writes the
// projector position/direction back into it (types 1 and 2).
struct ShadowEntry{std::uint8_t type{}; float strength{}; ShadowVector position{},direction{};};
struct ShadowCasterRecord{std::size_t caster{}; int limit{}; std::vector<ShadowEntry> entries;};
// Slot (manager +0x10540 + 0xc0*k). view = +0x10, clip = +0x50 (P'*A*V),
// scale/offset = +0x90/+0xa0 xyzw as 003d2970/003d4810 leave them (the
// render-time TEX0 words in their w lanes are not part of this), facing = +0xb0.
struct ShadowSlot{
    std::size_t caster{};              // index into ShadowFrameInput::casters
    float strength{},half_angle{};     // +0x4, +0x8 (0.9*theta)
    std::uint8_t type{};               // +0xe: 1 shadow, 2 reflection, 3 projector
    std::int16_t texture{-1};          // +0xc: the type-3 texture handle (s16)
    ShadowMatrix view,clip;
    ShadowVector scale{},offset{},facing{};
};
struct ShadowReceiverRecord{std::size_t receiver{}; ShadowVector center{}; std::vector<std::size_t> slots;};

// ---- inputs --------------------------------------------------------------------
// Light record fields read by 00372c50 (light pointer in the table entry +4).
struct ShadowLight{
    std::uint8_t type{};               // +0x1e: 2 directional, 4 point, 0x10 spot, other values unattenuated
    float intensity{};                 // +0xc
    float inner{},outer{};             // +0x10, +0x14 distance attenuation
    float weight{};                    // +0x18 (copied to the table entry)
    ShadowVector position{},direction{}; // +0x30, +0x40
    float cone_outer{},cone_inner{};   // +0x50, +0x54 cosines (cone_inner == 1 disables the cone)
};
// Light manager table entry (00535d20 +0x60 + 32*k, filled by 00373940 for
// the lights that pass its camera test).
struct ShadowLightTableEntry{
    std::uint8_t flags{};              // +0x0 (the light type)
    std::size_t light{};               // +0x4, index into ShadowLightSet::lights
    float range{},weight{};            // +0x8 (light +0x14), +0xc (light +0x18)
    ShadowVector position{};           // +0x10 (light +0x30)
};
// The fields 00373940 copies from a light (its camera test is level_lights.hpp's rebuild_level_light_table).
ShadowLightTableEntry shadow_light_table_entry(const std::vector<ShadowLight>& lights,std::size_t index);
struct ShadowLightSet{
    bool any_lists{true};              // manager +0x14/+0x18/+0x1c not all zero
    std::vector<ShadowLight> lights;
    std::vector<ShadowLightTableEntry> table;
};
// An actor as 0013f1a0/003d5260/003d4810/003d37e0 see it.
struct ShadowCasterInput{
    ShadowMatrix transform;            // transform object (+0x50) +0x10..+0x4f, already refreshed
    ShadowVector bounds_min{},bounds_max{}; // actor +0x60/+0x70 (model space)
    bool hidden{false};                // actor +0x38 == 0x80: culled this frame (LEVEL_LIGHTS.md 4.2)
    bool has_visibility_object{true};  // actor +0x58 nonnull: the actor has a controller component
    bool visible{true};                // +0x58 vcall +0x54 result: the controller is a player (00177b10)
    bool model_ready{true};            // vcall +0x110 nonnull with byte +0x52 or +0x51 set: standing on a flagged floor triangle
    std::optional<ShadowVector> ground_plane; // vcall +0xf8 (nullopt when it returns 0)
};
// A model instance as 0013f1a0/003d40d0/003d3d50 see it.
struct ShadowReceiverInput{
    std::uint32_t model_flags{};       // model +0x24 (0x20 shadows, 0x40 reflections)
    bool model_loaded{true};           // model +0 nonzero
    bool hidden{false};                // instance +0x38 == 0x80
    std::optional<ShadowMatrix> transform; // instance +0x50 transform object matrix
    ShadowVector bounds_min{},bounds_max{}; // model +4 -> +0x30/+0x40
};
struct ShadowObjectRef{bool caster{}; std::size_t index{};};
struct ShadowFrameInput{
    ShadowSettings settings;
    ShadowMatrix camera_view;          // renderer +0xa80 + 0x40*[00538c64]
    ShadowLightSet lights;
    std::vector<ShadowCasterInput> casters;
    std::vector<ShadowReceiverInput> receivers;
    std::vector<ShadowObjectRef> order; // object-list walk order (the five lists of 0013e840..0013e864)
    // Manager +0x11768 at the start of the frame: the record the entry adders
    // write to. Only the light branch of 003d37e0 (0x20 receivers present and
    // mode bit 0x20) sets it, so with that branch off every ground entry goes to
    // this stale record (0 after boot; the .bss object has no other writer).
    std::size_t caster_cursor{0};
};
struct ShadowFrame{
    std::vector<ShadowCasterRecord> casters;
    std::vector<ShadowSlot> slots;
    std::vector<ShadowReceiverRecord> receivers;
    std::size_t caster_cursor{0};      // +0x11768 after the frame (input for the next one)
    std::size_t projectors{0};         // +0x104d4: type-3 slots this frame (003d4800 resets it)
    // The ShadowBuf holder table (+0x11748, five slot indices) is render-time
    // cache state: 003d37e0 sets it to -1 (only when a mode bit 0x60 is set) and
    // 003d2d10 fills it. It does not change any slot, so it is not modelled.
};

// ---- steps -----------------------------------------------------------------------
// 003d5260 with the limit chosen by 0013f1a0 (2 when visible, else 1, or not
// registered when require_visible_casters). Returns false when not registered.
bool register_shadow_caster(ShadowFrame& frame,const ShadowSettings& settings,const ShadowCasterInput& caster,std::size_t index);
// 0013f1a0 gate (flags & 0x60, model loaded) then 003d40d0 (mode, count < 512,
// not hidden): registers and computes the world centre.
bool register_shadow_receiver(ShadowFrame& frame,const ShadowSettings& settings,const ShadowReceiverInput& receiver,std::size_t index);
// 003d5600 / 003d54b0 / 003d5370: mode bit, strength > 0, cap min(entry_cap,
// limit), append or replace the first weaker entry.
bool add_shadow_entry(const ShadowSettings& settings,ShadowCasterRecord& record,const ShadowEntry& entry);
// 00372c50 for one caster at `position`: candidates (003730f0), sorted by key
// (qsort 001273c8 with 00373ba0), at most caster_cap lights with positive intensity.
void gather_shadow_lights(const ShadowSettings& settings,ShadowCasterRecord& record,const ShadowVector& position,const ShadowLightSet& lights);
// The order in which 00372c50 examines candidates with these keys: qsort
// 001273c8 with comparator 00373ba0 (ascending; equal keys keep no stable order).
std::vector<std::size_t> shadow_light_order(const std::vector<float>& keys);
// 003d4810 for caster record `record_index` (+ 003d3d50 for each new slot).
void build_shadow_slots(ShadowFrame& frame,const ShadowSettings& settings,std::size_t record_index,const std::vector<ShadowCasterInput>& casters,
    const std::vector<ShadowReceiverInput>& receivers,const ShadowMatrix& camera_view);
// 003d3d50: append slot `slot_index` to every accepting receiver.
void assign_shadow_receivers(ShadowFrame& frame,const ShadowSettings& settings,std::size_t slot_index,const ShadowVector& caster_center,
    const std::vector<ShadowReceiverInput>& receivers);
// 003d40d0 with a2 = 1 (the mode gate bypassed): the receiver record of
// index (searched by index, else appended: count < 512, not hidden, model
// loaded), or -1.
int find_or_register_shadow_receiver(ShadowFrame& frame,const ShadowReceiverInput& receiver,std::size_t index);
// 003d4350(mgr, obj, pos, nrm, f12 = size, f13 = strength, t0 = texture): one
// type-3 projector slot on the receiver (gates: projectors < cap, a receiver
// record, < 24 slots on it, < 24 slots in all). fog is 003d3700 at the
// position (shadow_fog_factor). Returns false when a gate fails (a receiver may
// still have been appended).
struct ShadowProjectorInput{ShadowVector position{},normal{}; float size{},strength{}; std::int32_t texture{-1};};
bool add_shadow_projector(ShadowFrame& frame,std::size_t cap,const ShadowReceiverInput& receiver,std::size_t receiver_index,
    const ShadowProjectorInput& projector,const std::function<float(const ShadowVector&)>& fog);
// The whole frame (see the top of this header).
ShadowFrame build_shadow_frame(const ShadowFrameInput& input);

// 003d3cc0/003d3bc0/003d3b30: a receiver's slots are drawn in consecutive
// batches of at most four, in list order.
struct ShadowBatch{std::size_t first{},count{};};
std::vector<ShadowBatch> shadow_receiver_batches(const ShadowReceiverRecord& receiver);
}
