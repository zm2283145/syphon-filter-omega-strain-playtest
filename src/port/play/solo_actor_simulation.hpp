#pragma once
// One locally controlled actor over static COL5 resources, run through the
// native stages in their recovered order: optional actor logic (ordinary
// channel producer 00199f30 and final heading), model evaluation, gather
// bounds (001ea240) and gather, then the physical frame 001e9980. Authored
// choices (root weights, model anchor, shape, radius, start) are explicit
// configuration, not recovered actor data. One simulation is a single-actor
// view of the world frame of actor_world.hpp (pump, logic traversal, model and
// physical traversal, frame-update event). A shared ActorWorld supplies live
// collision-owner translation velocity to support-assisted jumps; a standalone
// simulation has no moving owner. The pad helper's Cross/Triangle senders, the edge
// slots (001dfb80 with its detection body and 001e1600) and the actor's
// ledge action (0x2a, 001930a0) are hosted, with the jump to grab, the ledge
// catch, the hang and the pull-up (HANGING.md); see the Native wiring
// sections of the climb and hanging notes for what still throws.
#include "actor_animation.hpp"
#include "actor_checkpoint.hpp"
#include "actor_damage.hpp"
#include "actor_death.hpp"
#include "actor_fader.hpp"
#include "game_end_state.hpp"
#include "actor_hanging.hpp"
#include "actor_ladder.hpp"
#include "actor_object_action.hpp"
#include "actor_world_stack.hpp"
#include "actor_running_jump.hpp"
#include "actor_teleport.hpp"
#include "hotbox_world.hpp"
#include "actor_heading_state.hpp"
#include "actor_physical_frame.hpp"
#include "actor_footstep.hpp"
#include "animation_root.hpp"
#include "animation_root_clip.hpp"
#include "animation_channel.hpp"
#include "col_gather.hpp"
#include "collision_collection.hpp"
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include "actor_navigation.hpp"
#include "actor_stunned_action.hpp"
#include "player_pad_control.hpp"
#include "player_action_buttons.hpp"
#include "upper_body_pass.hpp"
#include "pad_interact_prompt.hpp"
#include "root_attachments.hpp"
#include "water_effects.hpp"
#include "weapon_field_of_view.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
class ActorMovementWorldHost;   // actor_movement_scheduler.hpp

// Static COL5 resources. Resource i has geometry handle i+1 and is its own
// owner object i+1. Terrain is already in world space (no owner matrix);
// placed objects carry a column-major owner matrix. Gathering visits resources
// in load order; the original live gather's world-object order is not reproduced.
struct SoloCollisionWorld {
    static constexpr std::uint64_t actor_handle=0x100000;
    std::vector<std::vector<std::uint8_t>> cols;
    std::vector<std::string> names;
    std::vector<std::optional<CollisionMatrix>> owners;
    // Port host input for physical +54/+a0: translational velocity of an
    // owner component in the current world frame.
    std::vector<std::optional<RootVector>> owner_velocities;
    // The game object whose virtual +b8 returns resource i (0: terrain or an
    // object the port does not name). 003c4f00 skips the direct geometry of a
    // query's excluded objects (provider +18 / +1c, 00183070).
    std::vector<std::uint64_t> game_objects;
    // 001830d0 on a game object (+2d && +2c && !+2e), tested by the LOS
    // traversal 003c4f00 (003c500c / 003c5070) and the physical gather
    // 00181260 (001813f4) before the object's +b8 geometry: an object that
    // fails it neither blocks nor collides. Empty, or geometry without a game
    // object (terrain): eligible.
    std::function<bool(std::uint64_t object)> object_eligible;
    CollisionCandidateBatch batch;
    // Parsed gather index of cols[i], built on its first query (cols only grow through add()).
    mutable std::vector<std::shared_ptr<const Col5Index>> col_indices;
    void add(std::vector<std::uint8_t> col,std::string name,std::optional<CollisionMatrix> owner=std::nullopt);
    // Names the game object of resource `handle`.
    void set_game_object(std::uint64_t handle,std::uint64_t object);
    std::uint64_t game_object(std::uint64_t handle) const;
    // Replaces an owner matrix (the entry must have one: static terrain has no transform component).
    void set_owner(std::uint64_t handle,const CollisionMatrix& owner);
    void set_owner_velocity(std::uint64_t handle,std::optional<RootVector> velocity);
    std::optional<RootVector> owner_velocity(std::uint64_t handle) const;
    void clear_owner_velocities();
    std::span<const std::uint8_t> geometry(std::uint64_t handle) const;
    std::optional<CollisionMatrix> owner(std::uint64_t handle) const;
    std::vector<ExpandedCollisionTriangle> expand(const Bounds& bounds) const;
    // Checkpoint 132win11 (port-side speed only): expand() with the caller's storage, kept across
    // queries (no per-gather allocation once warm). Returns scratch.triangles.
    struct ExpandScratch {
        ColGatherResult gathered;
        std::vector<CollisionTriangleDraft> drafts;
        std::vector<ExpandedCollisionTriangle> triangles;
    };
    // excluded_surface_flags: 00181270's a3 (scratchpad +8, tested by 001839f0 against surface byte 0);
    // the camera zoom gather 00181260 passes 4 (query +1f0).
    const std::vector<ExpandedCollisionTriangle>& expand(const Bounds& bounds,ExpandScratch& scratch,
        std::uint8_t excluded_surface_flags=0) const;
    void gather(const Bounds& bounds);
    ExpandScratch gather_scratch;
    ExpandedCollisionTriangle refresh(const CollisionTriangleSource& source) const;
    // Centroid of the largest upward-facing solid triangle, raised by height.
    RootVector pick_start(float height) const;
};

// The animation model the actor logic drives (an AnimationModel or the model
// inside an AnimatedCharacter). step() is the model evaluation 003ab510.
class SoloActorAnimation {
public:
    virtual ~SoloActorAnimation()=default;
    virtual std::size_t channel_count() const=0;
    virtual const AnimationChannelState& channel(std::size_t index) const=0;
    virtual void apply_commands(std::span<const ActorChannelCommand> commands)=0;
    virtual void set_anchor_world_orientation(const RootAnchorKey& key,const RootVector& orientation,bool reset_counter)=0;
    virtual void set_anchor_world_position(const RootAnchorKey& key,const RootVector& position)=0;
    virtual void set_anchor_world_placement(const RootAnchorKey& key,const RootParentTransform& placement,bool reset_counter)=0;
    virtual void set_anchor_state(const RootAnchorKey&,const RootAnimationState&){
        throw std::runtime_error("This animation model does not host full root-state writes");
    }
    // The anchor root's parent in another actor's collection (the body-carry
    // link 001a6d40); the local state is kept.
    virtual void set_anchor_parent(const RootAnchorKey&,std::optional<RootParentTransform>){
        throw std::runtime_error("This animation model does not host anchor parents (001a6d40)");
    }
    virtual void set_anchor_constraint(const RootAnchorKey& key,std::optional<RootMotionConstraint> constraint)=0;
    virtual void step(float elapsed)=0;
    virtual RootAnimationState anchor_state(const RootAnchorKey& key) const=0;
    // Actor motion installation (quick turn, 001aac10). Models that do not
    // host it reject the request.
    virtual ActorMotionRegistration resolve_motion(std::string_view name) const;   // 001add90
    // The named-root registry (model +0x44: CoreMotions, blendFall, blendLand,
    // blendLadderClimb*...), searched by 001926d0/001926c0. Not hosted by default.
    virtual ActorMotionRegistration resolve_named_root(std::string_view name) const;
    // The node-name map's value (model +0x24, 0018c740; the numeric node id
    // 00472e10 stores at +3514) and the numeric lookup (0018a420 tag 1).
    // Not hosted by default.
    virtual std::optional<std::uint32_t> motion_node_id(std::string_view name) const;
    virtual ActorMotionRegistration resolve_motion_id(std::uint32_t id) const;
    virtual ActorMotionRegistration retained_motion() const;                        // actor +326c
    virtual void install_motion(const MotionRequest& request,MotionInstallMode mode); // 003ad500
    // Immediate (flags bit 0) animation effects from installs and frames.
    virtual void set_motion_sink(MotionDeliveryHooks::Sink sink);
    // Root +d0 of the named anchor after the last evaluation (the model clears
    // and re-accumulates it from the clips driving that root). nullopt: the
    // model does not report root weights; the simulation then uses its
    // authored weights.
    virtual std::optional<float> anchor_weight(const RootAnchorKey& key) const;
    virtual bool has_anchor(const RootAnchorKey& key) const;
    // 00132910(P+8, node): with a node collection (context +14), the node's
    // skeleton-space translation (node +40, W included) through the component
    // matrix the last evaluation published (context +28); see
    // published_skeleton_node_position. The ledge resolution of 001e2180 reads
    // nodes 12, 16 and 0 (1e3128/1e315c/1e31a4): in MALE.SKEL the ends of the
    // two leg chains (toe tips) and the root (pelvis); names are inferred from
    // the hierarchy. Not hosted by default.
    virtual RootVector skeleton_node_world_position(int node) const;
    // 00133ed0(out, P+8, node): the node's world matrix (rows x, y, z, t) over
    // the same published component (published_skeleton_node_matrix). Not
    // hosted by default.
    virtual std::array<RootVector,4> skeleton_node_world_matrix(int node) const;
    // The skeleton-space node matrices of the last evaluation (node +0x10..+0x4f
    // of the context's node collection, file order, column-major), as 003a9220
    // reads them for the actor bounds (caster_bounds.hpp). Not hosted by default.
    virtual std::span<const std::array<float,16>> skeleton_node_matrices() const;
    // Context byte +14 (actor +2e64): the model has the human node collection
    // that skeleton_node_matrices() reports. Activation then weights its nodes
    // (001af190) and every world entry rebuilds the context centroid +50
    // (003a9090) after the physical step; the physical reset 001e9380 and the
    // model anchor 001e5880 place that centroid. False by default: the
    // centroid keeps the constructor's (0,0,0,1).
    virtual bool human_node_collection() const{return false;}
    // The placement the last evaluation published into the model's transform
    // component (003ab510: the pose aggregate, context +28). When reported,
    // the simulation's component is this placement; nullopt keeps the
    // floor-root-pose stand-in.
    virtual std::optional<RootParentTransform> published_placement() const;
    // Model +e0/+f0: the root collection's base aggregate of the last
    // evaluation (003ae750; AnimationPlayerState::base_orientation/position),
    // and node 0 +1a0 (the rotation finalizer's cache, AnimationPlayerState::
    // bone0_rotation). The upper-body aim reads both (secondary_aim.hpp). Not
    // hosted by default.
    virtual RootParentTransform base_aggregate() const;
    virtual RootVector bone0_rotation() const;
    // The upper-body pass's view of the first 21 nodes (node +1a0 / +1b0, the
    // bone record's offset, the parent bytes, the binding's bone count) and
    // its write into them: 001ce7c0's vt +38 rows per node and node 0's vt
    // +3c translation (AnimationModel::write_node_locals). Not hosted by
    // default.
    virtual UpperBodyModelNodes upper_body_nodes() const;
    virtual void write_upper_body_nodes(std::span<const std::optional<std::array<float,12>>> rows,const std::optional<RootVector>& root_translation);
    // The start placement of a named motion as 001a2470 reads it for
    // DATA_ID_JumpGrabLedge: 00190f50 lookup on actor +2ec0, then 001901c0
    // with the controller's 00190f30/00190f40 argument (copied by 0017cd90);
    // nullopt when the lookup returns null (00190060 identity placement).
    // Not hosted by default.
    virtual std::optional<RootParentTransform> motion_start_placement(std::string_view name) const;
    // The end placement of a named motion (001a77e0 / resolve_receiver_motion_placement
    // with MotionPlacementEndpoint::end), as token 0x42 (001a4a1c) reads it for
    // buddyclimb_firstguyup. Not hosted by default.
    virtual std::optional<RootParentTransform> motion_end_placement(std::string_view name) const;
    // 0018a410: the model's first float, its default crossfade duration (the
    // SKAS DEFAULT_CROSSFADE). The revive installs the retained motion with it
    // when no transition override is pending. nullopt: not hosted.
    virtual std::optional<float> default_motion_duration() const;
    // The anchor key 0018d2c0 reads for a named motion: 0018c5a0 on actor
    // +2ec0, then 001914a0(node, 0) (node byte +4 == 0, a leaf) and the leaf
    // clip's header word +24. nullopt: the node is not a leaf (the caller's
    // default 'edge', 004ad048). A name the node map does not hold (the
    // 0018c5a0 index-0 fallback) throws. Not hosted by default.
    virtual std::optional<RootAnchorKey> motion_anchor_key(std::string_view name) const;
    // The same for a numeric node id (0018da90: 0018c6b0 *(model+3c)[id],
    // unchecked in the original; an id past the node count throws). Not
    // hosted by default.
    virtual std::optional<RootAnchorKey> motion_anchor_key_by_id(std::uint32_t id) const;
    // 00198f30 / 00198ca0 / 001987d0 on actor +2ec0 with the selector {0, group}
    // (actor_gait_sample): 00197e00 reads group 0 (the walking gait) and group 5
    // (the shimmy move). nullopt: not hosted (the footstep hooks then throw).
    virtual std::optional<ActorGaitSample> gait_sample(std::int32_t group) const{(void)group;return std::nullopt;}
};

