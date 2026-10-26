#pragma once
// The combat stages of a play session over an ActorWorld (research/COMBAT.md,
// "Checkpoint 54"): what omega_play --combat runs around ActorWorld::step.
// Owns the CombatActors adapter (ActorWorldCombat), the CombatWorld
// (inventories, fire requests, bullets), the target manager's visibility
// cache (004f77c0, TargetVisibility) with its registration list, the HUD
// target markers (HUD +90) and the local player's input component fields
// read by the pad producer's combat stages.
//
// Frame order (the original world update, with the session's boundaries):
//   begin_frame  the fire requests queued last frame (001a92a0 -> 001960c0 in
//                the message pump) and the inventory views published;
//   ActorWorld::step, inside which per actor
//     the pad producer 0024b5d0 of the local player: 0022c210 (HUD markers)
//       and 00247d60 (targeting, its aim/look actions delivered at once to
//       001a92a0, the aim accumulator 0022f5b0) after the selector copy; the
//       weapon block 0x0024f1c8..0x0024f810 (fire request, transport 0);
//     the inventory update 00144080 at 001b1d6c and the +352c decay
//       (001b2560) of actor logic;
//     the secondary component's update 001cb160 after the physical step;
//     activations / deactivations: 0022c650 / 0022c720 (presence hook);
//   end_frame    the bullet manager 00275670, the HUD update 00273b40 and the
//                frame counter 0055a818 (00165610) incremented.
// The order of the bullet manager, the HUD update and the counter increment
// relative to each other within the original frame is not established; the
// session's order is the one above.
//
// Host facts the session supplies (static readings, COMBAT.md "Checkpoint 54"):
// actor virtuals of vtable 004da690: +bc (00184af0) node 9 of a present actor,
// +c4 (00184b90) 0 when dying or dead else 2, +cc (00188d30) 0.3 / 0.5 / 0.5 /
// 1.0 by subtarget & 3, +d0 (00184be0) 1, +d4 (00184bf0) both secondary states
// fire-ready; controller +74 (0022f510) the physical +310 velocity of an
// actor with a physical component, else the zero vector 004e1d90; teams
// (controller +68: player 00175340 = 0 in single player, AI 0015b800 = +54)
// through IsEnemyTeam +6c (0022f440, both controller classes); 001b33d0 =
// 1 + (0.2 - 1) x (+352c / 5); 001b3400 adds the shake to +352c (at most 5)
// and calls 001ca7d0 on the secondary component; +3470 is 001b31b0's copy of
// +346f or +346e (actor_goggles.hpp; +346e is the local player's reticle return 00241ec0, 0 outside the aim view,
// and +346f is written by the IR switch 00199590 of the goggles one-shot); +346c is 0 (written only by the
// multiplayer message cNetSetFlagMsg, 00199190); +33a8 is written only by
// the area flash 00186830: 0 without the throw flow, else the flow's senses
// (CombatThrows, read by 0022f5b0, 0022b660 / 0022ad30 and 0016c260); a flashlight object +33ec exists for the
// actors built with the inventory object (roles 1 / 3, 001b5fe0 -> 0045bad0)
// with its lit spot (a base world object, ID category 0xe7); the local
// player's D-pad Right block (0024c980..0024ca44) runs in the targeting
// stage and toggles it (00199770 / 00199860), and the update 0045ae70 runs
// in the inventory stage while it is on (flashlight.hpp). Terrain objects (vtable 004d9be0) take the
// base damage 003ccb00, which in single player changes nothing while their
// +40 is not positive: 003cf0e0 stores +40 = 0 and neither 00395dd0 nor
// 00174500 writes it.
// Not hosted (throws): the aim view's movement (0024e1d0, SoloActorSimulation)
// and its pad-block camera distance (sp+1f0, 0024bdc0), the arc preview
// 0025cdc0, hold class 1 without
// CombatPlayConfig::throws, hold class 7 (combat_throw_play.hpp), a static
// hit on a non-terrain object, the eye +bc of an actor that is not present,
// 00263080 (the
// inventory object's +8 state), multiplayer. The aim view's lock 00249b10,
// thrown point 002497c0 and shot frame 00176400 read the camera +50 transform
// CombatPlayConfig::aim_view_camera and the aim point 00249670 its +da0
// (aim_view_camera_da0; absent: they throw).
// The reload action 0x3c (the AI's send and a discharge's reload_action,
// transport 0) is pumped in begin_frame with the other queued actions
// (001aa048 / 0013fb90, actor_reload.hpp); its one-shot completes through the
// throw flow's playback (CombatPlayConfig::throws; absent: it throws), and
// 0013fd20's TransferAmmo HUD notice (003fdb50 / 00128bd0 / 00242b00) throws.
#include "actor_reload.hpp"
#include "aim_view_targeting.hpp"
#include "combat_actor_world.hpp"
#include "combat_throw_play.hpp"
#include "coop_partner_kit.hpp"
#include "flashlight.hpp"
#include "hud_callouts.hpp"
#include "hud_markers.hpp"
#include "hud_targets.hpp"
#include "hud_script_state.hpp"
#include "npc_world.hpp"
#include "player_camera.hpp"
#include "player_targeting.hpp"
#include "player_weapon_input.hpp"
#include "play_loadout.hpp"
#include "skeleton.hpp"
#include "target_visibility.hpp"
#include "team_relations.hpp"
#include "weapon_aim.hpp"
#include "weapon_backpack.hpp"
#include "backpack_object.hpp"
#include "hotbox_world.hpp"
#include "item_inventory.hpp"
#include "material_properties.hpp"
#include "pickup_menu.hpp"
#include "pickup_menu_play.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {

class IeScreen;
struct PickupMenuPreview;

// The model data of a character for the combat adapter: `parts` are the
// decoded part meshes (LOD 0 / LOD 1) in the character's part order with the
// part flags 0 (the loader stores 0, or 1 / 2 / 3 for preregistered LOD 0
// meshes, none of which sets 0x80 or 0x1c: COMBAT.md C1d "Part codes"); the
// inverse binds are skeleton_bind_pose's (their bit-exactness against the
// original +d0, 003ba730, is not established); with a hard mesh the attach
// slots are 0038e100's (hard_mesh_attach_points with the bone scales, node
// +110; empty: the identity). Without a hard mesh the slots stay unbuilt and a
// shot from that character throws (the default frames are not ported).
CombatActorModel make_combat_actor_model(const SkeletonDefinition& skeleton,std::vector<HitVolumePart> parts,
    const SkinnedMesh* hard_mesh,std::span<const SkinMatrix> bone_scales);

