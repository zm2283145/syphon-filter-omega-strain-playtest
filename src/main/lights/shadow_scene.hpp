#pragma once
// Per-frame character-shadow scene (LEVEL_LIGHTS.md section 7): receivers kept
// beside the merged terrain mesh, caster facts from the solo actor, the light
// table and build_shadow_frame with the stored caster cursor, plus the
// receiver-pass constants 0037c870 the renderer uploads per batch.
//
// The original culls objects with the VU0 micro routine 003c8990 before the
// shadow walk; that routine is not ported. ShadowScene uses a NATIVE
// SUBSTITUTE (shadow_visibility_substitute) unless the host supplies the
// visibility itself; receiver and caster sets can differ from the original
// at the view edges.
#include "caster_bounds.hpp"
#include "character_shadows.hpp"
#include "geometry.hpp"
#include "level_lights.hpp"
#include "solo_actor_simulation.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace omega {
// ---- receivers ------------------------------------------------------------------
// 00377120 alone: per material, flag 0x10 -> 0x20 (shadow receiver), 0x20 ->
// 0x40 (reflection receiver); pass shaders 11 -> 0x400, 10 -> 0x800, 14 ->
// 0x1000; then header flag 0x10 clears 0x60 and sets 0x20000.
std::uint32_t fold_shadow_model_flags(std::uint32_t flags,std::uint32_t header_flags,std::span<const MeshMaterial> materials);
// Model +0x24 after load (00377a84..00377aac): 0 (constructor, 00377c5c), then
// 00377570's header bits (header flag 1 -> 0x400000, else 2 -> 0x800000, else
// 0x200000), then 00377120. Mesh::header_flags is VUM header +4.
std::uint32_t shadow_model_flags(const Mesh& model);
// The bucket 0013e1b0 files a model instance in (0013e4a8..0013e5a0): header
// flag 8 first, then model flag 0x800000, then 0x400000, else the first list.
// The shadow walk visits the lists in the order first, (actors), 0x400000,
// header 8, (actors); 0x800000 instances are never visited. Each list holds
// at most 1024 objects (0013e374).
enum class ShadowWalkBucket : std::uint8_t { first, model_400000, header_8, model_800000 };
ShadowWalkBucket shadow_walk_bucket(std::uint32_t header_flags,std::uint32_t model_flags);
// One model instance the shadow walk can register as a receiver (kind 1).
struct ShadowReceiverInstance {
    std::uint8_t kind{1};                  // object +0x49 (terrain instances 00395dd0 are kind 1)
    std::uint32_t header_flags{};          // VUM header +4
    std::uint32_t model_flags{};           // model +0x24 (shadow_model_flags)
    ShadowVector bounds_min{},bounds_max{};// VUM header +0x30/+0x40 (Mesh::declared_bounds, stored w lanes)
    std::optional<ShadowMatrix> transform; // instance +0x50; terrain has none (world coordinates)
    std::uint32_t first_surface{},surface_count{};   // the instance's surfaces in the merged mesh
    bool active{true};                     // +0x2c/+0x2d set, +0x2e clear (zone activation is the host's)
    // false: a GOB's model sub-instance (its +0x64 part, cVUM_GOBJ 00395dd0). It is
    // never filed by 0013e1b0 (only the GOB is), so 0013f1a0 never walks it; only
    // the type-3 projectors (003d40d0 with a2 = 1) register it. Its +0x38 is the
    // GOB's, copied by the GOB draw 0021a840 when the GOB is walked: `walked`
    // (set by the host) is +0x38 != 0x80, i.e. the GOB has been drawn at least
    // once (the copy only happens for a filed GOB, whose code is never 0x80).
    bool filed{true};
    bool walked{false};
    // PORT-ONLY: the instance's surfaces in the merged mesh are already posed in
    // world space (apply_stored_pose), so the receiver constants use the clip
    // alone and the facing direction rotated by `transform` (the VU routine
    // dots the world direction with the model-space normal); the record centre
    // 003d40d0 still goes through `transform`.
    bool world_vertices{false};
};
struct ShadowReceiverTable { std::vector<ShadowReceiverInstance> instances; };
// Records `source` as the next terrain instance; call it just before
// append_terrain(destination, ..., source, ...), which appends source's
// surfaces at destination.surfaces.size().
void add_terrain_shadow_receiver(ShadowReceiverTable& table,const Mesh& destination,const Mesh& source);
// Records a GOB's model sub-instance (filed = false, world_vertices = true):
// `model` is the unposed VUM (its declared bounds and flags), `transform` the
// GOB's +0x50 matrix and [first_surface, first_surface + surface_count) its
// posed surfaces in the merged mesh. Returns the instance index.
std::size_t add_gob_shadow_receiver(ShadowReceiverTable& table,const Mesh& model,const ShadowMatrix& transform,
    std::uint32_t first_surface,std::uint32_t surface_count);