struct SoloActorConfig {
    RootVector start{0,0,0,1};
    float yaw_degrees{};
    float radius{1.74F};
    int shape{3};
    bool keep_velocity{true};
    float dt{1.0F/60.0F};
    // BOX: records the simulation hosts (types 3, 8, 9, 12, 13, 15, 16, 17; see
    // select_hosted_hotboxes; other types throw at construction). Every box is
    // activated (+2d = 1): the original activates loaded hotboxes through their
    // object associations (00173d60), which is not traced, and the solo world
    // has all of its terrain loaded (inferred). The local player's tracker
    // 001716e0 follows the actor (CROUCH_ZONES_AND_RUNNING_JUMP.md).
    std::vector<PopulationHotbox> hotboxes;
    // Registry keys of the mission objects (GOB:) loaded before the boxes;
    // type-8/12 box links may name them (build_hotbox_world).
    std::vector<std::uint32_t> hotbox_link_objects;
    // The CAM: records' keys a type-11 box may link to (build_hotbox_world).
    std::vector<std::uint32_t> hotbox_link_cameras;
    // The ENV: records' keys a type-5 box may link to (build_hotbox_world).
    std::vector<std::uint32_t> hotbox_link_environments;
    // Registry keys of the actors (NPC records) type-1/6/7 box links may name;
    // their EnableMsg/DisableMsg go to the hotbox link sink.
    std::vector<std::uint32_t> hotbox_link_actors;
    // Checkpoint 132fx4: the world objects (GOB:) type-1/6/7 box links may name besides the actors (the host's
    // InteractGOBJs); their EnableMsg/DisableMsg go to the hotbox link sink too (build_hotbox_world link_gobs).
    std::vector<std::uint32_t> hotbox_link_gobs;
    // Checkpoint 132fx4: the world objects (GOB:) type-3 box links may name besides the actors (the host's
    // PathedGOBJs); their GOBJTriggerEventMsg goes to the hotbox trigger sink (build_hotbox_world trigger_gobs).
    std::vector<std::uint32_t> hotbox_trigger_gobs;
    // The population record's two parameter words (health, armor; the
    // player is record 0). Required with an animation model: the life state
    // (virtual +108) every actor-logic stage reads depends on +40. The solo
    // actor is role 1 (the player): 001b5fe0 stores the health word in +40
    // (float) and +3398 and the armor word in +33a0/+339c, then 001ae450
    // replaces armor and max armor with StartingArmor (0048a238, 250). No
    // agent-data unlocks are applied (ACTOR_DAMAGE_AND_HEALTH.md).
    std::optional<std::array<std::uint32_t,2>> population_parameters;
    // +3260 as 001b3460 sets it from the model's skeleton slot (2 MALE.SKEL, 1 FEMALE.SKEL; 1 selects the _F
    // voices of 00187360 / 00186bf0). nullopt leaves the health state's 0 (the constructor 001b5fe0's).
    std::optional<std::uint8_t> gender_3260;
    // Fall start +30a4 before the first logic frame. Every actor loaded with the level holds -FLT_MAX (ff7fffff,
    // the teleport tail's value 001bd55c; emulator, EMULATOR_COMPARISON.md); 0 with a floor below about -19.5 is a death on frame 0.
    float fall_start_30a4{};
};
// Actor +14 is 1 for the solo actor, as for every locally constructed object
// (003cb0e0 in the root object constructor; cleared only for multiplayer
// replicas). Its readers here: 0019f450 fall damage (the damage call
// apply_actor_damage), the slot-5 request in mode 10, the pull-up/drop gates, Cross
// (slot 1) and 0x1a/0x1b, the low-ceiling probe request +3d1, tokens
// 0x29/0x2e (+3d3) and the ladder dismount 004728d0 (LADDERS.md).

// Retained actor control fields: desired vector +31c0, speed +31d0, turn +31d4,
// and the bytes the player messages write: +31b3 (action 3, forward-turn
// correction), +31b4 (action 4, accumulation) and +31b7 (kind 12, turn pose).
struct SoloActorControlFields {
    std::array<float,4> desired{};
    float speed{},turn{};
    bool turn_correction_31b3{},accumulate_31b4{},turn_pose_31b7{};
};
// Actor state words +3200 (previous), +3204 (current), +3208 (pending).
struct SoloActorStateWords {
    std::uint32_t previous_3200{},current_3204{},pending_3208{};
};

struct SoloActorFrameInput {
    // Pad mode: 0024b5d0 cases 0/4 run after actor logic (later in 001b1710
    // than 00199f30), so the command is consumed on the next frame. Its
    // elapsed time is the simulation dt.
    std::optional<PlayerMoveInput> pad;
    // With the pad: Cross (0055a4a4) and Triangle (0055a49c) words /255 after
    // the game-state gates, for the button tail 0x0024f810..0x0024fa34.
    float cross{},triangle{};
    // With the pad: Circle (0055a4a0, sp+f98) /255 after the game-state
    // gates: the roll send 0x0024cef0 (action 8, player_circle_roll_request).
    float circle{};
    // With the pad: L1 (sp+f94) and R1 (sp+f90) words /255 for 0024a850's
    // caller (0x0024f838), and the followed camera's +9c4 (the boom curve
    // +910's remaining time) that 0017a610 reads with selector 8. Without a
    // camera, selector 8 throws in the tail.
    float l1{},r1{};
    std::optional<float> camera_9c4;
    // With the pad, for the aim view (selector 8, 0024e1d0): the followed
    // camera's +50 transform rows 0 and 1 (+10/+20, the camera world matrix
    // rows), its +60 (the half field of view) and +934 (the boom curve
    // +910's current Y, read by 00250380 for the distance test 0x0024bdc0),
    // and the right stick sp+f60. Selector 8 throws without them.
    std::optional<std::array<std::array<float,4>,2>> camera_rows_50;
    std::optional<float> camera_half_fov_60;
    std::optional<float> camera_boom_934;
    // With the pad, for the field of view 0x0024f968..0x0024fa08: the
    // followed camera's +c64 and the zoom words: Triangle 0055a49c (sp+fc8,
    // stored at 0x0024bb5c before the game-state gates clear its sp+fa4
    // copy) /255 selects zoom 1, else the word 0055a4a0 (Circle, sp+fcc,
    // stored with its sp+f98 copy at 0x0024bb54, so the gates leave it) /255
    // selects zoom -1.
    std::optional<float> camera_fov_c64;
    float zoom_in_fc8{},zoom_out_fcc{};
    // With the pad (Checkpoint 132t): the D-pad action values sp+fb0 (Up),
    // sp+fb4 (Down), sp+fb8 (Left) and sp+fbc (Right), which only the pickup
    // menu block 0x0024c3d8 reads (with their end latches +b0..+b3), and
    // sp+1c0, 0045bd00(HUD +6a4): the lift floor chooser is visible (0x0024c628
    // then runs 0045bd30 through pad_combat.choose_menu_track and skips the
    // Cross latch clear).
    float dpad_up_fb0{},dpad_down_fb4{},dpad_left_fb8{},dpad_right_fbc{};
    bool choose_menu_1c0{};
    // Checkpoint 132hud: sp+f84, the D-pad Left pressure word 0055a490 /255 after the game-state gates
    // (0x0024bb30; zeroed with the others at 0x0024beb4 and, with priority bit 0xb, 0x0024c168), the
    // interact prompt's map toggle edge (0x0024ec1c), and HUD +6a8 / +6a9 (the start menu / the map shown).
    float dpad_left_f84{};
    bool hud_6a8{},hud_6a9{};
    std::array<float,2> right_stick{};
    // Without an animation model: the "flor" root frame for this step.
    std::optional<RootFrame> root_frame;
    // Actor +31dd (lead foot, read by the 0x15/0x16 take-off 001914c0) as it
    // stands at the start of this step. Its writer is the gait 00197e00: with
    // footstep hooks (set_footstep_hooks) the simulation owns it and this
    // input is ignored; otherwise the host supplies it: 1 selects the _Right
    // running-jump motions, 0 or 2 _Left.
    std::uint8_t lead_foot_31dd{};
};

// A script AI notification an AI component sends its actor (component +30)
// with transport 0: key 004ff268 Activated (0016f1c0, vtable 004dcd30, +24
// the local player's actor), 004ff270 Deactivated (0016f0e0, vtable
// 004dcd10, +24 not written), 004ff278 Dead (0014f680: +24 the killer, +28 the
// damage type; script AIDeadMsg: Attacker 0026f060, DamageType 0026f050),
// 004ff288 WaypointReached / 004ff290
// DestinationReached (00151d30 path events 5 / 6, vtables 004dccd0 /
// 004dccb0: +24 the node; +28 the path follower's +50 for WaypointReached,
// null for DestinationReached). Nodes are node collection indices (-1 =
// null). The base damage 003ccb00 sends the damaged actor itself a DamageMsg
// (actor_damage_message_key) the same way (003ccc90, transport 0).
struct ActorAiNotification {
    std::uint32_t key{};
    std::int32_t node{-1},node2{-1};
    // The script death notice (004ff278, vtable 004dccf0): +24 the killer, +28 the damage type.
    // DamageMsg (005716f0, vtable 004e08a0): object = +28 the source, value =
    // +24 the cvt.w.s damage (003ccbc8), type_2c = +2c the damage type
    // (003ccbe0), category_2d = +2d the category (003ccbe8).
    std::uint64_t object{};
    std::int32_t value{-1};
    std::uint8_t type_2c{},category_2d{};
    bool operator==(const ActorAiNotification&) const=default;
};
inline constexpr std::uint32_t actor_damage_message_key=0x005716f0;   // DamageMsg (003ccb00 -> 003ccc90)
// Script AI Unconscious Message (vtable 004dcd70 / 004dcc30, +24 the source,
// +20 2): queued to self by type-7 damage (00187cd8..00187d24) with +14.
inline constexpr std::uint32_t actor_unconscious_message_key=0x004ff2c0;
// AIBodyMovedMsg (vtable 004dcc70, +24 the carrier, +28 PickedUp as value,
// +20 2): queued to the carried body by the pickup 001abbe8 and the unlink
// 001a02c0 (actor_body_carry.hpp) when the carrier is the player.
inline constexpr std::uint32_t actor_body_moved_message_key=0x004ff2b0;
// Script AI Disguise Taken Message (AIDisguiseTakenMsg, vtable 004dcc50, +24 the taker,
// +20 2, +1c 0x40): queued to the backpack's owner by the pickup menu's kind-4 take
// (00252298..00252344, transport 0) after the owner's AI +1d8(0, 1) (checkpoint 132sx10).
inline constexpr std::uint32_t actor_disguise_taken_message_key=0x004ff2b8;

// 001b3b60's single-player branches for the hotbox link messages (1b4310 /
// 1b4440): an EnableMsg activates (+5c(1)) an actor that is not present
// (+2d, +2c, !+2e) when its component's virtual +44 holds; a DisableMsg
// deactivates (+60(1)) a present actor whose component's +141 bit 7 is
// clear. The component is read only on those paths (interpreter goldens).
enum class ActorLinkEffect {none,activate,deactivate};
ActorLinkEffect actor_link_message_effect(bool enable,bool present,const std::function<bool()>& alive_44,
    const std::function<bool()>& radius_controlled_141);