// A terrain object's collision resource (geometry handle = owner handle in
// SoloCollisionWorld) and its +c (the packed TER identity the terrain
// constructor 00174500 stores; only compared with itself by the bullets).
struct CombatTerrainObject {
    std::uint64_t handle{};
    std::int32_t id{};
};

// The level services the hosted backpack object needs (Checkpoint 121g,
// backpack_object.hpp): 0040c240 for its ID (0xf5) and its hotbox's (0xf4),
// the regions 00224ac0 / 002248a0 of the placement 002561d0, its downward
// line of sight with *004f5468, and the gearpack model's +3e0 / +3f0 box
// (00253fb0, gun_model_load.hpp) the hotbox 00170e20 bounds read.
struct CombatBackpackWorld {
    std::function<std::uint32_t(std::uint32_t category)> allocate_id;
    std::function<std::optional<BackpackRegion>(const RootVector& p)> containing_region;
    std::function<std::optional<BackpackRegion>(const RootVector& p)> nearest_region;
    std::function<std::optional<BackpackLosHit>(const RootVector& origin,const RootVector& displacement)> line_of_sight;
    std::uint32_t game_class_5468{};
    RootVector model_minimum{},model_maximum{};
    // Checkpoint 132el6: a lift hit by the placement's line of sight takes the
    // backpack's handle (002563f8..00256444 -> ElevatorGOBJ::attach_rider).
    std::function<void(std::uint32_t lift,std::uint32_t bp,std::uint32_t id)> attach_rider;
    // The destroy 00254770's 003cb230(bp, 0.1): `fire` runs when the deferred
    // release fires (003cb4f0 -> 0040bf50: the backpack is no longer in the
    // tree, so its destructor runs and the base destructor 003cad70 writes +c
    // = 0xf0fffff0). Unset, a rider handle to a destroyed backpack throws.
    std::function<void(std::uint32_t bp,std::function<void()> fire)> release_3cb230;
};