// 0037cad0..0037ce0c for one batch of a receiver record: per slot k (VU 16 +
// 7k) the clip matrix times the instance transform (clip alone without one),
// then scale, offset and facing (VU 16 + 7k + 4/5/6). The VU 15 qword
// (manager +0x104e0) is copied from the manager and not produced here.
struct ShadowReceiverSlotConstants { ShadowMatrix clip; ShadowVector scale{},offset{},facing{}; };
std::vector<ShadowReceiverSlotConstants> shadow_receiver_constants(const std::vector<ShadowSlot>& slots,const ShadowReceiverRecord& record,
    const ShadowBatch& batch,const std::optional<ShadowMatrix>& transform);

// ---- visibility (NOT ORIGINAL) ---------------------------------------------------
// Native substitute for the VU0 micro routine 003c8990 (with 003c8ad0, which
// loads renderer +0xf00): the eight corners of the local box, through the
// object transform when present, against camera_clip; false (culled, +0x38 =
// 0x80) when every corner is outside one common plane |x|,|y|,|z| <= |w|.
// The original's exact test, its depth handling and the distance it writes
// (+0x3c) are unknown (LEVEL_LIGHTS.md question 7).
bool shadow_visibility_substitute(const ShadowMatrix& camera_clip,const ShadowVector& minimum,const ShadowVector& maximum,
    const std::optional<ShadowMatrix>& transform);

// ---- casters -----------------------------------------------------------------------
// What 0013f1a0 / 003d5260 / 003d37e0 / 003d4810 read from an actor.
struct ShadowCasterFacts {
    ShadowMatrix transform;                // actor +0x50 transform object +0x10..+0x4f
    // Actor +0x60/+0x70 (render object bounds, skeleton space): human_actor_bounds
    // (caster_bounds.hpp) over the skeleton node matrices.
    ShadowVector bounds_min{},bounds_max{};
    bool has_controller{true};             // actor +0x58 non-null
    bool is_player{true};                  // +0x58 vcall +0x54 (player component 00177b10: 1, AI: 0)
    bool on_flagged_floor{};               // vcall +0x110 (0013e020): physical +e8 and record +0x51 or +0x52
    std::optional<ShadowVector> ground_plane; // vcall +0xf8 (0014d3d0): physical +f0 while +e8 is set
    bool visible{true};                    // +0x38 != 0x80 after the frame's culling
};
ShadowCasterInput shadow_caster_input(const ShadowCasterFacts& facts);
// The solo actor's caster facts. The floor contact record (physical +0x100,
// built by 001e1b70 at 001e40ec whenever the floor response sees a probe hit)
// keeps its bytes +0x51/+0x52 = the hit triangle's raw byte 11 / 12 bit 0x80
// across frames; observe() follows it from each step's trace.
class SoloShadowCaster {
public:
    void observe(const SoloActorSimulation& simulation,const SoloActorFrame& frame);
    // Transform = root_placement_matrix(component_placement()); is_player
    // true; bounds = human_actor_bounds(simulation.skeleton_node_matrices(),
    // attachment()), i.e. the pose of the last model evaluation (the original
    // world step 00392aa0 evaluates the model, runs 003a9220 and copies the
    // bounds in 00389fa0 before the frame's shadow build). Throws when the
    // animation model does not host skeleton node matrices.
    ShadowCasterFacts facts(SoloActorSimulation& simulation) const;
    // The same facts with explicit bounds (hosts without skeleton nodes, tests).
    ShadowCasterFacts facts(SoloActorSimulation& simulation,const ShadowVector& bounds_min,const ShadowVector& bounds_max) const;
    // Render slot 0 (+0x2410): the held weapon's sphere. The port has no weapon
    // models; nullopt (the default) is the original's empty slot.
    void set_attachment(std::optional<ActorAttachmentSphere> sphere) { attachment_=sphere; }
    const std::optional<ActorAttachmentSphere>& attachment() const { return attachment_; }
    bool record_51() const { return record_51_; }
    bool record_52() const { return record_52_; }
private:
    bool record_51_{},record_52_{};
    std::optional<ActorAttachmentSphere> attachment_;
};
ShadowMatrix shadow_matrix(const std::array<float,16>& column_major);