struct SoloActorFrame {
    ActorPhysicalFrameTrace trace;
    // Synchronous actor events sent this frame: stance messages (applied to
    // the player component at once, also listed in component_codes) and
    // running-world messages (no receiver here, counted only).
    int stance_events{},running_events{};
    std::array<float,18> channels{};
    std::optional<PlayerMoveCommand> pad;
    // Quick-turn detector 00249d00 sent the action-14 message this frame; the
    // actor handles it immediately (handle_actor_quick_turn).
    bool quick_turn{};
    // The actor accepted it (mode 0): the named turn motion, empty otherwise.
    std::string quick_turn_motion;
    int water_events{};
    // Pad actions sent by the Cross/Triangle senders this frame, in order.
    std::vector<std::int32_t> pad_actions;
    // 001dfb80 ran its detection body this frame.
    bool edge_detection{};
    // 001e1600 messages delivered this frame: CAS_EVENT 8 count, the winning
    // CAS_ACTION 0x2a (slot and mask) and the motions it installed.
    int edge_events{};
    std::optional<std::size_t> ledge_slot;
    std::uint32_t ledge_mask{};
    std::vector<std::string> ledge_motions;
    // The body carry's installed motions (pickup / drop / body actions; "" = retained).
    std::vector<std::string> carry_motions;
    // Player input +0x130 ("found ledge" mask, the HUD Interact prompt) after
    // this frame.
    std::uint32_t found_ledge{};
    // Input selector +0x38 used by this frame's pad (0/4 movement, 7 hang).
    std::int32_t pad_selector{};
    // Jump to grab and hang (HANGING.md). Immediate sound requests (M_GRUNT
    // from token 0x0b, M_SHIMMY_ON from 001a1c80) and the number of 001a1c80
    // timed repeats scheduled (not delivered by the solo host). Messages
    // queued with transport 0 this frame: pad actions to the actor (0x11 pull
    // up, 0x13 drop) and codes to the player component (6 from token 0x14, 7
    // from 0019f890); the next step delivers them first. delivered_actions are
    // the queued actor actions delivered at the start of this step.
    std::vector<std::string> sounds;
    int delayed_sounds{};
    std::vector<std::int32_t> queued_actions,component_codes,delivered_actions;
    // 001a2c00 called the launch 001a2470 / the catch 001a1c80 / the fall
    // 001a1590 / the landing 001a0b30 this frame (the drop 0x13 also falls).
    bool jump_launched{},ledge_caught{},fell{},landed{};
    // Landing effect requests: 00188fb0(actor, 2, 1) at once, and timed
    // 004da870 events (00188fb0(actor, 1, 1) later; not delivered).
    int landing_effects{},delayed_landing_effects{};
    // The gait 00197e00 called 00188fb0(actor, foot, 0) this frame (footstep
    // hooks only), dry-branch surface properties with an effect name the
    // level's 004089f0 did not resolve (no resolve ran), and the resolved
    // effects sent to 00232460 (FootstepHooks::effect).
    int footsteps{},unresolved_surface_effects{},surface_effects{};
    // Ladder (LADDERS.md): exit requests of pad case 6 this frame (31..34).
    std::vector<std::int32_t> ladder_exit_requests;
    // 0019a978 ran the ladder body 00474050 / it called the dismount 004728d0.
    bool ladder_body{},ladder_dismount_checked{};
    bool carry_pair_body{};               // 0019d950 ran (+324c bit 3 or 6)
    // Hotboxes: the tracker's events queued to boxes at the end of this step
    // (00170660 from the frame-update event), and the box events the pump
    // delivered to the actor this step (00172400/00171ed0 -> 001b3cb0 ->
    // 00177380). The two queued hops put the player component's reaction two
    // steps after the tracker saw the change.
    std::vector<HotboxTrackerEvent> hotbox_events;
    std::vector<HotboxActorEvent> hotbox_codes;
    std::vector<HotboxCameraAction> hotbox_cameras;   // type-11 camera pushes / removals (00172878 / 0017230c)
    std::vector<HotboxEnvironmentAction> hotbox_environments;   // type-5 environment switches (00172688 / 001721e0)
    // Box events (from the tracker) the script filter took this step; the
    // box's native receiver did not run for them.
    std::vector<HotboxTrackerEvent> hotbox_script_events;
    // The Triangle object interaction (001714c0 from the button tail): the
    // box it took this step, the GOBJTriggerEventMsg triggers it queued
    // (00171c90, transport 0) and the queued triggers the script filter took
    // this step.
    std::optional<std::size_t> interaction_box;
    std::vector<HotboxTrigger> triggers;
    std::vector<HotboxTrigger> trigger_script_events;
    // Queued triggers delivered to a backpack's receiver 00254600.
    std::vector<HotboxTrigger> native_triggers;
    // game::PerformAction_WithObject (actions 0x25/0x26/0x27, 0018d2c0)
    // delivered this step, the motions it installed ("" = retained), the
    // CAS codes the actor sent its object (0xe at phase 0.5, 0xf at the end,
    // 0xd when interrupted or refused) and the active world-object list
    // changes (00131320 push / 001313f0 pop: virtual +5c activate, +60
    // deactivate), recorded in order and not run (the objects' activation
    // effects are not hosted).
    std::optional<ActorObjectActionResult> object_action;
    std::vector<std::string> object_action_motions;
    // A named object action's node id (the kind-10 prelude of 0018d2c0).
    std::optional<std::int32_t> object_action_motion_id;
    std::uint8_t object_action_byte_38{};   // the resent message's +38 (kind-10 +64)
    struct ObjectCode {std::uint64_t object{};std::int32_t code{};};
    std::vector<ObjectCode> object_codes;
    struct WorldObjectEvent {bool activate{};std::uint64_t object{};};
    std::vector<WorldObjectEvent> world_object_events;
    // The script AI notifications (Activated 004ff268, Deactivated 004ff270,
    // WaypointReached 004ff288, DestinationReached 004ff290) this step's pump
    // delivered to the actor, and whether a script handler took each one.
    struct AiNotificationEvent {ActorAiNotification notification;bool script_handled{};};
    std::vector<AiNotificationEvent> ai_notifications;
    // The hotbox link messages (EnableMsg / DisableMsg) this step's pump
    // delivered to the actor: whether a script handler took each one, and
    // what the native receiver 001b3b60 did (activation +5c(1), deactivation
    // +60(1), or nothing).
    struct LinkMessageEvent {
        bool enable{};
        bool script_handled{};
        enum class Effect {none,activated,deactivated} effect{};
    };
    std::vector<LinkMessageEvent> link_messages;
    // The GOBJTriggerEventMsgs a type-3 box's enter (00172598) sent this
    // actor that this step's pump delivered, and whether a script handler
    // took each one (unhandled: 001b3b60 and 003cea10 have no branch for key
    // 004f5440, so 003caca0 walks the empty +0x20 child list: dropped).
    struct BoxTriggerEvent {HotboxType3Trigger trigger;bool script_handled{};};
    std::vector<BoxTriggerEvent> box_triggers;
    // Camera +cf8 bits of 00247800 (called by the pad at 0024ec14, after the
    // movement message): 0x400 for selector 4 inside a crawl zone with
    // headroom, 0x800 for selector 4 crouched (+3204 bit 4) with physical
    // +3d2. The other +cf8 bits are not hosted.
    std::uint32_t camera_crouch_flags{};
    // 00247800 at 0024ec14: +cf8 bit 2 (4) with input +3c bit 8, the aim view,
    // and then bit 3 (8) unless 001437a0(+3524) & 0xff is 1
    // (player_camera_aim_flags_cf8). The pad writes it after the targeting
    // stage, and the camera's preupdate 00280f60 (registered after the
    // actors, so later in the same logic traversal) copies it to +cf4. The
    // camera update 00280690 of this frame then switches to the aim mode.
    std::uint32_t camera_aim_flags{};
    // 0x0024f9fc / 0024fc20: camera +c64 = +c68 = the clamped 00142f20
    // (pad_field_of_view_clamp(weapon_field_of_view)). Unset without the
    // pad's field_of_view hook.
    std::optional<float> camera_field_of_view;
    // 0x0024c4f0: the pickup menu's Triangle close reset the pad (36a240 on
    // 0055a300: every port's held buttons read released until they are
    // pressed again); the host masks its held buttons from the next frame.
    bool pad_reset{};
    // 00247800 at 0x00247cd8..0x00247d14: camera +d90 = 001ca840 of the
    // secondary component +3314 (camera_recoil_rotation of its +250), or
    // zero without one. Unset when the pad has no secondary_250 hook.
    std::optional<RootVector> camera_rotation_d90;
    // 00247800 at 0024ec14 (0x00247844..0x00247990): with input +3c bit 8
    // clear, the pad's +c0/+c4 lock while the weak handle still resolves
    // (camera +d04/+d08, +d10 = *004a9330) and +e4 (camera +d0c); with bit 8
    // set, no lock and +d0c = -1. Without a combat session the pad holds no
    // lock and +d0c stays -1.
    std::optional<std::pair<std::uint64_t,std::int32_t>> camera_lock;
    std::int32_t camera_subtarget_d0c{-1};
    // 00247800 at 0024ec14: camera +d60 = sp+f30 * f24 (the pad's look
    // rates; selector 8 the smoothed aim-view rates, cases 0/4 (-0, 0, 0, 0),
    // the others zero) and camera +d30 = sp+ed0: zero for selectors 5 and 8
    // (00250310 at 0x0024c310, no 002503e0 call), else the free-look offset
    // 002503e0(right stick, input +125, +126). Unset when the selector dispatch
    // did not run: 00247800 is +d30's only writer, so the camera keeps it.
    std::array<float,4> camera_rates_d60{};
    std::optional<std::array<float,4>> camera_look_offset_d30;
    // Action 0x17 (00192800) sent the running-jump take-off this step.
    std::optional<ActorRunningJumpTakeoff> running_jump;
    // Damage and death (ACTOR_DAMAGE_AND_HEALTH.md). The 0019f450 call of
    // this step; the events of the damage call (00187360 / 003ccb00 / kill
    // 00186bf0) in order, recorded and not played (damage message, sounds,
    // rumble, the death message, the item and controller notices); the death
    // code; the death action 0018a950 (motion, landing pose).
    std::optional<ActorFallStateTrace> fall_state;
    std::vector<ActorDamageEvent> damage_events;
    // Port-only (checkpoint 132pc3): a damage call the partner shield dropped (port_cheats.hpp PortPartnerShield).
    bool port_partner_shielded{};
    std::optional<std::int32_t> death_code;
    // 00186950 turns: the heading written to the selected history entry.
    std::vector<float> hit_direction_turns;
    std::optional<ActorDeathResult> death;
    std::vector<std::int32_t> voice_lines;       // 0018c770(actor, line), the player's death voice
    int death_landing_effects{};                 // 00188fb0(actor, 0, 1) from 0019e9a0
    // 001cb120 requests from 001977f0 (death action tail) to the secondary
    // component +3314, which the solo host does not model (its per-frame
    // virtual +0c is not run either).
    std::vector<std::uint8_t> secondary_modes;
    bool dead_state{};                           // 001a3780 entered the dead state (directly for code 11, or token 0x92/0x93; not with 0x1f already set)
    std::uint8_t fade_alpha{};                   // 0025e5b0 after the world fader update
    bool fade_out{};                             // Player component code 16 started the fade-out
    bool respawn_requested{};                    // MSGGAME_RESPAWN delivered this step (001b45dc)
    bool checkpoint_added{};                     // MSGGAME_ADD_CHECKPOINT delivered this step (001b3b60)
    // 001b1de8..1e04: 0022e900 on the checkpoint component +33f0 (actor +14
    // set) queued its label for the text manager's immediate list this step
    // (the host resolves the font and the width, checkpoint_label_x).
    std::optional<CheckpointLabelDraw> checkpoint_label;
    // The revive 001898f0 ran (respawn with action 0x1f set, an AI controller,
    // or actor +14 clear). It queued MSGGAME_NOTIFY_CHECKPOINT and then
    // component code 17 (transport 0); the next step's pump delivers both
    // (notify_checkpoints).
    bool revived{},notify_checkpoint{};
    // The NotifyCheckpointMsgs this step's pump delivered: the message's
    // position in the pump's snapshot and whether a script handler took it
    // (an unhandled one has no native effect).
    struct NotifyCheckpointEvent {std::size_t pump_position{};bool script_handled{};};
    std::vector<NotifyCheckpointEvent> notify_checkpoints;
    // The cOutOfAmmoMsgs (004ea150) this step's pump delivered: the item and
    // whether a script handler took it (an unhandled one has no effect).
    struct OutOfAmmoEvent {std::int32_t item{};bool script_handled{};};
    std::vector<OutOfAmmoEvent> out_of_ammo;
    // The cWeaponVisChangeMsgs (00583968) this step's pump delivered: the
    // value (+24) and whether a script handler took it.
    struct WeaponVisChangeEvent {std::int32_t value{};bool script_handled{};};
    std::vector<WeaponVisChangeEvent> weapon_vis_changes;
    // Actor logic 001b1710 returned at once because +3269 was set (the
    // revive sets it; the respawn timer's stage 0 clears it).
    bool logic_gated{};
    // RespawnTimer 001ba130 fired this step: the stage, the fade-in it started,
    // the screen effect 00260990(1, 0, 0, 0, 0) it requested (recorded, not
    // hosted), and the end of the +33a4 invulnerability window.
    std::optional<std::uint8_t> respawn_timer_stage;
    bool fade_in{},screen_effect{},invulnerability_ended{};
    // 00186360(actor, 0, 0, 0, 0, -1.0) calls (revive and timer stage 0) on a
    // non-burning actor: no hosted effect.
    int extinguish_calls{};
    // Teleport tail (001bc4b0 §2.4): the player component's virtual +50
    // (00176fa0) shifts its camera's pivot/look curves by each delta, in
    // order. The simulation does not own the camera; the host applies them
    // with translate_camera_follow_curves.
    std::vector<RootVector> camera_shifts;
};
// SoloActorSimulation::teleport: false when a root is parented (the handler
// writes nothing); the player's camera delta otherwise (zero for the
// rotation-only modes, absent for an AI controller).
struct SoloActorTeleportResult {
    bool applied{};
    std::optional<RootVector> camera_shift;
};