struct CombatPlayConfig {
    std::size_t player{};                                        // the local player's actor index
    std::vector<std::int32_t> object_ids;                        // actor +c per world index
    std::vector<std::optional<CombatActorModel>> models;         // per world index
    // 00144dc0's with_object per actor (roles 1 and 3, 001b6f70); the same
    // roles get the flashlight object +33ec (001b5fe0).
    std::vector<bool> with_object;
    // Actor +3588: the population record's two item words (001b6678..001b6724;
    // 0xffffffff: none), added with 00143df0(inv, id, 1, 0, 0, 0) in single
    // player and followed by 00143c50. nullopt: the actor has no record words.
    std::vector<std::optional<std::array<std::uint32_t,2>>> record_items;
    // The controller's team (+68) per actor, read when it is needed (SetTeam
    // changes it).
    std::function<std::int32_t(std::size_t actor)> team;
    const TeamRelations* relations{};                            // *004ee620
    // 0x0024f928: 00241ec0 on the HUD *004ffc2c (aim 1e0, far 200, the held
    // record, the goggles record's +130 while +346f is set): the HUD side of
    // the reticle switch, returning +346e. Empty: the index and return alone
    // (pad_crosshair_346e), no HUD.
    std::function<std::int8_t(bool aim_1e0,bool far_200,const std::optional<HudCrosshairRecord>& held,
        const std::optional<std::int32_t>& goggles_130)> crosshair_hud;
    // 001b31b0's local-player HUD calls, applied as they happen (Checkpoint
    // 132cp4): hud_mode 002609e0(*004ffc30, mode, 0) (the vision effects'
    // 0025fbb0), hud_vision 00241e30(*004ffc2c, mode == 1) and vision_flag
    // (0053b540 = mode == 1). Empty: a player vision change throws.
    std::function<void(std::size_t actor,const GogglesEvent&)> vision_hud;
    // The aim view's HUD calls on *004ffc2c, applied as they happen: 00249b10
    // and 002497c0's events, and 00247e84's 00244960(0.0) with nothing held.
    // Empty: they are only recorded in CombatFrame::aim_view_hud.
    std::function<void(const AimViewHudEvent&)> aim_view_hud;
    // The R1 lock-on HUD calls on *004ffc2c, applied as they happen (Checkpoint
    // 121n): 0022c210's 00244280 (one hud_none_280 per call), then 00247d60's
    // hud_changed_240 / hud_target_380 / hud_none_280 / hud_aim_2e0 in order.
    // Empty: they are only recorded in CombatFrame::hud_none / targeting.
    std::function<void(const TargetingEvent&)> lock_hud;
    // Port-only co-op HUD (checkpoint 132hud5; play_main turns it on with --coop-partner unless --no-coop-hud):
    // the HUD targets' controller virtual +54 (0022c210's always-marker, 00274260's player colour 004ffdc0)
    // answers true for an actor in a port-only agent slot 1..3 (ActorWorld::in_agent_slot), as the original's
    // second player's component (vtable 004d9cf0) does in a co-op game. Off: only the player's own.
    bool coop_hud{};
    // 00273890's listener +c (the HUD map's 0026c8d0) with a new marker's ID and +10 (empty: none).
    std::function<void(std::int32_t id,std::uint32_t state)> hud_marker_added;
    // Agent-data unlocks (00332f90 0, 1 and 2) and IncreasedAmmo (5): explicit
    // (unlock 1, the dual pistols, is read by the backpack pickup 00254da0).
    std::optional<bool> agent_unlock_0,agent_unlock_1,agent_unlock_2,increased_ammo;
    std::vector<CombatTerrainObject> terrain;
    // The population objects (GOB) a bullet can end on (bullet_static_los: the
    // model COL's owner): their +c (nullopt: not one) and their damage virtual
    // +100 (003ccb00 with category 1, ActorWorldBlastObjects::object_damage).
    // Absent: a bullet ending on one throws.
    std::function<std::optional<std::int32_t>(std::uint64_t object)> object_id;
    std::function<void(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount)> object_damage;
    // The local agent record's statistics (001314f0(004ffb50, -1),
    // agent_statistics.hpp) at the start of the session: the NPC death notice
    // adds its kill statistics and reads 0x19 (the level's friendly kills).
    // A new game's record is all zero (00336700 -> 003361e0); nullopt throws
    // when a kill by the player is noted.
    std::optional<AgentStatistics> agent_statistics;
    // *0055a818 when the session starts. New pair entries are zero, so every
    // start value >= 50 (the largest max_age passed to 0022cb10) gives the same
    // answers; smaller values throw.
    std::int32_t frame_0055a818{-1};
    ToHitSettings to_hit{};                                      // game::ToHit (0048ac80)
    bool effects_not_drawn{};                                    // ActorWorldCombatConfig::effects_not_drawn
    std::function<std::int32_t(std::int32_t index)> effect_table;   // ActorWorldCombatConfig::effect_table (Checkpoint 131b)
    std::function<std::uint32_t(std::size_t actor)> scene_cull_38;  // ActorWorldCombatConfig::scene_cull_38 (Checkpoint 122f)
    std::function<float()> frame_rate_1e8;                          // ActorWorldCombatConfig::frame_rate_1e8 (Checkpoint 122f)
    // The level's lights (00535d20) for the torch light (ActorWorldCombatConfig::lights;
    // also CombatPlaySession::set_lights); nullptr: holding a torch throws.
    LevelLights* lights{};
    // Checkpoint 131: the weapon model library (record +13c / +11c, preloaded
    // by the host). With it the combat world keeps the actors' attachment
    // tables (CombatWorld::set_weapon_models), the shot frames add the
    // attached model's muzzle offset and the loadout's stand-in attach
    // events become the records' models; nullptr: models are not hosted.
    WeaponModelLibrary* weapon_models{};
    // Checkpoint 131c: the surface material table 0053bb90 (with its effect and
    // decal ids resolved by 004089f0). With it a bullet ending on a static
    // object carries the 00276460 presentation (BulletEvent::impact): the row
    // 00189560 reads through the hit triangle's COL surface byte 1 (003c20c8);
    // nullptr: static impacts carry no presentation.
    const MaterialPropertyTable* materials{};
    // The backpack an NPC's dead state drops (001a3520 -> 00257640,
    // "backpack.cc", vtable 004dc350) is not simulated: set, each drop is
    // recorded as a labelled diagnostic (CombatBackpack: the weapons and the
    // armor 00255af0 / 00255a50 put into it, in order, and the contents the
    // native 00255af0 / 00255a50 build) and the object gets no model,
    // placement (001a32a0 / 002561d0), hotbox (00170e20: its bounds 00253fb0
    // come from VU0 micro code at model load), update or pickup; unset, a
    // drop throws.
    bool backpacks_not_simulated{};
    // The backpack object hosted (Checkpoint 121g; backpacks_not_simulated is
    // then not read): the constructor 00257640 with its placement and owner,
    // the contents, the hotbox 00170e20 as a type-0x12 box of the player's
    // hotbox world, the update 002568f0 (end_frame), the destroy 00254770,
    // and the pickup: the player component's code 0x12 (001774c8) sends the
    // trigger *004f5440 to the backpack's receiver 00254600 -> 001762c0 ->
    // PickupWpn 00254da0 (inventory add, ammunition merge, armor), whose
    // HUD messages (003fdb50 / 00128bd0 / 00242b00) and sounds (0020f860)
    // are recorded in CombatPlayFrame::pickups.
    std::optional<CombatBackpackWorld> backpack_world;
    // The player has a followed camera (play_main): set, the pad's field of
    // view 0x0024f968..0x0024fa08 (00142f20, weapon_field_of_view.hpp) runs
    // and SoloActorFrameInput::camera_fov_c64 must be given; unset (sessions
    // without a camera), the stage and its +c64/+c68 write are not run.
    bool camera_field_of_view{};
    // The throw flow (C6b, combat_throw_play.hpp): set, the session builds
    // CombatThrows over its actors (the throw start, the item triggers, the
    // one-shot playback, the release, the thrown items after the bullet
    // manager, the senses +33a8 / +33ac); unset, a hold-class-1 request throws.
    std::optional<CombatThrowConfig> throws;
    // game::HitInnocentNpcChance (*0048a2b8), read by the bullet rule per hit
    // (00188918). Empty: its boot value 0.3.
    std::function<float()> innocent_hit_chance;
    // The followed camera's +50 transform (its +10 matrix), read live by the
    // aim view (selector +38 = 8) lock 00249b10, thrown point 002497c0 and
    // shot frame 00176400: PlayerCameraState::transform_50 in omega_play.
    // Absent: the aim view throws.
    std::function<ShotFrame()> aim_view_camera;
    // The camera's +da0, the copy of +50 the update makes before 00280d90
    // rotates +50 with +cf4 bit 3, read by the aim point 00249670:
    // PlayerCameraState::world_matrix in omega_play. Absent: it throws.
    std::function<ShotFrame()> aim_view_camera_da0;
    // Checkpoint 132ln5h: a network game (byte 005721c8 set): the player's record writes into its network block
    // (DME/dme_game.hpp): the targeting's 002494e0 / 00249420 / 00249490 (PlayerTargetingHost::record), the
    // shot 00249530 (0x0024f778) and the weapon 002493c0 after a switch (0x0024cd24, with the held item). Unset:
    // single player (005721c8 clear; the targeting and the switch do not record).
    std::function<void(const TargetingRecord&)> net_record;
    std::function<void()> net_shot;
    std::function<void(std::int32_t item)> net_weapon;
    // 003fdb50 (common pool, then the mission pool) for the reload's
    // TransferAmmo notice (0013fdb8); nullptr: a notice throws.
    const Localization* localization{};
    // Checkpoint 132t: the Triangle pickup menu 00253070 (pickup_menu.hpp)
    // that the backpack receiver 001762c0 builds on the HUD object's screen
    // *(*004ffc2c + 0x54) (HudObject::screen in omega_play; null: opening
    // the menu throws naming 00253070), the pickup text colour block
    // 004f7e70 (SetPickupTextColor) and the local agent record's name
    // (00336170, the menu's "inventorytextpanel"; nullopt: it throws).
    IeScreen* pickup_menu_screen{};
    std::array<float,4> pickup_text_colour{};
    std::optional<std::string> player_name;
    // Checkpoint 132zf5: the mission items (ITEM_INVENTORY.md). `item_list`
    // gives an actor's +3528 list by its population ID (object_ids), the
    // lists AddItem / PlayerAddItem fill; `item_icon` is the table row's
    // +28 handle (*(*00587ff8 + item * 0x2c + 0x28), 004703b0) and
    // `item_name` 00470270 (003fdb80 of the row's +0; nullopt: null). Unset,
    // every +3528 list is the empty one 001b6a60 builds (004704f0 = 0) and
    // a kind-5 menu call throws.
    std::function<ItemInventory&(std::int32_t id)> item_list;
    std::function<std::uint32_t(std::int32_t item)> item_icon;
    std::function<std::optional<std::string>(std::int32_t item)> item_name;
};