// ---- per-frame driver -----------------------------------------------------------
struct ShadowSceneSettings {
    ShadowSettings shadow;                 // mode 0x75 (|0x80 or 0x100), caps 2/4, radius 80, strength 0.33
    float aspect{640.0F/480.0F};           // renderer +0xf94: renderer_aspect(640, 448), NTSC (0037f740)
    float near_plane{2.0F};                // 00132ef4
};
struct ShadowSceneFrame;
struct ShadowSceneFrameInput {
    // PlayerCameraState::world_matrix (+da0, the camera's +0x50 transform):
    // rows X, Y (forward), Z (up) and the position, as column qwords.
    std::array<std::array<float,4>,4> camera_world{};
    float half_angle{};                    // PlayerCameraState::rig.half_fov_60 (camera +0x60)
    // Actors in walk order (bucket +0x7140 before +0x10e0); at most the first
    // four visible ones register.
    std::vector<ShadowCasterFacts> casters;
    // Visibility (+0x38 != 0x80) per receiver instance and per caster. When
    // absent, the native substitute decides (instances: declared bounds and
    // transform; casters: their bounds and transform).
    std::optional<std::vector<bool>> receiver_visible;
    // Also require the substitute test for casters (their bounds through their
    // transform); when false only ShadowCasterFacts::visible counts.
    bool cull_casters{true};
    // Port-only (Checkpoint 132ws): the factor on camera_clip's x row for the
    // visibility tests above (frame_wide_cull_x_scale: the wide Hor+ view);
    // 1 keeps the PS2's 4:3 test.
    float view_x_scale{1.0F};
    // Port-only (Checkpoint 132ws3): the same for the y row (1 / frame_fov_scale:
    // the field of view's taller view; it also divides view_x_scale).
    float view_y_scale{1.0F};
    // Called once the frame's type-1/2 slots are built and before the receiver
    // constants: adds the type-3 projector slots (add_shadow_projector).
    std::function<void(ShadowSceneFrame&)> projectors;
};
struct ShadowSceneBatch { ShadowBatch batch; std::vector<ShadowReceiverSlotConstants> constants; };
struct ShadowSceneReceiver {
    std::size_t instance{};                // index into ShadowReceiverTable::instances
    std::size_t record{};                  // index into ShadowSceneFrame::frame.receivers
    std::vector<ShadowSceneBatch> batches;
};
struct ShadowSceneFrame {
    ShadowMatrix camera_view;              // shadow_rigid_inverse(camera_world)
    ShadowMatrix camera_projection;        // 003754a0 for (half_angle, aspect, near, far)
    ShadowMatrix camera_clip;              // (projection * axis) * view: renderer +0xf00 as 0037d9a0 builds it [S]
    float far_plane{};                     // level_camera_far
    // Renderer +0x180/+0x190 (level_fog, 001339b0); nullopt when the current
    // environment's terms are undefined (equal near/far or overflow): only the
    // silhouette level needs them, and it then throws.
    std::optional<LevelFog> fog;
    LevelLightTable lights;
    // What build_shadow_frame received: input.receivers[k] is table instance k
    // (hidden when inactive or culled); input.casters[k] is the k-th caster.
    ShadowFrameInput input;
    ShadowFrame frame;
    std::vector<ShadowSceneReceiver> receivers;   // per frame.receivers record, same order
};
// 003d2f60..003d2fa8 for a caster of the frame: shadow_fog_factor(camera_clip,
// fog.base, fog.scale, translation of the caster's transform). 003d2d10 calls
// it before it pushes the buffer camera, so renderer +0xf00 is still the
// frame camera's matrix (camera_clip, [S]). Throws for an index outside the
// frame's casters.
float shadow_scene_caster_fog(const ShadowSceneFrame& frame,std::size_t caster);
// 003d3700 at a point (003d4350's projector strength), same matrix; throws when
// the environment's fog terms are undefined.
float shadow_scene_point_fog(const ShadowSceneFrame& frame,const ShadowVector& point);
// The type-1 silhouette level a3 of a slot: shadow_silhouette_level(strength,
// shadow_scene_caster_fog(frame, slot.caster)). Throws for other slot types.
std::int32_t shadow_scene_silhouette_level(const ShadowSceneFrame& frame,const ShadowSlot& slot);
class ShadowScene {
public:
    ShadowScene(LevelLights level_lights,ShadowReceiverTable receiver_table,ShadowSceneSettings scene_settings={});
    // One frame: camera view and projection, light table (00373670), culling,
    // walk order, build_shadow_frame with the stored caster cursor, and the
    // 0037c870 constants per receiver batch. The cursor is updated.
    ShadowSceneFrame build(const ShadowSceneFrameInput& input);
    // The light table build() makes for this camera (0013e820 -> 00373670),
    // without the rest of the frame (the stealth pass reads the last one).
    LevelLightTable light_table(const std::array<std::array<float,4>,4>& camera_world,float half_angle) const;
    // The camera matrices build() makes (camera_view, camera_projection,
    // camera_clip, far_plane of ShadowSceneFrame), without the rest of the frame.
    struct Camera { ShadowMatrix view,projection,clip; float far_plane{}; };
    Camera camera(const std::array<std::array<float,4>,4>& camera_world,float half_angle) const;
    LevelLights& lights() { return lights_; }
    const ShadowReceiverTable& receivers() const { return receivers_; }
    // A GOB sub-instance's +0x38 (ShadowReceiverInstance::walked) and its GOB's
    // current +0x50 matrix (pathed GOBs move). Throw for a filed instance.
    void set_receiver_walked(std::size_t instance,bool walked);
    void set_receiver_transform(std::size_t instance,const ShadowMatrix& transform);
    std::size_t caster_cursor() const { return cursor_; }
private:
    LevelLights lights_;
    ShadowReceiverTable receivers_;
    ShadowSceneSettings settings_;
    std::size_t cursor_{0};                // manager +0x11768 (0 after boot)
};
}