// Actor +30ac..+30e0, the navigation target block. Action 0 kind 0xd
// (001a92a0) stores an AI component's request here; the navigation body
// 001a80e0 reads it in the next frame's actor logic and writes the arrival
// byte +30ad (NPC_SPAWN_AND_AI.md, "Bridge into the actor").
struct ActorNavigationTarget {
    bool enabled_30ac{},arrived_30ad{};
    RootVector target_30b0{0,0,0,1};
    float yaw_30c0{};
    RootVector facing_30d0{};
    float speed_30e0{};
};
// The equipment inputs of 001a80e0 (1a89a8..1a8a3c) for an actor driven by
// an AI component: the secondary component +3314 (allocated for every
// activated actor by 001b5560; its constructor 001d81a0 clears +310..+314),
// whether the weapon-class table 0049f790 entries of its bytes +310/+311 have
// byte +4c set (runtime-initialized; entry 0 has it clear in the boot image),
// and its state +312. The inventory mode +84 and byte +a0 are the
// simulation's inventory state.
struct ActorNavigationEquipment {
    bool present{true},primary_classified{},secondary_classified{};
    std::uint8_t state_312{};
};
// 001a92a0 action 0 (single-player branch): kind 0xd (1a99a8..1a9a10) and
// kind 0xc (1a9330..1a9388); see SoloActorSimulation::receive_navigation_*.
void apply_actor_navigation_request(ActorNavigationTarget& navigation,const RootVector& target,float yaw,float speed);
struct SoloActorControlFields;
void apply_actor_navigation_stop(ActorNavigationTarget& navigation,SoloActorControlFields& controls,std::uint8_t latch_303c,
    const RootVector& direction,float speed,float turn,std::uint8_t pose_byte);
class SoloActorSimulation;
// The kill's arguments to controller virtual +7c (00186bf0 -> 003ccac0).
struct ActorDeathNotice {
    std::int32_t amount{};
    std::uint64_t killer{};
    std::uint8_t category{},type{};
};
// The +58 component of an actor driven by an AI controller.
class ActorAiController {
public:
    virtual ~ActorAiController()=default;
    // Component virtual +3c: actor +4c (001b2a80) calls it through 003ce750
    // in the first traversal for every actor, eligible or not.
    virtual void always_run(SoloActorSimulation& actor,float dt)=0;
    // Component virtual +40: actor logic 001b1710 reaches it through
    // 00392b40 -> 003ce650 after the curve tail, where the player component
    // runs its pad producer.
    virtual void update(SoloActorSimulation& actor,float dt)=0;
    // Component virtual +48 (Activate), called by the actor's activation
    // 001b53d0 through 00392940 -> 003ce980 after +2d is set.
    virtual void activated(SoloActorSimulation& actor)=0;
    // The +58 component's part of the actor's deactivation 001b5190.
    virtual void deactivated(SoloActorSimulation& actor)=0;
    // Component virtual +44 and +141 bit 7, which the actor's EnableMsg /
    // DisableMsg branch of 001b3b60 reads. Not hosted unless overridden.
    virtual bool alive_or_eligible(SoloActorSimulation&){throw std::runtime_error("Actor AI controller: virtual +44 is not hosted");}
    virtual bool radius_controlled() const{throw std::runtime_error("Actor AI controller: +141 is not hosted");}
    // The pump (003c8de0): messages the component queued for itself with
    // transport 0, delivered after the actor's own queued messages. The pump
    // snapshots every queue first (snapshot) and then delivers (pump), so a
    // message sent while it delivers waits for the next pump. A controller
    // without a queue has nothing to deliver.
    virtual void snapshot(){}
    virtual void pump(SoloActorSimulation&){}
    // The controller virtuals the damage path calls (ACTOR_DAMAGE_AND_HEALTH.md):
    // +84 the death code adjustment (kill 00186bf0), +78 the hit notice of a
    // surviving victim and +7c the death notice (003ccb00 / 003ccac0). Not
    // hosted unless overridden.
    virtual std::uint8_t death_code_84(SoloActorSimulation&,std::uint8_t){
        throw std::runtime_error("Actor AI controller: virtual +84 (death code) is not hosted");}
    virtual void hit_notice_78(SoloActorSimulation&,std::uint64_t,std::int32_t){
        throw std::runtime_error("Actor AI controller: virtual +78 (hit notice) is not hosted");}
    virtual void death_notice_7c(SoloActorSimulation&,const ActorDeathNotice&){
        throw std::runtime_error("Actor AI controller: virtual +7c (death notice) is not hosted");}
    // The dead state 001a3780 for an actor whose controller is not the player:
    // 001a3520, the weapons and armor into a backpack (npc_dead_drop.hpp).
    virtual void dead_state_drop(SoloActorSimulation&){
        throw std::runtime_error("Actor AI controller: the dead-state drop 001a3520 is not hosted");}
    // The animation controller message (key 0055a1d8, vtable 004da950, +24 the
    // code) sent to +58 with transport 0, e.g. code 16 from the dead state.
    virtual void controller_message(SoloActorSimulation&,std::int32_t){
        throw std::runtime_error("Actor AI controller: an animation controller message is not hosted");}
    // The same message with transport 1 and event byte bit 0 (003c9420 calls
    // the component at once), e.g. the stance codes 2/3 and 4/5 of
    // 0019a700..0019a8f8.
    virtual void controller_message_immediate(SoloActorSimulation&,std::int32_t){
        throw std::runtime_error("Actor AI controller: an immediate animation controller message is not hosted");}
    // 00186da4..00186df8 (the kill, death code 2 outside category 2 with
    // virtual +58 true): the AI controller's byte +1d6 = 1.
    virtual void set_byte_1d6(){
        throw std::runtime_error("Actor AI controller: the +1d6 store of the kill (00186df4) is not hosted");}
    // 0019e978 (the dead landing 0019e670 with virtual +58 or +5c true): the
    // AI controller's word +114 = 0.
    virtual void clear_action_114(){
        throw std::runtime_error("Actor AI controller: the dead landing's +114 store (0019e978) is not hosted");}
    // The controller's virtuals +54 / +58 / +5c the damage path reads: an AI controller (0012f370, 0015b7e0, 001535d0)
    // answers 0 / 1 / 0.
    virtual ActorDamageController damage_predicates() const{return {false,true,false};}
};
// The actor's +58 component: the player component (pad producer 0024b5d0,
// virtual +54 = 1; ai == nullptr) or an AI component (virtual +54 = 0012f370
// returns 0, so 00199f30 runs the navigation body 001a80e0).
struct SoloActorController {
    ActorAiController* ai{};
    ActorNavigationTuning navigation{};        // 001a80e0 tuning (AI only)
    ActorNavigationEquipment equipment{};      // AI only
};
// What an ActorWorld shares with its actors: the static collision world, the
// game RNG state (00127dd0, one sequence for every actor) and the frame
// counter 005383c8 read by 00199f30 (19c1e8) and the AI.
struct SoloActorWorldLink {
    std::shared_ptr<const SoloCollisionWorld> world;
    std::uint64_t handle{SoloCollisionWorld::actor_handle};      // the actor object
    std::uint64_t controller_handle{0x200000};                   // its +58 component
    std::uint64_t* random_state{};
    const std::uint32_t* frame_counter{};
    // Checkpoint 132cp11: the world's water-effect manager (world +e4, built once by 00137ab0 -> 002b3070) with
    // the world's actor view and its effect sink. Null: the actor builds its own (the standalone simulation).
    WaterEffectManager* water{};
    WaterEffectActorHost* water_actors{};
    WaterEffectSink* water_sink{};
};

// A damage call to the actor's virtual +100 (00187360) from outside its own
// update (a bullet hit entry 00188780, for example): the source object and
// its +58 controller answers, the category, type, penetration (in/out),
// quiet flag, hit point and amount. source_position is the source's +e4,
// read by the kill for a direction-classified death code (types 1, 2, 0xd);
// those codes come from 00186950 (hit_direction.hpp).
struct SoloDamageRequest {
    std::uint64_t source{};
    std::optional<ActorDamageController> source_controller;
    std::uint8_t category{},type{};
    std::optional<std::int32_t> penetration;
    bool quiet{};
    std::optional<RootVector> hit_point;
    float amount{};
    std::optional<RootVector> source_position;
    // The actor's own update damages itself with another source: the burn
    // damage 001b1fa0 (virtual +100(actor, +3480, 1, 4, ...)).
    bool own_update{};
};
// The weak handle +3480 / +3484 (the fire setter) when valid, and its +e4.
struct SoloFireSetter {
    ActorDamageParty party;
    RootVector position_e4{};
};
struct SoloFireHooks {
    // 001b1e10..001b2214 (the effect slots, the burn damage, +2df6, +31f4),
    // run at 0x001b1e10: after the +3200 shift, before the curve 001b2218.
    std::function<void(float dt)> update;
    // 00186360(actor, 1, 0, 0, 0, -1.0) from 00187360 (+33b0 past 20 while
    // not burning, +14 set); empty: that damage throws.
    std::function<void()> ignite;
    // 00187360 reads +3480 / +3484 for type 4 (001873bc); empty: never valid
    // (nothing hosted can set it).
    std::function<std::optional<SoloFireSetter>()> fire_setter;
    // 00186360(actor, 0, 0, 0, 0, -1.0) on a burning actor from the revive
    // (001898f0 step 11) and the RespawnTimer's stage 0 (001ba130); empty:
    // those throw.
    std::function<void()> extinguish;
};
// What the call did: the damage result and a frame record holding the damage
// events, the death code and death action, voice lines, sounds and the
// secondary mode requests (the fields a step reports for the same work).
struct SoloDamageOutcome {
    ActorDamageResult result;
    SoloActorFrame record;
};

struct PadCombatFields {
    std::int32_t& selector_38;
    std::uint32_t& bits_3c;
    std::int32_t& top_40;
    std::uint32_t& bits_44;
    PlayerButtonState& buttons;
    // sp+1e0 = (+38 == 8), stored at 0x0024be2c before the selector shift
    // 0x0024c338 (so the previous frame's selector), and the distance bytes
    // sp+1f0 / sp+200 of 0x0024bdc0 (computed only with sp+1e0 set).
    bool aim_1e0{};
    bool near_1f0{};
    bool far_200{};
};
struct PadCombatHooks {
    std::function<void(PadCombatFields&)> targeting;
    std::function<void(PadCombatFields&)> weapon;
    // 0x0024f928..0x0024f964: the crosshair 00241ec0 (after the button tail);
    // its return is stored to actor +346e.
    std::function<void(PadCombatFields&)> crosshair;
    // 0x0024ec14: the pad's lock +c0/+c4 (set when the weak handle resolves)
    // and +e4, read by 00247800 when input +3c bit 8 is clear.
    struct CameraLock {
        std::optional<std::pair<std::uint64_t,std::int32_t>> lock;
        std::int32_t subtarget_e4{-1};
    };
    std::function<CameraLock()> camera_lock;
    // 0x0024dc5c: selector 5 resolves the R1 pick +c8/+cc (0014d350: the
    // pointer is set and its +c equals +cc) and reads the target's +50.
    std::function<bool()> pick_resolves;
    // 0x00247b40: 001437a0(+3524), the held weapon's class (11 with nothing
    // held), read only with input +3c bit 8.
    std::function<std::uint8_t()> hold_class;
    // 0x00247cd8: the secondary component +3314's +250 (nullopt without one).
    std::function<std::optional<RootVector>()> secondary_250;
    // 0x0024f9bc: 00142f20(+3524, a1, a2, camera +c64, elapsed) on the
    // actor's inventory (weapon_field_of_view).
    std::function<float(const WeaponFieldOfViewInput&)> field_of_view;
    // Checkpoint 132t: input +128, the pickup menu 00253070 that the backpack
    // receiver 001762c0 stores in the component's +178 (the combat host's;
    // `open` empty: +128 is null). The block 0x0024c3d8..0x0024c624 runs the
    // D-pad cursor / item moves, Triangle's close (00247320, the pad reset
    // and GS_CONFIRM: `confirm_sound`, 001ca010(msg, 3, "GS_CONFIRM") ->
    // 003c9420(msg, 004ffb50, 1)) and Cross's 00251920 (true: 00247320);
    // `close_for` is the hotbox 0x12 exit's 002473e0 / 00247320.
    struct PickupMenu {
        std::function<bool()> open;
        std::function<void()> previous,next,left,right;   // 00251690 / 002517d0 / 00251370 / 00251500
        std::function<bool()> confirm;                    // 00251920
        std::function<void()> close;                      // 00247320
        std::function<void()> confirm_sound;
        std::function<void(std::uint32_t key)> close_for;
    };
    PickupMenu pickup_menu;
};
class SoloActorSimulation {
public:
    static constexpr RootAnchorKey flor_key{std::uint8_t{'f'},std::uint8_t{'l'},std::uint8_t{'o'},std::uint8_t{'r'}};
    static constexpr RootAnchorKey edge_key{std::uint8_t{'e'},std::uint8_t{'d'},std::uint8_t{'g'},std::uint8_t{'e'}};
    static constexpr RootAnchorKey ladd_key{std::uint8_t{'l'},std::uint8_t{'a'},std::uint8_t{'d'},std::uint8_t{'d'}};
    // animation and tuning are both present (graph mode) or both absent.
    SoloActorSimulation(SoloCollisionWorld world,const SoloActorConfig& config,
        SoloActorAnimation* animation=nullptr,std::optional<ActorAnimationTuning> tuning=std::nullopt,
        SoloActorControlFields initial={});
    ~SoloActorSimulation();
    SoloActorSimulation(const SoloActorSimulation&)=delete;
    SoloActorSimulation& operator=(const SoloActorSimulation&)=delete;
    SoloActorFrame step(const SoloActorFrameInput& input={});