// A dropped backpack as the session records it (not a simulated object).
struct CombatBackpack {
    std::size_t actor{};                                        // the dead NPC
    std::uint64_t handle{};                                     // session-local identity (its +3464)
    std::vector<std::array<std::int32_t,3>> weapons;            // 00255af0: item, spare, clip
    std::optional<std::array<std::int32_t,2>> armor;            // 00255a50: armor, max armor
    // Its contents as the native 00258330 / 00255af0 / 00255a50 build them
    // (weapon_backpack.hpp); without CombatPlayConfig::backpack_world the box
    // block is recorded as box_not_simulated.
    WeaponBackpack object;
    std::vector<WeaponBackpackEvent> events;
    // With CombatPlayConfig::backpack_world: the object's address (= handle),
    // its transform component +50, the life-cycle events (construct, owner,
    // destroy), the hotbox (the player's hotbox index), whether 0040c540
    // finds it (0040c9d0 .. 0040c9a0) and whether 00254770 ran.
    std::uint32_t address{};
    BackpackTransform transform;
    std::vector<BackpackObjectEvent> object_events;
    std::optional<std::size_t> box;
    std::optional<NpcInteractableBounds> box_bounds;            // 001710a0's bounds of the box
    bool registered{},destroyed{};
    // Checkpoint 132el6: the deferred release of the destroy fired (the
    // destructor ran: +c = 0xf0fffff0, 003cad70).
    bool deleted{};
    // A population INV: crate (00257f30 at load): no dead NPC (`actor` unused),
    // a zone member (+2d through the zone tracker).
    bool crate{};
};

// Checkpoint 132bp: a CrateInteractMsg the pickup menu sent its backpack
// (key 004f7ea0, vtable 004dc480: +20 flags, +24 Taken 002587e0, +28 Given
// 00258800, +2c Who 002587d0 = the player's actor): Event_Send 003c9420
// (msg, bp, 0) queues it for the next pump (the script handler 003c8de0
// first; unhandled, 00254600 -> 003cea10 -> 003caca0 over the crate's +20
// child list, which is empty).
struct CombatCrateMessage {
    std::size_t backpack{};                                     // index into CombatPlaySession::backpacks()
    std::uint32_t address{};
    std::int32_t taken{},given{};
    std::uint8_t flags_20{};
    // Checkpoint 132pw1: Who (+2c) when the taker is not the player (the PORT-ONLY CPU partner's actor index).
    std::optional<std::size_t> who;
};

// Checkpoint 132zf5: the InventoryMsg the kind-5 take (00251920 ->
// 002523b0) sends its backpack (key 004f7ee0, vtable 004dc460: +1c 0x40,
// +20 byte 2, +24 Taken 00258420 = the item, +28 Count 00258400, +2c Who
// 002583f0 = the player's actor): Event_Send 003c9420(msg, bp, 0) queues it
// for the next pump like CombatCrateMessage.
struct CombatInventoryMessage {
    std::size_t backpack{};                                     // index into CombatPlaySession::backpacks()
    std::uint32_t address{};
    std::int32_t item{},count{};
};

// One trigger *004f5440 a backpack received from the player (001774c8 ->
// 00254600 -> 001762c0): the backpack's events (00254da0's display names,
// strings, formats, HUD messages, sounds, box words), the inventory events
// of the player's adds, the HUD message texts in order (localized as
// 003fdb50 / 00128bd0 "%s %s" build them) and the armor afterwards.
struct CombatPickup {
    std::size_t backpack{};                                     // index into CombatPlaySession::backpacks()
    // What ran: "001762c0" (the trigger), or the pickup menu's "00251920"
    // (Cross), "00251370" / "00251500" (Left / Right), "002526f0" (the
    // per-frame update), "00252fb0" (the close: 00255f10).
    std::string via{"001762c0"};
    std::vector<WeaponBackpackEvent> events;
    std::vector<WeaponInventoryEvent> inventory_events;
    std::vector<std::string> notices;
    std::int32_t armor_33a0{},max_armor_339c{};
    // Checkpoint 132pw1: the taker when it is not the player (the PORT-ONLY CPU partner's actor index); its notices
    // belong to that agent's own HUD ticker, which is not drawn.
    std::optional<std::size_t> taker;
};

struct CombatPlayInput {
    float square{};          // action 7 (Square /255 after the game-state gates)
    bool r1{};               // R1 held
    // 00247d60's t0: L1 held and input +b8 clear (+b8 has no writer found,
    // so it is 0); only the subtarget cycle outside the aim view reads it.
    bool l1{};
    std::uint32_t now_ms{};  // the aim accumulator's clock (+40)
    float dt{};
    // D-pad Right's action value 2 (sp+f88: its pressure / 255; 1.0 from a
    // key), read by the flashlight block 0024c980..0024ca44.
    float dpad_right{};
    // Select (sp+f80, 0055a44c & 0x100) after the input gates, read by the
    // weapon switch 0x0024ca48..0x0024cd30.
    float select{};
    // R3 (sp+fd8, 0055a44c & 0x400) after the input gates: the manual reload 0x0024cdb0..0x0024cde4.
    float r3{};
    // Checkpoint 132hud: GuiGameScreen +d4 == 1 (the weapon select is open, 0x0024cc9c) and the hold-Select
    // menu 0045ca20(HUD +6a4, 1) (0x0024cc74); empty, a held Select throws naming 0045ca20.
    bool game_screen_d4_one{};
    std::function<void()> choose_menu_45ca20;
};