    const SoloCollisionWorld& world() const;
    const ActorPhysicalFrameState& physical() const;
    RootParentTransform floor_root_placement();
    RootVector component() const;
    // The placement behind the model's transform component (P+8 +28, actor
    // +0x50): the model's published placement when it reports one, otherwise
    // the floor root's animated pose (the stand-in described above). Its
    // matrix is root_placement_matrix of this placement.
    RootParentTransform component_placement();
    // The animation model's skeleton_node_matrices(); throws without an
    // animation model.
    std::span<const std::array<float,16>> skeleton_node_matrices() const;
    // The animation model's base_aggregate() / bone0_rotation(); throw without a
    // model that hosts them.
    RootParentTransform base_aggregate() const;
    RootVector bone0_rotation() const;
    // The animation model's upper_body_nodes() / write_upper_body_nodes();
    // throw without a model that hosts them.
    UpperBodyModelNodes upper_body_nodes() const;
    void write_upper_body_nodes(std::span<const std::optional<std::array<float,12>>> rows,const std::optional<RootVector>& root_translation);
    // The secondary component's update (virtual +0c of actor +3024, 001cb160),
    // which 00392aa0 runs after the model update and the physical step of the
    // second traversal. Empty: no secondary update (the default).
    void set_secondary_update(std::function<void(float dt)> update);
    // The combat stages of the player component's pad producer 0024b5d0 (a
    // combat host fills them; empty: skipped): `targeting` after the selector
    // copy (0x0024ce5c 0022c210, 0x0024cec0 00247d60), `weapon` after the
    // movement command (0x0024f1c8..0x0024f810), both before the button tail.
    // The fields are the input component's selector groups (+38 / +3c, +40 /
    // +44) and the button state, whose +bc / +c8 / +cc the Triangle
    // interaction writes (the targeting's recheck latch and R1 pick).
    void set_pad_combat_hooks(PadCombatHooks hooks);
    // Checkpoint 132el2: 0x0024c628..0x0024c67c, with input sp+1c0 (the lift floor chooser shown, 0045bd00),
    // 0045bd30(HUD +6a4, the actor's transform component +40 after its +50 refresh vt+58): the chooser
    // closes once the player strays 3 from where it opened. Empty: not hosted (sp+1c0 then throws).
    void set_choose_menu_track(std::function<void(const RootVector& position)> track);
    // The pad's interact prompt 0x0024ec1c..0x0024eea0 (pad_interact_prompt.hpp),
    // after the camera bits 00247800 and before the weapon block: the actor
    // fills the input and hosts the box selection 00171260(*004ffbd0, 0)
    // (select_interaction_hotbox); the caller runs it on its game screen
    // (HUD +6a4). Empty: skipped.
    using PadInteractPrompt=std::function<void(const PadInteractPromptInput&,
                                               const std::function<std::optional<PadInteractBox>()>& select_box)>;
    void set_pad_interact_prompt(PadInteractPrompt prompt);
    // The actor update's weapon inventory call 00144080 (0x001b1d6c), after
    // the component update in actor logic; skipped while +3269 gates logic.
    void set_inventory_update(std::function<void(float dt)> update);
    // The death action's tail 001977f0(actor, 0) (0018c504) with a held item
    // reads the actor's fields (`input`; empty: that case throws), and
    // 001cb120(+3314, mode) applies the mode (`apply`; empty: recorded only).
    void set_death_secondary_hooks(std::function<SecondaryModeInput()> input,std::function<void(std::uint8_t mode)> apply);
    // Checkpoint 132fx3: the +3524 inventory reads of the channel update 00199f30 (human.cc:9182,
    // 0x0019a3c8..0x0019a43c): `first` the armed test (the player component, virtual +54: 001434d0 any of the
    // eight slots holds an item; an AI component: +84 != 6) and `second` the pose type 00143510 (the held
    // record's +48, 0 with nothing held). Channel 12 (the casual / combat stand, walk and peek sliders) goes to
    // 1 when armed and the pose is not 6; channel 13 (sliderNonCrawl) when the pose is 6. Empty: unarmed (false, 0).
    void set_equipment_hook(std::function<std::pair<bool,std::uint8_t>()> hook);
    // The secondary component +3314 for actor actions 0x40 / 0x41 (the
    // flashbang hide, actor_flashbang_hide.hpp; a combat host links it). A
    // null result is the original's absent component; an empty hook makes
    // those actions throw.
    void set_secondary_component(std::function<SecondaryComponent*()> component);
    // Actor logic's item stages of a combat host (empty: skipped, the
    // default): `triggers` is 00197e00 at 0x001b1d10 (after 00199f30 /
    // 00199980, before the +3200 shift), `senses` the +33a8 / +33ac decay at
    // 0x001b2328..0x001b237c; both skipped while +3269 gates logic.
    void set_logic_item_hooks(std::function<void(float dt)> triggers,std::function<void(float dt)> senses);
    // Actor logic's grip stage of a combat host (empty: skipped, the
    // default): 0x001b1c28..0x001b1cec, after 0019f6c0 and before 00199f30;
    // skipped while +3269 gates logic or without an animation model.
    void set_logic_grip_hook(std::function<void(float dt)> grip);
    // The burning actor of a combat host (actor_burning.hpp); empty members
    // keep the default (the update skipped, the others throw where reached).
    void set_fire_hooks(SoloFireHooks hooks);
    // The receiver's inventory hosting (the release 00141cd0 of embedded code
    // 2, the item tokens 0xa0 / 0xa1 and their launch dependency): the
    // bindings (00489d70 / 00489d78, the part definitions) and the host's
    // inventory members (inventory_owner, the model slot functions, the empty
    // and selection notices, launch_inventory_projectile, null_target_key);
    // the other members of `host` are not used. nullopt: not hosted (the
    // default; those branches throw).
    void set_inventory_receiver(std::optional<ActorAnimationInventoryBindings> bindings,ActorAnimationReceiverHost host);
    // The receiver state for the actor's item functions that run outside the
    // receiver (00197560 / 00197140, 00197e00's item triggers): +3210, +324c,
    // +14 and +33cc..+33e8.
    ActorAnimationReceiverState& item_actions();
    // An animation effect delivered to this actor outside a model delivery
    // (the secondary component's one-shot post 001d7940, immediate): the
    // receiver with an empty delivery view.
    void receive_animation_effect(const MotionEffect& effect);
    // 00132910 on the actor's model context (+2e50): the animation model's
    // skeleton_node_world_position; throws without a model that hosts it.
    RootVector skeleton_node_world_position(int node) const;
    // 00133ed0: the animation model's skeleton_node_world_matrix.
    std::array<RootVector,4> skeleton_node_world_matrix(int node) const;
    const SoloActorControlFields& controls() const;
    SoloActorControlFields& controls();
    const PlayerMoveState& pad_state() const;
    const PlayerQuickTurnState& quick_turn_state() const;
    // Actor priority actions +3210/+3218 and mode +320c.
    const ActorAnimationReceiverState& actions() const;
    // The receiver's view of the +3524 inventory (mode +84, the slots, +a0)
    // from the combat inventory (combat_actor_world.hpp). The combat host's
    // stages inside the actor's own step (the pad producer, 00144080) write it
    // too: the original has one +3524 object that both sides change.
    void set_inventory_view(const ActorAnimationInventoryState& view,std::uint8_t byte_a0);
    const SoloActorStateWords& state_words() const;
    const PlayerButtonState& button_state() const;
    // Player input +0x130.
    std::uint32_t found_ledge() const;
    // Physical +810: the edge slot 6 mask (read by 001977f0 in mode 8 and the hang channels).
    std::uint32_t physical_flags810() const;
    RootParentTransform edge_root_placement();
    // Actor jump fields (+3490, +34a0..+34d0, +30a0/+30a4).
    const ActorJumpState& jump_state() const;
    // Input selector +0x38 and pending priority set +0x40/+0x44.
    std::int32_t pad_selector() const;
    // The player's option bytes +174 / +175 / +176 (input +124 / +125 / +126):
    // manual aim (0x0024e3a8) and free look X / Y (002503e0's a2 / a3). Stored by
    // the level load 00135848 and the options apply 0045a3a0; any other offset throws.
    void set_input_option_byte(std::uint32_t player_offset,std::uint8_t value);
    std::uint8_t input_option_byte(std::uint32_t player_offset) const;
    // Actor byte +2e00 (written by the L1 handler 0024a850 only).
    std::uint8_t aim_view_byte_2e00() const;
    // Ladder: actor +31b6/+3514/+3518, +31d8 and physical +980.
    const ActorLadderExitFields& ladder_exit_fields() const;
    float ladder_slide_amount() const;
    RootVector ladder_reference() const;
    bool published() const;
    // The component translation (actor +50 +40, as +e4 reads it): the physical response's component +14 store,
    // the load placement before the first frame, or, in a step without that store (the setup's tail 0x001e4dd8:
    // physical +12 clear or mode 12; checkpoint 132hud4), the translation the model step wrote (component()).
    RootVector published_position() const;
    // Model context +50 (physical +8 +50): the weighted node centroid the last
    // world entry stored (003a9090), or the constructor's (0,0,0,1).
    RootVector model_centroid() const;
    // Actor virtual +e4 (00132860): that centroid through the component (the
    // published position's translation, the component orientation). Equal to
    // published_position() only while the centroid is (0,0,0,1). Throws
    // before the first physical publication.
    RootVector position_e4() const;
    // Actor virtual +e0 (00133e00): physical +2f0 once the physical tail has
    // run (else the component translation), plus physical +380.
    RootVector position_e0() const;
    // Actor virtual +88 (00184380): actor +2e80, the model context's bounds
    // +30/+40 (skeleton space) as the last world entry's 003a9220 stored them
    // (after the physical step), or the constructor's sentinel (00393760 ->
    // 00183010: minimum +FLT_MAX, maximum -FLT_MAX) before the first.
    Bounds context_bounds() const;
    // 003ae690(actor +2f70, key) +d0: the weight of the "flor" (004a03c8),
    // "edge" (004a03d0) or "ladd" (004a03d8) root.
    float root_weight(ActorRoot root) const;
    // The services 001e7f40 / 001e8760 read on the other actors (the global
    // list 004ea190, the player slots, their virtuals); the actor itself is
    // passed as handle(). Null (the default): the solo stand-in, an empty
    // list and no players. The host must outlive this actor's steps.
    void set_movement_world(ActorMovementWorldHost* host);
    const HotboxWorld& hotboxes() const;
    const HotboxTracker& hotbox_tracker() const;
    // The mission script's part of the message pump (SCRIPT_HOSTING.md
    // section 1.1): 003c8de0 offers each message to the receiver's filter
    // (+0x38, the script context's HotboxMsg handler selection) first and
    // calls the native receiver (+0x0c, 00171b60) only when no handler was
    // selected. The filter gets the box index and the tracker payload (0
    // enter, 2 exit) and returns true when a script handler took the event.
    // Without a filter every event goes to the native receiver.
    using HotboxScriptFilter=std::function<bool(std::size_t box,std::uint8_t payload)>;
    void set_hotbox_script_filter(HotboxScriptFilter filter);
    // The same pump step for the interaction triggers: the receiver (a link
    // key, usually a "no_vum" InteractGOBJ) offers the message to its script
    // filter first; true means a handler took it. The native receivers
    // (00216580 for an InteractGOBJ) are not hosted: an untaken trigger throws.
    using TriggerScriptFilter=std::function<bool(const HotboxTrigger&)>;
    void set_trigger_script_filter(TriggerScriptFilter filter);
    // The native receiver of a trigger no script handler took (00216580 for
    // an InteractGOBJ, interact_gobj_pairing.hpp): true when hosted. Every
    // trigger in this actor's queue carries this actor at +30 (the tracker's
    // actor, or the sender of the CAS code behind a self-sent trigger). Unset
    // or false: the trigger throws as before.
    using NativeTriggerReceiver=std::function<bool(const HotboxTrigger&)>;
    void set_native_trigger_receiver(NativeTriggerReceiver receiver);
    // The CAS code (0xd / 0xe / 0xf) this actor's tokens send an object
    // action's object, at the pump that delivers it, before the self-sent
    // trigger is queued: the native object's state (00216580's CAS branch).
    using ObjectCodeReceiver=std::function<void(std::uint64_t object,std::int32_t code)>;
    void set_object_code_receiver(ObjectCodeReceiver receiver);
    // A trigger message (0021b790) queued to this actor's pump with transport
    // 0, as from a box's +0x114.
    void queue_trigger(const HotboxTrigger& trigger);
    // Port-only (checkpoint 132ob3, omega_play --coop-partner): the session
    // acts as the host of a multiplayer game (005721c8 = 1, 005721c0 = 1) for
    // the Triangle (001714c0's HotboxMsg to a type-12 box) and the box's
    // receiver of that message (001717a0 action 3 -> 00171880). Off by
    // default (a solo game).
    void set_multiplayer_host(bool host);
    // Port-only (checkpoint 132ob5, omega_play --coop-partner, the partner
    // only): in actor mode 16, 001a80e0 marks the navigation arrived (+30ad,
    // 001a84c8..001a84e8) on every frame its ladder request (00476490: 31 / 32)
    // goes out while action 14 (the climb) is clear, i.e. at the mount and at
    // each stop between climb cycles; 00161370 takes each rising edge as a node
    // arrival, so a route steps one node per climb cycle and, once it ends on
    // the ladder (event 6, movement state 0), the AI turns back down; a route
    // that ends at the ladder's top node ends there too as the exit latches
    // (00472e10 type 0: +31b6), and the down request that follows starts a climb
    // down before the body's dismount (00474050). With this set the ladder
    // request gives no arrival: the AI keeps its target until the exit has taken
    // the actor off the ladder, where the stop distance arrives. Off by default.
    void set_port_ladder_arrival_at_end(bool on);
    // The same pump step for the script AI notifications an AI component
    // queued to this actor (queue_ai_notification) and the DamageMsg its
    // base damage 003ccb00 queued: the actor's filter
    // (003cc850 -> 003e1aa0, receiver projected to its controller component) first.
    // When no handler takes one, the actor's native receiver 001b3b60 passes
    // it to 003cea10, which forwards an unknown message to the receiver's
    // child list +0x20 (003caca0); that list starts empty (003cb090 ->
    // 00139070) and stays empty for actors (AddChild 003cac40 is called only
    // by the Game constructor 00137ab0 x5 and the level load 001350e0, both
    // on G), so 003caca0 returns 0: the notification is only recorded
    // (SoloActorFrame::ai_notifications, script_handled false).
    using AiNotificationScriptFilter=std::function<bool(const ActorAiNotification&)>;
    void set_ai_notification_filter(AiNotificationScriptFilter filter);
    // 003c9420(notification, actor, 0): delivered by the next pump.
    void queue_ai_notification(const ActorAiNotification& notification);
    // Checkpoint 132zf8: the body carry (actor_body_carry.hpp) between the
    // actors of one world. The resolver gives the actor of an object handle
    // (null: none, or deleted: 0040c540 / 001adaf0 find nothing); the hooks
    // are the body's backpack (+3464) operations of the combat host.
    struct CarryHooks {
        std::function<bool(std::uint64_t body)> backpack;               // +3464 != 0
        std::function<void(std::uint64_t body)> hide_backpack;          // 00256120(+3464)
        std::function<void(std::uint64_t body)> turn_on_weapon_pickup;  // 001b48f8 -> 001a3250(body)
    };
    using ActorResolver=std::function<SoloActorSimulation*(std::uint64_t handle)>;
    void set_actor_peers(ActorResolver resolve,const CarryHooks* hooks);
    // The pickup menu's kind-3 line (0025220c): the actor message (key
    // 0055d480) action 0x2b, +28 kind, +2c the body's key, queued (transport 0).
    void queue_carry_pickup(std::uint64_t body,std::uint8_t kind_28);
    // 001e8ca0: the root of the relation tree +2fc0 (001ad8c0 links a carried
    // body under its carrier); the actor's own handle when unrelated.
    std::uint64_t relation_root() const;
    // +3478 while its +347c key matches (001adaf0): the carried body.
    std::optional<std::uint64_t> linked_body() const;
    // Hotbox link messages (hotbox_world.hpp). A type-1/6/7 box of this
    // actor's hotbox world sends its links EnableMsg/DisableMsg from its
    // native receiver (00172400 / 00171ed0) during the pump; the sink routes
    // each one to its receiver (an actor, queue_link_message). Without a sink
    // such a message throws. hotbox_load_link_messages are the DisableMsgs
    // the boxes send at load (00172ae0), for the host to route the same way
    // before the first step.
    using HotboxLinkSink=std::function<void(const HotboxLinkMessage&)>;
    void set_hotbox_link_sink(HotboxLinkSink sink);
    std::vector<HotboxLinkMessage> hotbox_load_link_messages() const;
    // 003c9420(link message, actor, 0): delivered by the next pump. The
    // actor's script filter (003cc850 -> 003e1aa0, types EnableMsg /
    // DisableMsg) first, then 001b3b60's single-player branch: EnableMsg
    // activates (+5c(1)) an actor that is not present when its component's
    // virtual +44 (00157a40) holds; DisableMsg deactivates (+60(1)) a present
    // actor whose component's +141 bit 7 (radius-controlled) is clear. The
    // player actor (no AI component) throws.
    void queue_link_message(bool enable);
    using LinkMessageScriptFilter=std::function<bool(bool enable)>;
    void set_link_message_filter(LinkMessageScriptFilter filter);
    // The enter of a type-3 box with links (MakeNPCInteractable's run-time
    // box) sends each link a GOBJTriggerEventMsg (00172598,
    // hotbox_type3_enter_triggers) with transport 0 during the pump; the sink
    // routes each one to its receiver's actor (queue_box_trigger). Without a
    // sink such a message throws.
    using HotboxTriggerSink=std::function<void(const HotboxType3Trigger&)>;
    void set_hotbox_trigger_sink(HotboxTriggerSink sink);
    // A type-5 box's environment switch (00172688 / 001721e0 -> 001781a0 on the controller 004ffbd4), called in
    // the pump; without a sink such a box event throws.
    using EnvironmentSink=std::function<void(const HotboxEnvironmentAction&)>;
    void set_environment_sink(EnvironmentSink sink);
    // 003c9420(trigger, actor, 0): delivered by the next pump. The actor's
    // script filter (003cc850 -> 003e1aa0, type GOBJTriggerEventMsg) first;
    // unhandled, 001b3b60 (no branch for key 004f5440) passes it to 003cea10
    // (none either: only 0055d4b8 / 0055d498 / 0055d4d8 / 0055d4e0), whose
    // 003caca0 walks the actor's +0x20 child list, empty for actors (see
    // set_ai_notification_filter): dropped, recorded in box_triggers. The
    // player actor (its filter is not hosted) throws.
    void queue_box_trigger(const HotboxType3Trigger& trigger);
    using BoxTriggerScriptFilter=std::function<bool(const HotboxType3Trigger&)>;
    void set_box_trigger_filter(BoxTriggerScriptFilter filter);
    // The revive's MSGGAME_NOTIFY_CHECKPOINT (queued at 00189dd8 ahead of CAS
    // 0x11): the controller's script filter (00175fd0 -> 003e1aa0, type
    // NotifyCheckpointMsg) when the pump delivers it. Without a filter, or
    // when no handler takes it, it has no effect.
    using NotifyCheckpointScriptFilter=std::function<bool()>;
    void set_notify_checkpoint_filter(NotifyCheckpointScriptFilter filter);
    // cOutOfAmmoMsg (key 004ea150, vtable 004d91e0, +24 the item) to the
    // player controller (+88 -> +58), transport 0: sent by the discharge
    // 0013ff40 (00140c74..00140cc4) when the clip and the spare are empty and
    // by the throw release 00141cd0 (00141d94..00141de0) when the thrown
    // slot is empty, both only for a controller whose virtual +54 holds
    // outside a multiplayer game (005721c8). The pump offers it to the
    // controller's script filter (00175fd0 -> 003e1aa0, type OutOfAmmoMsg)
    // first; no native receiver tests the key (only the two senders and the
    // key's static init 004c3a98 load 004ea150), so unhandled it has no
    // effect. Throws for an actor without a player controller.
    void queue_out_of_ammo(std::int32_t item);
    // MSGGAME_RESPAWN (key 004f7940, +24 = 0) queued to this actor: game::ReSpawn 0022e1d0 sends it as the
    // fade poll 00177c60 does; delivery is 001b45dc (revive 001898f0, or a live player's dead state 001a3780).
    void queue_respawn();
    // 132ln5h14c: a death action (action 0x42 kind 2) another console sent (003e4f70's 003c9350, transport 0):
    // delivered at the next pump, 001a92a0 case 0x42 -> 0018a950(code).
    void queue_network_death(std::int32_t code);
    using OutOfAmmoScriptFilter=std::function<bool(std::int32_t item)>;
    void set_out_of_ammo_filter(OutOfAmmoScriptFilter filter);
    // cWeaponVisChangeMsg (key 00583968, vtable 004e1140, byte +20 = 2, word
    // +24 the value) to the player controller (+88 -> +58), transport 0: sent
    // by the inventory update 00144080 (00144270..00144314) when the
    // controller's virtual +54 holds and 00142e80's value differs from +a4
    // (no multiplayer gate). The pump offers it to the controller's script
    // filter (00175fd0 -> 003e1aa0, type WeaponVisChangeMsg; its getter
    // Visibility 0043fb40 reads +24) first; no native receiver tests the key
    // (only the sender and the static init 004d7990 load 00583968), so
    // unhandled it has no effect. Throws for an actor without a player
    // controller.
    void queue_weapon_vis_change(std::int32_t value);
    using WeaponVisChangeScriptFilter=std::function<bool(std::int32_t value)>;
    void set_weapon_vis_change_filter(WeaponVisChangeScriptFilter filter);
    // Script Enable/Disable(Hotbox) (0016fcd0/0016fbb0 -> virtual +0x64/+0x68
    // of vtable 004d9ac0 = 0016fd00/0016fbe0): +2c = 1/0. Their multiplayer
    // replication message (005721c8 set) is not sent in a solo game. The
    // tracker sees the change on its next update. Occupied (0016fb50) is
    // hotboxes().boxes[box].occupancy_98 > 0.
    void set_hotbox_enabled(std::size_t box,bool enabled);
    // SetInteractMessage 0016fb70 / ClearInteractMessage 0016fb60: +0xa0.
    void set_hotbox_interact_message(std::size_t box,std::optional<std::string> message);
    // MakeNPCInteractable's run-time type-3 box (add_runtime_hotbox): appended,
    // so the tracker's indices stay; the tracker sees it on its next update.
    std::size_t add_runtime_hotbox(std::uint32_t id,std::uint32_t npc_key,const NpcInteractableBounds& bounds);
    // An object's type-0x12 box (00170e20(*004ffbd0, object, 0, 1), add_object_hotbox),
    // the caller's writes to its +94 (the parameter) and +9c (the priority), and its
    // removal 00170db0 from the global list and this tracker (remove_object_hotbox).
    std::size_t add_object_hotbox(std::uint32_t id,std::uint32_t object_key,const NpcInteractableBounds& bounds);
    void set_hotbox_word(std::size_t box,std::uint32_t offset,std::uint32_t value);
    // 00171050's stores on an object box: +70 / +80 (the x, y, z lanes the
    // containment reads; Checkpoint 132el6, a backpack riding a lift).
    void set_hotbox_bounds(std::size_t box,const RootVector& minimum,const RootVector& maximum);
    void remove_object_hotbox(std::size_t box);
    // 00170570: the run-time boxes (004ee6e0) out of the global list and this tracker.
    void clear_runtime_hotboxes();
    // The object side of a type-0x12 box's code at the player component
    // (001774c8): 0040c540(key) non-null, and the trigger message 0021b790 /
    // 003c9420 delivered at once to it, whose receiver reads and clears the
    // component's +1bc / +17c (the button state). Unset: the code throws.
    using ObjectExists=std::function<bool(std::uint32_t key)>;
    using ObjectTrigger=std::function<void(std::uint32_t key,PlayerButtonState& component)>;
    void set_object_trigger_hooks(ObjectExists exists,ObjectTrigger trigger);
// The armor fields +33a0/+339c as the backpack pickup (WeaponBackpackOps) leaves them.
void set_armor(std::int32_t armor_33a0,std::int32_t max_armor_339c);
// Checkpoint 132zf4: the script stores SetHitPoints (003cc990: +40 = cvt.s.w(hp)) and SetInvulnerable
// (001b8750: byte +33a4), and SetOnFire's 00186360(actor, 1, 0, 0, 0, -1.0) through the fire hooks'
// ignite (fire_ignite throws naming 00186360 when the hooks have none).
void set_health_40(float value);
// Checkpoint 132ln5h4: the network section 001b1710 on a remote human (research/NET_INLEVEL.md 4.15): +0x31b5
// (block +0x50 bit 0x20), the killed byte +0x33a5 (block +0x35) and the physical's +0x12 / +0x13 the action 0x43
// sets (001ad5e0).
bool byte_31b5() const;
void set_byte_31b5(bool value);
void set_killed_33a5(bool value);
void set_physical_12_13(std::uint8_t value);
void set_invulnerable_33a4(bool value);
// Checkpoint 132ms3: game::SetFootstepSoundRadius 00169b70: swc1 f20, +31f8 (the running_world radius).
void set_running_31f8(float value);
float running_31f8() const;
// Checkpoint 132sx8: SetFireInvulnerability 00149010 -> +204 = 0014a7c0: sb a1, +33b8 (read by the
// damage 001874a8, the ignition 00186360 and the area effects).
void set_fire_immune_33b8(bool value);
// Checkpoint 132zf7: byte +3468, cleared by the constructor (001b6d68 / 001b61e0) and written only by
// SetPickupable (00149230 -> AI component +1d4 = 0014a890: sb a1, +3468).
std::uint8_t pickupable_3468() const;
void set_pickupable_3468(std::uint8_t value);
// Checkpoint 132sx10: byte +3469 (the body's disguise), cleared by the constructor (001b61e0) and
// written only by the AI component's +1d8 = 0014ac90 (sb a1, +3469; SetHasDisguise 001491f0 and the
// pickup menu's kind-4 take 00252298). Read by the backpack (001a3748, 00255e2c, 002560bc, 00256840).
std::uint8_t disguise_3469() const;
void set_disguise_3469(std::uint8_t value);
bool fire_ignite_hosted() const;
void fire_ignite();
    // Health fields (+40, +3398, +33a0, ...), the life state (virtual +108,
    // 001872f0) and the stats counter PlayerDeaths (00176540 -> 00333f10(stats, 8, 1)).
    const ActorHealthState& health() const;
    // Port-only (checkpoint 132f5, the Infinite Health cheat's enable): +40 = (float)+3398.
    void port_refill_health();
    // Port-only (checkpoint 132ob7, --coop-partner): MSGGAME_RESPAWN's handler 001b45dc for an AI controller, i.e. the
    // revive 001898f0 at once (teleport to this actor's checkpoint, health +3398, armor +339c). Only a dead actor
    // (action 0x1f set, so the revive's 001a3780 only clears 0x1b) is revived; anything else throws.
    void port_revive();
    const ActorCheckpoint& checkpoint() const;
    void set_checkpoint(const std::array<float,3>& node_position,float node_yaw,std::string label="Checkpoint");
    // MSGGAME_ADD_CHECKPOINT sent with transport 0 (script SetCheckpoint,
    // 0022e080): queued behind the actor's pending messages; the next step's
    // pump (003c8de0 -> 001b3b60) applies set_checkpoint with the health and
    // armor of that moment and reports checkpoint_added.
    void queue_checkpoint(const std::array<float,3>& node_position,float node_yaw,std::string label);
    // game::PerformAction_WithObject (003e6540 -> 003e67c0): the CAS action
    // (key 0x40, kind 7, motion -1) sent to the actor with transport 0, so the
    // next step's pump runs 001a92a0 -> 0018d2c0 (handle_actor_object_action).
    // The object is an InteractGOBJ by its +c key; node is its +a8 node and
    // world_object its +ac (0 without one). The motion tokens' CAS codes to
    // the object are queued; its native receiver (00216580) is hosted only as
    // the self-sent GOBJTriggerEventMsg the script sees: code 0xf as action 2
    // (NotifyDone), 0xd as action 3 (NotifyInterrupted), 0xe with no trigger.
    // The same message from InteractGOBJ pairing 00216230 (actions 0x31 /
    // 0x33 / 0x34 / 0x35 / 0x37 / 0x38: 0018d2c0's buddy-climb branches,
    // handle_actor_buddy_climb_action) and its 0x32 / 0x36 (001a92a0 ->
    // 001ad3a8, handle_actor_buddy_climb_cancel).
    struct ObjectAction {
        std::int32_t action{};
        // The object key (+2c); nullopt: none (-1, game::PerformAction).
        std::optional<std::uint32_t> object;
        std::optional<ActorObjectActionNode> node;
        std::uint64_t world_object{};
        float time{-1.0F};
        // Set: the 003e6910 kind-10 message (4/5-argument forms) with its
        // lowercased name and +64 byte. At the pump, 0018d2c0 (0018d300..
        // 0018d508, before the gate) looks the name up in the model's +24
        // map (0018c740, exact 48-byte compare) and delivers the kind-7
        // message {action, object, id, time, +64} synchronously (003c9420
        // transport 1) in the same pump. An empty name gives id -1 (the
        // action's own motion, as the 3-argument form); an absent one (the
        // original's id 0) throws.
        std::optional<std::array<char,48>> motion_name;
        std::uint8_t byte_64{};
    };
    void queue_object_action(const ObjectAction& action);
    // An action-0x28 request (game::StopPerformingAction 003e6300: kind 2,
    // +2c = 1) queued with transport 0; at the pump 001ad0e8 admits it
    // (actor_cancel_request_admitted) and queues action 0x29.
    void queue_cancel_request(std::uint8_t kind,std::uint32_t word_2c);
    // Game world 004ffb50 active object list (+c4) as the object actions push
    // and the tokens 0x68/0x6a pop it.
    const ActorWorldStack& world_objects() const;
    // Teleport Message (001bc4b0, ACTOR_TELEPORT.md §2) with the native field
    // map of §4.1: roots and model anchors, physical integrator and contacts,
    // model component, jump target, heading history and curve target, and the
    // tail. An active action 10 needs the landing with argument 0, which is
    // not hosted: the call throws before any write. Actions 7/8 and 16 (not
    // 15) run the hang drop and the ladder cancel synchronously.
    SoloActorTeleportResult teleport(ActorTeleportMode mode,const ActorTeleportMatrix& matrix);
    // The calls a melee takedown (00196480, COMBAT.md "Checkpoint 115 melee
    // hit and takedown execution") makes on the attacker's or the victim's
    // model *(+2ec0), the victim's between its steps. Each throws without a
    // model.
    // 0018c5a0(model, name): the node-name map +24's value for the name (0
    // when absent, 0018c740 / 0018c710 / 0018c700), then 0018c6b0 (+3c[id]).
    ActorMotionRegistration resolve_mapped_motion(std::string_view name) const;
    ActorMotionRegistration retained_motion() const;                            // *(+326c)
    void install_motion(const MotionRequest& request,MotionInstallMode mode);   // 003ad500 on +2ec0 +34
    // 0018c9d0(actor, name, code): the name's first 47 bytes, the node-name
    // map +24's value (0 when absent), stored at +34e4[code & 0xff] (the
    // original does not bound the index; a code above 11 throws). The death
    // action (0018a950 at 0018bbc8) uses an override that is not -1.
    void set_death_motion(std::string_view name,std::uint8_t code);
    const std::array<std::int32_t,12>& death_motion_overrides() const;          // +34e4..+3510 (-1: none)
    // Physical +11 = 1 (the integrator reset of the next physical step).
    void request_physical_reset();
    // 00131320(G, object) then G +d8 = 1 with G +d4 = object, as 00131540
    // ends: the object, with its own +2c/+2d/+2e (state; activate / deactivate
    // set / clear its +2d), is pushed on this actor's world_objects and is the
    // pending removal 00131510 that the tokens 0x96 / 0x97 run.
    void push_pending_world_object(std::uint64_t object,const ActorWorldObjectSnapshot& state);
    // 00131510(G) outside the tokens 0x96 / 0x97 (the air taser's release
    // 00263080 of a shock): the pending removal on this actor's world_objects.
    void finish_pending_world_object();
    // The current +2c/+2d/+2e of an object pushed by push_pending_world_object.
    std::optional<ActorWorldObjectSnapshot> hosted_world_object(std::uint64_t object) const;
    // Checkpoint 132ms2: PushCurrent / PopCurrent(GameCamera) 0017a160 / 0017a130: 00131320(G, object) /
    // 00131240(G) on world_objects (the object's +c is its population key). Pop returns the removed object.
    void push_world_object(std::uint64_t object);
    std::uint64_t pop_world_object();
    // Physical +3d0: 1 from the physical constructor (001eb990 at 001ebfa0),
    // cleared by the melee start 00196480 (001965f4 / 0019660c), set by the
    // tokens 0x96 / 0x97 (001a3fcc). The movement pair pass (001e7f40) reads it.
    std::uint8_t physical_3d0() const;
    void clear_physical_3d0();
    // Actor +346a, the stun-baton immunity read by 00141880 (and 002655e0):
    // 0 from the spawn reset (001b61e8); its only local writer is 00198f90
    // (sb a1, +346a), reached from the script native 0014a760 (actor vtable
    // slot +20c via SetNotTaserable 00148fb0) and the network handler 00199228.
    std::uint8_t stun_immune_346a() const;
    void set_stun_immune_346a(std::uint8_t value);
    // The burning byte +326a (health().burning_326a) as the ignition 00186360
    // leaves it (the combat host's, actor_ignition.hpp).
    void set_burning_326a(bool value);
    // The fire damage +33b0 (health().fire_damage_33b0) as the decay 001b2380 leaves it.
    void set_fire_damage_33b0(std::int32_t value);
    // The stun's action message (0055d480, +24 7, +28 2, +2c the code) with
    // transport 1: 001a92a0 case 7 (001a9bc8, actor_stunned_action.hpp) on this
    // actor's priority set and model (0018c5a0 / +326c / 003ad500 as the
    // takedown calls above). Throws without a model.
    ActorStunnedActionResult receive_stunned_action(std::uint32_t code_2c);
    // Tear-gas action 6 writes actor +3539. Choke_NextFrame consumes it in
    // the actor logic after heading and before gait/item triggers.
    void receive_tear_gas();
    // Actor +3088 heading history and the +3030 heading curve.
    const ActorHeadingState& heading_state() const;
    std::uint8_t life_state() const;
    int player_deaths() const;
    // Virtual +100 (00187360 with 003ccb00 and the kill 00186bf0) called
    // between world steps (the outcome's record holds the work) or, inside a
    // world step, from another actor's phase (the melee hit 00141880: the
    // work goes to this actor's frame record; during this actor's own phase
    // it throws). The player component answers +54/+58/+5c =
    // 1/0/0, +78 is 0016f9f0 (nothing), +7c 00176540 (PlayerDeaths, pending
    // bit 11) and +84 0016fa10 (the code unchanged); an AI controller answers
    // 0/1/0 (0012f370, 0015b7e0, 001535d0) and its +78/+7c/+84 are the
    // ActorAiController virtuals. The death message runs the death action at
    // once (transport 1). Presentation (the damage message, sounds, rumble,
    // the Flak Jacket notice) is recorded; LOS requests (type 2), ignition,
    // the unconscious message, queued death messages and the invulnerable
    // kill's refill throw.
    SoloDamageOutcome apply_damage(const SoloDamageRequest& request);
    // D4 respawn state: the world fader (*004ffc38), the 15 ms poll, the
    // revive's RespawnTimer (001ba130) and the actor-logic gate +3269.
    const ActorFader& fader() const;
    // 00131470's fader +c (*004ffc38 + c = app +67, the world pause).
    void set_fader_paused(bool paused);
    // Game_MainUpdate's load-done path 00168ae8 (state 0x0f) at 00168ba0:
    // ActorFader_Start 0025e6e0(*(G+e8), 1, 2.0), the level-start fade-in.
    void start_level_fade_in();
    // The game object's level-end state (game_end_state.hpp) on that fader:
    // game::RequestLevelEnd (0012fa60 kind, 00136a90 at once), the main
    // update's frame-start fade check (00168f10, solo: no remote player) and
    // its frame-end teardown (001690f0; the teardown itself is not hosted).
    // The RespawnTimer acts only in state 15.
    LevelEndRequestResult request_level_end(std::uint8_t kind);
    bool level_end_fade_check();
    std::optional<std::uint8_t> level_end_teardown();
    const GameEndState& game_end() const;
    // The 001314f0(G, -1) -> 00335830 call of every request (GameEndState::end_agent).
    void set_end_agent(std::function<void(bool completed)> end_agent);
    // The G+50/+51 bytes after a native main-update pass (play_native_flow.hpp).
    void apply_game_state(std::uint8_t state_50,std::uint8_t previous_51);
    const ActorRespawnPoll& respawn_poll() const;
    const ActorRespawnTimer& respawn_timer() const;
    std::uint8_t logic_gate_3269() const;