// A reload action 0x3c pumped by begin_frame (001aa048) and the reload start's
// events (0013fb90: the sound) when it was accepted; `notices` are the
// TransferAmmo texts handed to the HUD message ticker (00242b00), in order.
struct CombatReload {
    std::size_t actor{};
    ActorReloadResult result;
    std::vector<WeaponReloadEvent> events;
    std::vector<std::string> notices;
};
// 00278160 (the bullet list 00504030's callback) for a bullet crossing a water
// triangle: the ownerless point splash 002b2b70(*004ffc34, radius *004a9098 =
// 1.25 from the initializer 004d242c, height = the point's z, the point) and the
// B_HIT_WATER sound message (001ca210 -> 0020f860 -> 003c9420 to 004ffb50). The
// play adds the splash to the actor world's manager (ActorWorld::water, Checkpoint
// 132cp11); the sound message is recorded.
struct CombatWaterHit {
    RootVector point{};
    float radius{};
    std::uint64_t geometry{};
    std::uint32_t triangle{};
    // Checkpoint 132cp11: an index into CombatPlayFrame::events, as ActorHitEffect's `at` (the splash came before
    // that event; past the end: after all of them). Inside the bullet update the count of its bullet events so far.
    ActorHitEffect::Cursor at{};
};
struct CombatPlayFrame {
    std::vector<CombatEvent> events;                             // pump, inventory updates, bullets (in order)
    std::vector<TargetingEvent> targeting;                       // 00247d60's events
    std::vector<std::size_t> hud_none;                           // 00244280 calls from 0022c210 (count per pass)
    std::vector<WeaponAimNotice> aim_notices;                    // target controller +a0 notices
    std::vector<std::pair<std::size_t,SoloDamageOutcome>> damage;
    // 00188140's wound effects (Checkpoint 132b), `at.event` an index into
    // `events` (spawned before that event; past the end: after all of them).
    std::vector<ActorHitEffect> effects;
    std::vector<std::pair<std::size_t,SecondaryStep>> secondary;
    std::vector<ActorFireRequest> fire_requests;                 // sent this frame (pumped next frame)
    std::size_t shot_counter{};                                  // 00249530 calls
    std::vector<std::string> notes;                              // registrations and other host records
    std::vector<std::size_t> backpacks;                          // indices into CombatPlaySession::backpacks() made this frame
    float hud_meter{};                                           // the 00261540 value of 00273b40
    std::vector<HudSpriteEvent> hud_sprites_pad;                 // the marker sprite calls of 0022c210 (the pad producer)
    std::vector<HudSpriteEvent> hud_sprites_update;              // the marker sprite calls of 00273b40 (end_frame)
    std::optional<CombatThrowFrame> throws;                      // with CombatPlayConfig::throws
    std::vector<AimViewHudEvent> aim_view_hud;                   // the aim view's HUD calls (00249b10 / 002497c0), in order
    std::vector<BulletEvent> aim_view_scan;                      // the lock probe's 0022ba30 candidates
    std::vector<std::pair<std::size_t,TorchFlickerCall>> torch;  // the torch flicker objects' presentation calls
    std::vector<std::pair<std::size_t,GogglesEvent>> goggles;    // 001b31b0's vision loop sound (actor_goggles.hpp)
    // Checkpoint 132ia1c: 00142f20's zoom sound (start: 0020f990, stop: 0020eba0;
    // 004f2170 bank 1 "E_ZOOM", owner the inventory's +88), with the
    // inventory's +e8 zoom angle (degrees) at the call.
    struct ZoomSound {std::size_t actor{};bool start{};float angle_e8{};};
    std::vector<ZoomSound> zoom_sounds;
    std::vector<CombatReload> reloads;                           // the reload actions 0x3c pumped this frame
    std::vector<CombatPickup> pickups;                           // backpack pickups (CombatPlayConfig::backpack_world)
    std::vector<CombatCrateMessage> crate_messages;              // the pickup menu's CrateInteractMsg sends (pumped next frame)
    std::vector<CombatInventoryMessage> inventory_messages;      // the kind-5 take's InventoryMsg sends (pumped next frame)
    // Checkpoint 132f3: the pickup menu's sound messages (its operations' and 0x0024c570's GS_CONFIRM), in
    // order, for the play's gameSound.
    std::vector<PickupMenuSound> menu_sounds;
    std::vector<CombatWaterHit> water_hits;                      // bullets crossing water (00278160), in order
    // The flashlight (flashlight.hpp): the toggles' and set state's events
    // (registrations, goggle switches applied; the click sound is the play's
    // gameSound post; `_LIGH` byte and HUD calls recorded), the beam probe's 0022ba30 candidates and the Sight
    // requests of the update 0045ae70 (delivered through
    // CombatPerceptionWorld::flashlight_sight when it is set).
    std::vector<std::pair<std::size_t,FlashlightEvent>> flashlight;
    std::vector<BulletEvent> flashlight_scan;
    std::vector<std::pair<std::size_t,FlashlightSightRequest>> flashlight_sight;
};

// What the NPC perception's host view needs beyond the combat world
// (npc_perception_think.hpp, CombatPlaySession::npc_perception_link).
struct CombatPerceptionWorld {
    // The AI controller of an actor (nullptr: the player): component +158,
    // path follower +5c (the region) and its perception's target +40 / +44.
    std::function<NpcAiController*(std::size_t actor)> controller;
    const NpcAiSystem* system{};                                 // the think list 004ea190
    // Actor +44, written each frame by 00133ab0 from the model light gather
    // 00373320 (native: StealthLighting::value, model_light_gather.hpp;
    // FLASHLIGHT.md section 16); empty: a read throws.
    std::function<std::int32_t(std::size_t actor)> stealth_44;
    // 004ed9c0[team] and 0015d790 (the route manager's regions); empty: the
    // perception throws when it reads them.
    std::function<std::int32_t(std::int32_t team)> player_region;
    std::function<std::int32_t(const RootVector& point,std::int32_t previous,std::optional<float> f20,const char* site)> region_at;
    // The level's node objects (vtable 004d95b0), handles npc_node_object(k):
    // a script SetTarget(NPC, Node) aims at one; null: a node handle throws.
    const NpcNodeGraph* nodes{};
    // The flashlight update's 0016c9d0(controller +1ac, owner, 6 = Sight,
    // 0.0) for an AI actor of the think list (FlashlightSightRequest); empty:
    // the requests are only recorded in CombatPlayFrame::flashlight_sight.
    std::function<void(std::size_t actor,std::uint64_t source)> flashlight_sight;
};

// A population GOB as the NPC look entry 2 sees it (a script RequestFacial of
// a GOB, e.g. STREETS' StreetsBlow): +c the record ID, +40 (00141850 for
// +108), +2c/+2d/+2e, the world matrix +e8 (003cd1c0: the transform's copy)
// and the eye +c0 (0014d340 -> +e4 003cd260, object_eye_c0). Its +4c is the
// GOB class word *004f5468 (0021add0 -> 003cf0e0; 0x4a46c8 in the boot
// snapshot), neither the actor class *0049d010 (0x49e9c8) nor *004ea770
// (0x49c170).
struct CombatHostObject {
    std::int32_t id{};
    float health_40{};
    std::array<std::uint8_t,3> root{};
    RootVector position{};      // +e8 row 3
    RootVector eye{};           // +c0 (any subtarget)
    // Virtual +88 (GOB 00213ac0: the model VUM's declared bounds, null for
    // "no_vum"; a loaded hotbox's 00173660: +70 / +80), read by the bullet
    // scan 0022ba30 when the object is registered. Not hosted: that throws.
    bool bounds_hosted{};
    std::optional<std::array<RootVector,2>> bounds_88;
    // Virtual +f4 (003ccff0, the same matrix as +e8 for a GOB or hotbox),
    // rows: the map sprite place 003e9550 and the radar blip 00261360.
    std::array<RootVector,4> matrix_f4{};
    // Checkpoint 132zf11: the virtual +c4 when it is not the GOB's 002152d0
    // (an NIE's 0015c020 answers 0); nullopt: the GOB's.
    std::optional<std::uint8_t> kind_c4;
};

class CombatPlaySession {
public:
    // The world must outlive the session; the session installs its hooks on
    // the actors (pad combat for the player, inventory update, secondary
    // update and presence for every actor) and registers every eligible
    // actor in actor order (0022c650: the local player is activated at load
    // through 001b53d0; 001b4f40 registers an actor found active the same way).
    CombatPlaySession(ActorWorld& world,const WeaponDatabase& database,CombatPlayConfig config);
    ~CombatPlaySession();
    CombatPlaySession(const CombatPlaySession&)=delete;
    CombatPlaySession& operator=(const CombatPlaySession&)=delete;

    // The light manager's pools for the torch branch (CombatPlayConfig::lights).
    void set_lights(LevelLights* lights);
    void begin_frame(const CombatPlayInput& input);
    CombatPlayFrame end_frame();
    // Inside begin_frame / end_frame: the open frame's events so far (132cp11: the flashes the
    // morph step 0038b530 sees on +2d90/+2d91 before end_frame hands them to the draw).
    std::span<const CombatEvent> frame_events() const;

    // game::SetPlayerWeapons(first, second) (00185330) on the player.
    bool set_player_weapons(std::int32_t first,std::int32_t second);
    // Checkpoint 132sx11: AddWeapon(Player) 00175bf0 / RemoveWeapon(Player) 00176060 on the
    // player (record 00147840(item) +13c gate, then 00143df0(inv, item, 1, 1, 0, 0) /
    // 00142450(inv, slot, 1, 0, 0)). false (deferred) without the weapon model library: the
    // records' +13c are then not loaded as the original's are.
    bool player_add_weapon(std::int32_t item);
    bool player_remove_weapon(std::int32_t item);
    // Checkpoint 132sx11: 002570c0 (game::RemoveAllBackpacks 00253c60, ResetLevel 0012f6e0's
    // second step): every backpack whose +60 is 0 destroyed (00254770), the INV: crates
    // restored from the snapshot (00256d90), then the run-time boxes cleared (00170570 on
    // the player's hotbox world). false (deferred) without CombatPlayConfig::backpack_world.
    bool remove_all_backpacks();
    bool hosts_backpacks() const;
    // 00333df0 on CombatPlayConfig::agent_statistics (ResetLevel's fourth step); false without them.
    bool reset_level_statistics();
    // game::SetRadarBlip (0022c180 -> 0022c1f0: 00273890(hud + 90, &id, 1)) and
    // game::ClearRadarBlip (0022c0f0 -> 0022c120: 00272d50(hud + 90, &id, 0))
    // on the HUD targets; returns what 00273890 / 00272d50 return.
    bool set_radar_blip(std::int32_t id,bool on);
    // The loadout screen's Accept 002bcb50 on the player with a host-chosen
    // selection (omega_play --loadout, play_loadout.hpp); returns the
    // resulting inventory's description.
    std::string apply_equipment_loadout(const EquipmentLoadoutSelection& selection);
    // Port-only (omega_play --goggles-item): 00143df0(inv, item, 1, 0, 1, 0),
    // the add the loadout's Accept makes, on the player with a host-chosen
    // item (the goggles 73 / 74 / 75 go to slot 6 by their class); returns
    // the resulting inventory's description.
    std::string add_player_item(std::int32_t item);
    // A run-time actor the world has just appended (a generator spawn,
    // 001b7b00 -> 001b5fe0): its object ID +c, model, 00144dc0 flag and
    // record item words get the per-actor state the constructor builds for
    // the load-time actors (the inventory, the record items added with
    // 00143df0 and 00143c50 as 001b6678..001b6724 do, the combat adapter's
    // entries, the hooks, the throw flow's registry), and its inventory view
    // is published. It is registered (0022c650) when it is activated, through
    // the presence hook. Throws unless the world holds exactly one actor more.
    void append_actor(std::int32_t object_id,std::optional<CombatActorModel> model,bool with_object,
        std::optional<std::array<std::uint32_t,2>> record_items,std::optional<SecondaryMotionRegistry> registry);
    // ResetNpc's in-place rebuild (001b8130, role 2: no 0xd0 object) of the
    // AI actor `actor` the world has just rebuilt (ActorWorld::rebuild_ai):
    // the per-actor state of append_actor rebuilt at the same index (the
    // destructor 001b57c0 released the old inventory and +3314), the hooks
    // on the new actor, its inventory view published.
    void reset_actor(std::size_t actor,std::optional<CombatActorModel> model,
        std::optional<std::array<std::uint32_t,2>> record_items,std::optional<SecondaryMotionRegistry> registry);
    // 00144ba0(inventory +3524, hand) from the destructor's inventory release
    // (00144c70): the hand's looping fire sound stopped; its events.
    std::vector<WeaponDischargeEvent> stop_weapon_loop_sound(std::size_t actor,std::uint8_t hand);
    // The NPC controller's view of the combat world (NpcAiController::set_combat_link).
    NpcCombatLink npc_combat_link(std::size_t actor);
    // The NPC perception link (NpcAiController::set_perception_link): each
    // think builds the NpcPerceptionHost from the actor world. The observer:
    // +2c/+2d/+2e, its eye +c0(-1), the head node 14 world matrix (00133ed0,
    // read while present), the 'flor' matrix row 1 (+e8), +33a8 (0 without
    // the throw flow, whose area flash 00186830 writes it) and +3470 = 0. Every actor's view:
    // +c, +108, the root bytes, the actor class, +3468 (SetPickupable), +14, +3034,
    // +3204, +44 (the world's stealth_44), +e8 row 3, +c0(-1), +d4, virtual
    // +f8 = 0014d3d0 (physical +e8, floor contact; the static buffers
    // 004ee5b0 / 004ee580 only receive physical +f0), the controller and its
    // team (+68), an AI's +158 / path +5c / perception target, the player's
    // aim handle (003ea550: the aim block's +28 / +2c) and the lit spot of
    // its flashlight while it is on (00199700; the spot's +e4). The local
    // player is player slot 0 (003fb8d0); the line test is this session's
    // 0022cb10 (age 0, not forced) and 0022c650 registers an actor. The world
    // is kept for the flashlight update's think list and Sight requests.
    NpcPerceptionLink npc_perception_link(CombatPerceptionWorld world);
    // The population GOBs the look entry 2 may name (handles gob_host_handle);
    // nullopt for any other object. Unset: a GOB handle is not an object.
    void set_host_objects(std::function<std::optional<CombatHostObject>(std::uint64_t)> view);
    // 0022cb10(004f77c0, a, b, max_age, force) at the current frame counter.
    std::uint64_t visibility_blocker(std::uint64_t a,std::uint64_t b,std::int32_t max_age,bool force);