    // World hosting (actor_world.hpp). The actor object and +58 component
    // handles (0x100000 and 0x200000 for a solo simulation).
    std::uint64_t handle() const;
    std::uint64_t controller_handle() const;
    bool player_controller() const;
    // The +58 controller's +54 / +58 / +5c as the damage path reads them: the player component's 1 / 0 / 0, else the
    // AI controller's (ActorAiController::damage_predicates).
    ActorDamageController damage_controller() const;
    // Root object bytes +2c, +2d (present) and +2e; the frame traversals run
    // actor logic and the model/physical phase only when +2c and +2d are set
    // and +2e is clear (0x00133420, 0040f3e0).
    bool eligible() const;
    std::uint8_t byte_2d() const;
    // The world's elapsed time for the next steps (00133340: the EE-timer dt,
    // clamped to [0, 0.05]; about 1/30 in TORONTO1 play on the emulator,
    // EMULATOR_COMPARISON.md). Must be positive and finite.
    void set_step_dt(float dt);
    float step_dt() const;
    // Root object bytes +2c, +2d, +2e as stored.
    std::array<std::uint8_t,3> root_bytes() const;
    // Called by activate (true, after +2d = 1) and deactivate (false, after
    // +2d = 0), before the AI component's part: where 001b53d0 registers the
    // actor with the target manager 004f77c0 (0022c650 at 001b5410) and
    // 001b5190 unregisters it (0022c720 at 001b51bc). Empty: nothing.
    void set_presence_hook(std::function<void(bool active)> hook);
    // The sound manager 004ffb50 for this actor's own sound messages: the
    // animation receiver's tokens (0x0b, the ladder tokens, the contact token
    // 3), the ledge catch's M_SHIMMY_ON (001a22f4), the ladder's F_LADDER_M
    // (00188f00) and the landing water sounds (00189440). Each original site
    // builds 001ca210(desc, bank, name, 0, 0) and 0020f990 / 0020eba0(msg,
    // desc, actor), then 003c9420(msg, 004ffb50, transport): `immediate` is
    // transport 1, else 0. Empty: only the frame's `sounds` record.
    using SoundSink=std::function<void(const ActorAnimationSoundRequest& request,bool immediate)>;
    void set_sound_sink(SoundSink sink);
    // The player's voice lines 0018c770(actor, line): the death action's line
    // 8 or 9 (0018a9d8). The host names and delivers them (game_sound_voice.hpp).
    // Empty: only the frame's voice_lines record.
    using VoiceSink=std::function<void(std::int32_t line)>;
    void set_voice_sink(VoiceSink sink);
    // 0018c770(this actor, line) from outside the actor (the NPC death notice's friendly-kill line).
    void voice_line(std::int32_t line);
    // 0020f990(msg, static descriptor, this actor) and 003c9420(msg, 004ffb50,
    // transport) from outside the actor (the AI's 0014d1a0 ledge-fall sound):
    // the descriptor's bank and name (game_sound_voice.hpp) to the sound sink.
    void post_static_sound(std::uint32_t descriptor,bool immediate);
    // This actor's positional (unowned) sounds: 001ca210(desc, bank, name, 0, 0),
    // 0020f860(msg, desc, position), +20 = 1, 003c9420(msg, 004ffb50,
    // transport): the dead landing's M_BODY_FALL / M_BODY_DROP (0019f1c4, bank
    // 1, transport 0) at the 'flor' placement's +30 (virtual +f4). Empty: only
    // the frame's sounds record.
    using PositionalSoundSink=std::function<void(std::int32_t bank,const std::string& name,const RootVector& position,bool immediate)>;
    void set_positional_sound_sink(PositionalSoundSink sink);
    // The timed event 004da870 (actor_hanging.hpp actor_timed_event): the
    // landing and the ledge catch draw its delay from the game RNG and hand
    // the host the delay (the record's +0 on the script callback list's clock, 003ea1d0) and the fire
    // (001b95c0: 00188fb0(actor, 1, 1) or M_SHIMMY_ON with transport 1). The
    // draw is made with or without a sink; empty: the frame records the count.
    using TimedEventSink=std::function<void(std::int32_t delay,std::function<void()> fire)>;
    void set_timed_event_sink(TimedEventSink sink);
    // Walking footsteps and the dry landing branch (research/GENERATOR_RUNTIME.md
    // "Checkpoint 119m"). With a material table the actor runs the gait
    // 00197e00 before its item triggers (owning +31dd and +351c), records the
    // floor probe's surface (001e1b70: physical +141, the material index the
    // COL fixup 003c20c8 writes) and 00188fb0's dry branch: the foot probe
    // 001e9710 and the row's sound, owned (the sound sink, transport 1) or
    // positional at the probe point (`positional`: 0020f860, posted with
    // transport 0). Empty table: the dry branch stays silent as before.
    // ffect (Checkpoint 131c): 00232460(id, frame) for a property id that
    // 004089f0 resolved (actor_footstep_effect_frame); without it such an id throws.
    struct FootstepHooks {
        std::shared_ptr<const MaterialPropertyTable> materials;
        std::function<void(std::int32_t bank,const std::string& name,const RootVector& position)> positional;
        std::function<void(std::int32_t id,const std::array<RootVector,4>& frame)> effect;
    };
    void set_footstep_hooks(FootstepHooks hooks);
    // Actor +14 (1 for every locally constructed actor, 003cb0e0).
    std::uint8_t byte_14() const;
    // Checkpoint 132ln5h14c: a network game's actor +14 (0 while another console owns the actor: the remote NPC
    // controller's install 001b3d98 / a remote human's).
    void set_byte_14(std::uint8_t value);
    // Checkpoint 132ln5h14c: a network game (005721c8; host: 005721c0) for the damage path 003ccb00. Its damage messages
    // other than the single player's (+20 5) go through `send` (003c9420 to the network), which answers whether
    // 003c9420 queued the message on this console too (003ca0e0); a queued one reaches the actor's pump as before.
    void set_network_game(bool network,bool host,std::function<bool(const ActorDamageEvent&)> send);
    bool network_game() const;
    // Actor byte +346d: the water support flag 001e5cf0 publishes (001e5ea8).
    bool in_water_346d() const;
    // Actor virtual +5c (001b53d0): 001b5560,
    // +2d = 1, the centroid weights 001af190 (see human_node_collection), the
    // presence hook (0022c650), then the +58 component's Activate. 001b5560
    // creates +3310 / +3314: the constructor state stands for the first
    // activation's; after a deactivation, and for an NPC that is already
    // present (001b53d0 has no +2d test; the old components leak), the physical
    // component is reset to the 001eb990 state (for the player, +14 set and
    // its controller the player, then +3310 +14 = 3; the +3314 +8 = 2 store has
    // no field here). A present actor with a water height throws: its leaked
    // water effect is not hosted. The presence hook
    // dedupes like 0022c650 and the AI's Activate returns while +159 is clear.
    // With an activation hook (required after a deactivation or when present)
    // its result is +31b2 (001977f0 at 001b571c) and +3524 +a0 = 0 (001b5730).
    // 001af460 and +3268 = 1 are not modelled.
    void activate();
    // Actor virtual +60 (001b5190): +2d = 0, the presence hook (0022c720),
    // the deactivation hook (00185d40(actor, 0..2, off) at 001b51d8; required
    // when an activation hook is set), the component's part (003928d0), then
    // 001e98f0: the water effect of a supported physical component is removed
    // (002b2330). The model detach (0038cbf0) has nothing to detach in solo.
    // effect_slots false: the network game's actor virtual +24 (001b4970), the
    // same body without the effect slots (and its component part 003928a0 is
    // 003928d0's twin).
    void deactivate(bool effect_slots=true);
    // The +3314 recreation with 001977f0(actor, 0) (returns +31b2), and the
    // effect slots off of 001b5190 (see activate / deactivate).
    void set_activation_hook(std::function<std::uint8_t()> hook);
    void set_deactivation_hook(std::function<void()> hook);
    std::uint8_t byte_31b2() const;
    // The messages queued to this actor for the next pump (transport 0).
    std::size_t queued_messages() const;
    const ActorNavigationTarget& navigation_target() const;
    // Action 0 delivered at once (transport 1) to 001a92a0, single-player
    // branch. Kind 0xd (1a99a8..1a9a10): +30b0 = target, +30c0 = yaw,
    // +30d0 = (-sinf(yaw), cosf(yaw), 0, 0) with the executable's sinf/cosf
    // (00121928 / 00121688), +30e0 = speed, +30ac = 1.
    void receive_navigation_request(const RootVector& target,float yaw,float speed);
    // Kind 0xc (1a9330..1a9388): the control fields +31c0 = direction, +31d0 =
    // speed, +31d4 = turn and +31b7 (pose byte 2: 001ae3b0 computes it from
    // the latch +303c, speed 0 and |turn| > 1e-4; otherwise 001ae430 stores
    // pose byte == 1), then +30ac = +30ad = 0.
    void receive_navigation_stop(const RootVector& direction,float speed,float turn,std::uint8_t pose_byte);
    // An actor action message (key 0055d480, kind 0) from the +58 component to
    // 001a92a0: immediate (transport 1) or queued for the next pump (transport
    // 0). Hosted: the pad-action set of handle_actor_pad_action (0x0f, 0x10,
    // 0x1a, ...); others throw on delivery.
    void receive_actor_action(std::int32_t action,bool immediate);
    // The AI dispatcher's path-link running jump (00151d30 link types 5 / 0xd
    // / 0xc): the CAS action 0x15 / 0x16 of kind 4 (vtable 004e0140) with the
    // point +30, queued (transport 0); the pump hands it to 001914c0.
    void queue_running_jump(std::int32_t action,const RootVector& payload);
    // 001b0e88..0ebc (from 001b0a50): physical +330 = +320 = +310 = 0 and
    // +300 = +2f0.
    void stop_physical_motion();
    // Roots +3290/+3294/+3298 (flor, edge, ladd) without a parent (+b0).
    bool roots_unparented() const;
    // Actor words +3030/+3034/+3038 (shifted by 001b2680/001b2670 in the curve
    // tail of actor logic); 00199f30 reads +3034 bit 0.
    const std::array<std::uint32_t,3>& control_history() const;
    void set_control_flags_3038(std::uint32_t bits);
    // 001b0ef0: 0019e670(actor, 1), the dead landing of the AI's ground snap
    // (a1 = 1 skips the physical +dc test); with actions 0x1d / 0x1e it
    // installs the landing pose and, for the AI controller, +114 = 0.
    void land_dead_ground_snap();
    // 001b0ed4: 001a0b30(actor, 0), the AI ground snap's landing for an actor in action 10 (the retained
    // motion reset, as the teleport tail's).
    void land_reset_ground_snap();
    // Actor +303c, the stationary latch the final heading stage writes.
    std::uint8_t stationary_latch() const;
    // The world frame counter 005383c8 as this actor sees it.
    std::uint32_t frame_counter() const;
private:
    friend class ActorWorld;
    SoloActorSimulation(SoloActorWorldLink link,const SoloActorConfig& config,SoloActorAnimation* animation,
        std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial,const SoloActorController& controller,bool present);
    // One world update over the given actors in insertion order: the pump of
    // each actor's queue, the first traversal (+4c then, when eligible, the
    // actor logic), the second traversal (model, gather and physical frame of
    // every eligible actor) and the frame-update event (hotbox trackers).
    // services: the world's child services of the frame-update broadcast
    // (00136a90 forwards it to its children through 003caca0 before the
    // object loop 00133420), e.g. the AI route manager's update 00163ad0; run
    // after the pump and before the first traversal. `after_first_traversal`
    // runs the traversal array's entries registered after the actors (the
    // level's generators) before the second traversal.
    // `appended` gives, one at a time until nullopt, the actors registered
    // during the frame (0040b790 appends to the traversal array; a generator's
    // spawn 0021e620). The first pass rereads the array end
    // (00133718..0013375c), so each is visited in this pass after the later
    // entries: +4c, then the logic through the fresh three-byte gate
    // (ACTOR_FRAME_SERVICE.md, checkpoints 95-96). They missed this frame's
    // pump snapshot and join the second traversal and the frame-update event
    // after the initial actors. Frame and input must stay valid for the call.
    // `traverse` (empty: every actor) marks the actors still in the traversal
    // array; an actor flushed out of it (0040b720 -> 0040b4b0) but not yet
    // deleted still receives its queued messages, since the pump 003c8de0
    // delivers to the receiver pointer whenever its +4 holds the object magic
    // (003c9160..003c919c), while both traversals and the frame-update event
    // skip it.
    struct AppendedActor{SoloActorSimulation* actor{};SoloActorFrame* frame{};const SoloActorFrameInput* input{};};
    // Checkpoint 132ws9: ResetNpc's rebuild 001b8130 from a script native that the pump runs (the revive's
    // NotifyCheckpointMsg -> ITALY / KYRGSTAN m::ReStartLevel). The new object takes the old one's place in this
    // frame's traversal (the original rebuilds at the same address, which the traversal then reaches): its frame
    // slot, empty again, and its input. The old actor must not be the one whose phase runs.
    static void replace_in_step(SoloActorSimulation& old,SoloActorSimulation& rebuilt,SoloActorFrame& frame,
        const SoloActorFrameInput& input);
    static void run_frame(std::span<SoloActorSimulation* const> actors,std::span<const SoloActorFrameInput> inputs,
        std::span<SoloActorFrame> frames,const std::function<void()>& services={},
        const std::function<void()>& after_first_traversal={},
        const std::function<std::optional<AppendedActor>()>& appended={},
        std::span<const bool> traverse={});
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