    // The camera's look-at host for a locked actor (00280070, Checkpoint 121c).
    PlayerCameraLookAtHost camera_look_at_host();
    // The target-lock tracker's update 003e9880 (Checkpoint 121n): 0040c540(&id)
    // and the found object's virtual +c0(out, object, sub); nullopt when the id
    // is not in the traversal tree.
    std::optional<RootVector> object_point_c0(std::int32_t id,std::int32_t sub);
    ActorWorldCombat& actors();
    CombatWorld& combat();
    const HudTargets& hud() const;
    // 0040c540(id) for a radar blip (003e9880 +64): an object of the traversal tree.
    bool hud_found(std::int32_t id) const;
    // 00261360's reads of 0040c540(id): an actor's virtual +e4 (00132860) and its +f4 row 1
    // (the floor root placement); nullopt when not found; an NPC node throws (not modelled).
    std::optional<HudBlipObject> hud_blip_object(std::int32_t id);
    // 00242330's reads of 0040c540(&key) for a callout: +2c / +2d / +2e and an
    // actor's virtual +e4 (00132860); a node is {1, 0, 0} (never shown, so
    // its +e4 is not read); nullopt when the id is not in the traversal tree.
    std::optional<HudCalloutObject> hud_callout_object(std::int32_t id);
    const PadWeaponState& pad() const;
    const PlayerTargetingState& targeting() const;
    const TargetVisibility& visibility() const;
    const std::vector<TargetHandle>& registered() const;
    // Checkpoint 132zf11 (the script tank, tank.hpp; research/TANKS.md).
    // 0022c650(004f77c0, object) for a hosted object that is not an actor
    // (set_host_objects; the tank's body NIE, 00284100): false when it holds
    // a slot already or the 30 slots are full (noted). Throws when unhosted.
    bool register_host_object(std::uint64_t object);
    // The actor traversal array *004ffd44 in actor order (the traversed
    // actors): the root bytes +2c..+2e, the +58 controller (every actor of
    // the array has one: the player controller or the AI component) and its
    // virtual +54 (the player controller's 00175330 answers 1, the AI's 0).
    struct TraversalActor {
        std::uint64_t object{};
        std::array<std::uint8_t,3> root{};
        bool controller_58{};
        bool controller_player_54{};
    };
    std::vector<TraversalActor> traversal_actors() const;
    // 0040c540 over the actors (traversed) and the nodes; 0 when not found.
    std::uint64_t object_by_key(std::int32_t id) const;
    // +c of an actor, a node or a hosted object.
    std::int32_t object_key(std::uint64_t object) const;
    // A weak handle (the object and its +c when stored) still resolves.
    bool object_resolves(std::uint64_t object,std::int32_t id) const;
    // Virtual +c0(object, -1): an actor's eye, a hosted object's +e4 (0014d340).
    RootVector object_eye_c0(std::uint64_t object) const;
    // 00277e20 for a shooter that is not an actor (the tank's fire 00286690);
    // inside begin_frame / end_frame, so the bullet manager 00275670 of
    // end_frame updates it the same frame (00133420 precedes 00275670).
    // `shooter` is what 00188780 reads of the shooter on a hit (132uk6).
    void fire_object_bullet(const ShotRequest& request,const BulletObjectShooter& shooter);
    float shake_352c(std::size_t actor) const;
    std::int32_t frame_counter() const;
    const std::vector<CombatBackpack>& backpacks() const;
    // Checkpoint 132el6: a lift's rider handle (bp address, id) as 00218200 /
    // 00256500 test it: the backpack is not deleted and its +c is the id. A
    // destroyed backpack without the release host (CombatBackpackWorld::
    // release_3cb230) throws; an address that is no backpack of the session
    // throws.
    bool backpack_rider_live(std::uint32_t address,std::uint32_t id);
    // The lift's rider pass on a backpack: s3 = 0 its vt+a8 00256670 with the
    // vector matrix[12..15]; s3 = 1 the 004d9360 message to 00254600 ->
    // 003cea10 mode 3 with the message matrix (transport 1, at once).
    void carry_backpack(std::uint32_t address,bool message,const std::array<float,16>& matrix);
    // Checkpoint 132bp: the population's INV: crates at load (001c2a10 ->
    // 00257480: cBackpack_ctor 00257f30, then the list append 00257620),
    // with CombatPlayConfig::backpack_world; `find_collision` answers
    // IdMgr_FindCollision 0040c480 for the owner pair (0: none). Returns
    // their indices into backpacks().
    std::vector<std::size_t> construct_crates(const std::vector<BackpackInvRecord>& records,
        const std::function<std::uint32_t(std::uint32_t,std::uint32_t)>& find_collision);
    // A zone request (00224330 / 00224180) on a crate's key: its +2d through
    // virtual +5c 003ce980 / +60 003ce790. False: not a crate of the session.
    bool set_crate_present(std::uint32_t key,bool present);
    // Checkpoint 132f7: 00256b40 at the loadout done path (00168bc0): the
    // crate snapshot list *004f7f48 rebuilt from the list's +60 == 1 entries
    // (backpack_crate_snapshot). Needs CombatPlayConfig::backpack_world.
    const std::vector<BackpackCrateSnapshot>& snapshot_crates();
    // The owner +300 of backpack i as its draw gate 00253fe0 reads it (+c, root bytes); nullopt when +300 is 0.
    std::optional<WeaponBackpackOwner> backpack_owner(std::size_t i) const;
    // The local agent record's statistics (nullopt: not supplied).
    const std::optional<AgentStatistics>& agent_statistics() const;
    // The config's player name (00336170 of the agent record; checkpoint 132f3b's stats screen).
    const std::optional<std::string>& player_name() const;
    // The throw flow (nullptr without CombatPlayConfig::throws).
    const CombatThrows* throws() const;
    CombatThrows* throws();
    // The flashlight object +33ec of an actor (nullptr: none, roles other
    // than 1 / 3) and its on byte (00199740).
    const Flashlight* flashlight(std::size_t actor) const;
    bool flashlight_on(std::size_t actor) const;
    // The engine variables Flash* (004d7c10) the update reads.
    void set_flashlight_tuning(const FlashlightTuning& tuning);
    // Checkpoint 132t: the pickup menu (input +128) the backpack receiver
    // 001762c0 built (nullptr: none), the interact prompt's 002526f0 on it
    // (0x0024ecac; false: no entries, the caller then closes it), the close
    // 00247320, and a preview object its element data names (nullptr: not
    // a token of the menu). Inside begin_frame / end_frame.
    const PickupMenu* pickup_menu() const;
    bool pickup_menu_update();
    void pickup_menu_close();
    const PickupMenuPreview* pickup_menu_preview(std::uint32_t token) const;
    // Checkpoint 132f3b: the start menu's equipment pane (GuiEquipmentPane
    // 00454b10 / 00454f80 / 00455260) on the player: its inventory +3524,
    // 00175ff0(player, item) -> 001439b0 (the multiplayer send 002493c0 is
    // not reached in solo play), the goggles' hold toggle 00199860 and actor
    // +33e7, and 001861f0(actor, record +74). Outside begin_frame / end_frame
    // (the world is paused); the notes join the next frame's.
    const WeaponInventory& player_inventory() const;
    bool player_equip_item(std::int32_t item);
    void player_goggles_toggle();
    std::uint8_t player_goggles_33e7() const;
    std::int32_t player_clips_for(std::int32_t clips_74);
    // Checkpoint 132hud: the weapon select's (GuiGameScreen 0045c480 / 0045c0f0) inventory reads and the
    // holster 00143b60(+3524).
    void player_holster_current();
    // Checkpoint 132ms2: game::SetHardpointVis 00185550 on an actor (the player or an NPC): 00142e40(actor
    // +3524, slot, vis) (CombatWorld::set_hardpoint_vis).
    void set_hardpoint_vis(std::size_t actor,std::uint8_t slot,bool visible);
    // Checkpoint 132sx10: StealPlayerWeapons (00253c90 / 00253cc0 -> 00253dd0)
    // into the backpack whose +c is `backpack`: 00258330(bp, 1), +2f3 = 1,
    // +2f0 = 0; per slot other than 8 holding an item (slot +8 >= 0):
    // 00255af0(bp, item, 00143870(inv, slot), 00143940(inv, slot), 0) and
    // 00142450(inv, slot, 0, 0, 0); then 00254990(bp). False without
    // CombatPlayConfig::backpack_world (the backpacks are not constructed).
    bool steal_player_weapons(std::uint32_t backpack,const std::array<std::uint8_t,3>& slots);
    // PORT-ONLY (checkpoint 132pw1, coop_partner_kit.hpp): the CPU co-op partner as the second agent at the
    // backpacks. partner_backpacks lists the live, simulated backpacks (dead drops and INV: crates with a hotbox) as
    // the partner's choice reads them; partner_agent_view is an actor's inventory, armour and per-item capacity (record
    // +70 x 001861f0(+74)); partner_pickup runs, inside begin_frame / end_frame, the walk-over a second agent's zone
    // enter makes (001774c8 -> 00254600 -> 001762c0 -> PickupWpn 00254da0 on the partner's inventory) and, with
    // swap_item, the Triangle menu's Cross 00251920 on that entry and its close (00255f10). The receiver's index test
    // (+308 against the picker's +68) is answered as the authority's serving of the request 005061b0 (not ported).
    struct PartnerBackpack {
        std::size_t index{};                                    // into backpacks()
        RootVector position{};                                  // the transform's translation (T +40)
        std::optional<NpcInteractableBounds> bounds;            // the hotbox's
        bool crate{};
        PartnerBackpackView view;
    };
    std::vector<PartnerBackpack> partner_backpacks(std::size_t partner);
    PartnerAgentView partner_agent_view(std::size_t actor);
    CombatPickup partner_pickup(std::size_t backpack,std::size_t partner,std::optional<std::int32_t> swap_item);
    std::int32_t player_spare(std::uint8_t slot) const;   // 00143870(+3524, slot)
    std::int32_t player_clip(std::uint8_t slot) const;    // 00143940(+3524, slot)
    bool player_has_clip(std::int32_t item) const;        // 00143710(+3524, item)
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
