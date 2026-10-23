#pragma once
// The throw flow (C6b) in the combat play session over an ActorWorld
// (research/COMBAT_ITEMS.md, "Native status"; combat_play.hpp). The session
// builds CombatThrows when CombatPlayConfig::throws is set; it then hosts,
// for every actor:
//   001960c0's throw start: 00197560 (hold class 1) on the actor's item fields
//     (+33cc..+33e4) and the inventory's +ef;
//   00197e00's item triggers at 0x001b1d10 (from 00198278: the release at
//     phase 0.515 launching through 00141e20 = ThrowWorld::launch);
//   the +33a8 / +33ac decay at 0x001b2328..0x001b237c;
//   after the secondary component's state step: the step's posts delivered at
//     once to the actor's receiver (001d7940: embedded code 2 runs the release
//     00141cd0), then 001d7480 / 001d6120's timing tail (SecondaryPlayback)
//     with the character's motion registry;
//   the receiver's inventory hosting (00489d70 / 00489d78, null part
//     definitions, the launch dependency of the tokens 0xa0 / 0xa1 bound to
//     ThrowWorld::launch with the inventory view republished);
// and, after the bullet manager (in end_frame), the thrown items
// (ThrowWorld::update): the projectile LOS 004ea0b0 -> 0013a480 over the
// static world (excluded mask 0x10), the actor scan 0022ba30 over the actors'
// bounds with the hit entry 00188780, and the area objects over the target
// manager's list 004f78dc (004f77c0 +11c, the session's registrations) with
// the actors' +33a8 / +33ac.
//
// Static readings and host decisions (COMBAT_ITEMS.md, "Play wiring"):
// - The hand frame of 00141e20 is node 20: `lb` of 0049d02c (00141e88), byte
//   12 of the node table 0049d020, through 00133ed0 (the model's
//   skeleton_node_world_matrix); the actor matrix +f4 is the 'flor' root
//   placement (as the session's other +f4 readers).
// - Result +50 of the projectile's LOS request is taken as the native
//   kernel's hit normal. The original's normal can differ from it at most in
//   sign (the kernel orients it against the ray); its only reader for hold
//   class 1, the bounce, gives the same result for n and -n (u and w change
//   sign, v does not, and each enters r twice; exact negations, so equal bits
//   up to the sign of an exact zero). Result +84 is the COL
//   geometry handle, as answer_los_request gives it.
// - The play model has no slot table (render +2410): record +13c is never
//   loaded natively, so every part definition is null; the release's null
//   part is recorded (set_model_slot), a non-null part throws, and so do the
//   slot count, presence, swaps, the owner query and the empty / selection
//   notices (the release of a thrown slot's last item).
// - The gait / footstep part of 00197e00 (00197e00..00198274) is not run; the
//   IR switch's argument +33e6 is not modelled (only 00199590, not hosted,
//   would read it).
// - Admitted launches: hold class 1 with damage type 0, 2, 3, 9 or 13 (the
//   explosion through CombatThrowConfig::explode); the area types 4..8 (fire,
//   gases, smoke: ignition 00186360, the gas exposure's node and LOS, the tear
//   gas action, the smoke volumes of 002594a0) and hold class 7 (the star
//   message 004eeed0, its sound, the scan's +d byte) throw at the start or
//   launch.
// - 001960c0's melee branch (COMBAT.md, "Checkpoint 113 NeckSnap takedown"):
//   the target choice 001414d0 over the registrations, then 00196480 with the
//   secondary one-shot (001cae20) on the actor's component, the swing sound
//   (0020f860) recorded in the frame and the replication message 004eeee8
//   recorded as queued (solo 003c9420 transport 0 clones and queues it,
//   003c9950..003c997c; no handler loads its key, so its delivery has no
//   effect). The guard *(*004ffbd0) +24 is the hotbox tracker's timer of the
//   locally owned player.
// - The melee hit 00141880 (COMBAT.md, "Checkpoint 115 melee hit and
//   takedown execution") at the swing's trigger phase (00197e00,
//   actor_throw.hpp): 0040c540 on +33cc (no call for none), the inventory's
//   +84 and slot item, the LOS 00148290 over the static world (excluded mask
//   0x58, trace_world_los), the hit volumes 00188140 and the virtual +100
//   (ActorWorldCombat::damage). doKnifeDmg *00489da0 is
//   CombatThrowConfig::knife_damage. The baton branch reads +346a
//   (SoloActorSimulation::stun_immune_346a), adds the knockout statistic
//   00333f10 to CombatThrowSession::statistics and starts the native stun
//   00184ce0 (actor_stun.hpp; its countdown runs in the actors' sense-decay
//   hook). Its shock effect 00185d40 (actor_effect_slots.hpp; COMBAT.md
//   "Checkpoint 120b baton shock and torch") runs on the actor's effect
//   slots with the effect / light objects recorded (CombatThrowFrame::
//   effect_calls: particle emitters 00236810 and the light 002624a0 are
//   presentation; their keys come from a session counter) and its action 7
//   (001a92a0 case 7, 001a9bc8) is SoloActorSimulation::
//   receive_stunned_action with transport 1. The stun end's type-7 damage
//   queues the death action 0x42 code 1 and the AIUnconsciousMsg for the
//   next pump (solo_actor_simulation.cpp). The torch's hold-class-10 light
//   00144080 runs in the weapon update (torch_light.hpp). The torch hit's
//   ignition 00186360 (actor_ignition.hpp) writes the victim's +326a
//   (SoloActorSimulation::set_burning_326a) and creates the fire / smoke
//   slots 0 / 1 and the fire zone 002624a0 and records the fire sound. The
//   burning actor (COMBAT.md "Checkpoint 120c burning actor",
//   actor_burning.hpp) runs natively: the effect objects (00235f60) and the
//   fire zone (00261df0: damage type 4 and heat around the burning actor) in
//   the world-object pass after the thrown items, the actor's slot loop
//   001b1e10..001b2214 (the zone strength 00262470, the burn damage from
//   the setter +3480, the extinguish and the slots 1 / 2 off at +80 == 0) and
//   the +33b0 decay 001b2380 in its update, and the damage path's ignition
//   (00187ac8: +33b0 past 20).
// - The takedown branch: the motion installs on attacker and victim through
//   the actors' models (0018c5a0, 003ad500, the retained +326c), physical
//   +11, the victim's teleport mode 2 with transport 1, the death motion
//   0018c9d0 (+34e4), the damage (null penetration) and the physical +3d0
//   clears (SoloActorSimulation::clear_physical_3d0). The player's takedown
//   camera 00131540 (COMBAT.md, "Checkpoint 120 takedown camera and
//   baton/torch hits") is player_takedown_camera with the head node 14 of
//   the attacker's model, its +f4 and the LOS 0013a480 over the static world
//   (answer_los_request); the camera object G +d4 is takedown_camera_object
//   on the attacker's world_objects (the original's one list 004ffb50 +c4),
//   pushed as the pending removal that the attacker's tokens 0x96 / 0x97 run.
#include "actor_burning.hpp"
#include "actor_effect_slots.hpp"
#include "actor_ignition.hpp"
#include "actor_goggles.hpp"
#include "actor_melee.hpp"
#include "actor_model_slots.hpp"
#include "actor_stun.hpp"
#include "actor_stunned_action.hpp"
#include "actor_throw.hpp"
#include "agent_statistics.hpp"
#include "animation_graph.hpp"
#include "animation_player.hpp"
#include "area_effect.hpp"
#include "combat_actor_world.hpp"
#include "combat_world.hpp"
#include "npc_perception.hpp"
#include "player_targeting.hpp"
#include "secondary_playback.hpp"
#include "takedown_camera.hpp"
#include "throw_world.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// 001d7480's registry lookups (0018a670 / 0018a660) over a character's motion
// graph (the manifest's flat node index is the numeric registry ID,
// actor_motion.hpp): the node of that name, and the entry of an ID: the
// node's type byte +4 (the manifest's node type, 0 a leaf) and for a leaf its
// clip's version, frame count (+4), key stride, frame period and loop word
// from the clip bank. Every name the playback looks up must resolve: a name
// that is not a node throws (the original would use registry ID 0). An ID
// outside the graph is a null entry; a leaf whose clip is not in the bank
// throws. Both must outlive the registry.
SecondaryMotionRegistry make_secondary_motion_registry(const AnimationGraph& graph,const AnimationClipBank& clips);

// The projectile's LOS request 005435a0 answered by 0013a480 over the static
// world (required mask 0, excluded mask 0x10; the projectile as request +50
// gates only object geometry, which the static world has none of): result
// +10 the hit point, +50 the hit normal (see above), +84 the COL geometry
// handle. A zero displacement has no hit (003c26d0).
std::optional<ProjectileLosHit> projectile_world_los(const SoloCollisionWorld& world,const RootVector& origin,const RootVector& displacement);

// The native key of the takedown camera object G +d4 (the word at
// 004ffb50 + d4 = 004ffc24) on an actor's world_objects. Its snapshot is +2c
// = 1 (base object), +2d from its activation, +2e = 0; its +c (the published
// generation of 00131320) is not modelled and published as 0: no native code
// reads the published generation.
inline constexpr std::uint64_t takedown_camera_object=0x004ffc24ULL;

struct CombatThrowConfig {
    // One per actor; nullopt: the actor's playback throws when it installs a motion.
    std::vector<std::optional<SecondaryMotionRegistry>> registries;
    // *004f7fe0 / *004f7fe8: 0 in the boot snapshot, no writer found (open). Explicit.
    std::optional<ThrownProjectileTuning> tuning;
    // The shared gravity vector 004f5430 (game_initial_gravity). Explicit.
    std::optional<RootVector> gravity;
    // *004bdd00, the "none" name word (-1 in the boot snapshot). Explicit.
    std::optional<std::uint32_t> none_word;
    // Actor +44 (the flash's blindness reads it, 00259880); empty: reading it throws.
    std::function<std::int32_t(std::size_t actor)> stealth_44;
    // 0021a200 for a detonation of damage type 2 / 13 (ExplosionWorld::blast); empty: throws.
    std::function<void(const BlastRequest& request)> explode;
    // The engine variable game::doKnifeDmg (*00489da0, boot value 1; scripts
    // may set it) whose low byte the melee hit 00141880 reads; empty: throws.
    std::function<std::uint8_t()> knife_damage;
    // Checkpoint 132ms4: the knife takedown's token 0x98 (001a3fd8): the first word of the effect table
    // entry 00236f20(index) (nullopt: a null entry, which the original reads at address 0) and the
    // directional effect 00232180(id, point, direction) of `actor`'s token; empty: the token throws.
    std::function<std::optional<std::int32_t>(std::uint8_t index)> effect_descriptor;
    std::function<void(std::size_t actor,std::int32_t id,const RootVector& point,const RootVector& direction)> directional_effect;
    // Session-local identities of the projectiles and area objects (their handles and +c).
    std::uint64_t first_object{0xd0000000ULL};
    std::int32_t first_id{0x70000000};
};

// One melee request of 001960c0: 001414d0's answer and 00196480's.
struct CombatMeleeStart {
    MeleeTarget choice;                                                    // 001414d0
    MeleeAttackResult result;                                              // 00196480
    std::vector<std::pair<std::uint32_t,RootVector>> sounds;               // 0020f860 (descriptor, position) to 004ffb50
    // The takedown's teleports (the victim's message 0055d4e0 mode 2) and
    // physical +3d0 clears (in order).
    std::vector<std::pair<std::uint64_t,SoloActorTeleportResult>> teleports;
    std::vector<std::uint64_t> clears_3d0;
    // 00131540: the player's takedown camera (a player's takedown only).
    std::optional<TakedownCamera> camera;
};
// One melee hit 00141880 (after 0040c540 found the target).
struct CombatMeleeHit {
    std::uint64_t target{};
    MeleeHitResult result;
};

// What the throw flow did in one session frame.
struct CombatThrowFrame {
    std::vector<std::pair<std::size_t,ActorThrowStart>> starts;            // 00197560
    std::vector<std::pair<std::size_t,CombatMeleeStart>> melee;            // 001414d0 / 00196480
    std::vector<std::pair<std::size_t,CombatMeleeHit>> melee_hits;         // 00141880
    // The stun's sound request 0020f990 (start) / stop 0020eba0 to 004ffb50
    // (actor, descriptor 004f12d0, start).
    struct StunSound {std::size_t actor{};std::uint32_t descriptor{};bool start{};};
    std::vector<StunSound> stun_sounds;
    // The star release's sound (00198410..00198460): G_THROWING_STAR (001ca210 bank 1) at the actor
    // (0020f990), to 004ffb50 with transport 0; one entry per release, the actor.
    std::vector<std::size_t> star_sounds;
    // 00185d40's services in order: "create" (00236810: descriptor, count,
    // f12 in value), "light" (002624a0: f12 / f13 in value / value2),
    // "register" / "unregister" (00571748), "on" / "off" (virtual +5c / +60),
    // "release" (003cb230, the delay in value) and "stop_sound" (G_TASER_BODY).
    struct EffectCall {std::size_t actor{};std::string what;std::uint32_t object{},descriptor{},count{};float value{},value2{};};
    std::vector<EffectCall> effect_calls;
    // 001a92a0 case 7 (001a9bc8) delivered with transport 1 by the stun.
    struct StunnedAction {std::size_t actor{};std::uint32_t code{};ActorStunnedActionResult result;};
    std::vector<StunnedAction> stunned_actions;
    // 00186360 by the melee hit (actor, the state it left; +326a is the health's).
    // Its fire sound start / stop are effect_calls "fire_sound:" / "fire_sound_stop:".
    std::vector<std::pair<std::size_t,ActorIgnitionState>> ignitions;
    // The controllers' virtual +80(controller, source, value): the burning
    // actor's own (001b2134, players only) and the fire zone's heat (00261df0).
    // The AI component's +80 (0015b830) does nothing; the player's (00176590)
    // keeps the maximum in +1ac for its presentation. own: the actor's own
    // (001b2134, inside its update 001b1710, before the controller update
    // 001770f0 reads +1ac); else the zone's (the objects pass, after it).
    struct Heat {std::size_t actor{};std::uint64_t source{};float value{};bool own{};};
    std::vector<Heat> heat;
    // The effect objects' emitter calls (00235f60): "create" (003a2dd0),
    // "destroy" (003a2ba0 + 0036b130) and "fade" (+6a = 1).
    struct EmitterEvent {std::size_t actor{};std::string what;std::uint32_t emitter{};std::string name;};
    std::vector<EmitterEvent> emitter_events;
    // The fire damage requests (virtual +100): the burn damage 001b1fa0
    // (category 1, type 4, from +3480 or the actor) and the zone's 00261df0
    // (category 3, type 4, from the zone); the outcomes are in the frame's damage.
    struct FireDamage {std::size_t actor{};std::uint64_t source{};std::uint8_t category{},type{};float amount{};};
    std::vector<FireDamage> fire_damage;
    std::vector<std::pair<std::size_t,ActorItemTriggers>> triggers;        // 00197e00 calls that did something
    std::vector<std::pair<std::size_t,GogglesEvent>> goggles;              // 00199590 (the switch_on_ir trigger)
    std::vector<std::pair<std::size_t,InventoryLaunchResult>> launches;    // 00141e20
    std::vector<std::pair<std::size_t,SecondaryEventPost>> posts;          // one-shot completions delivered
    // The upper-body pass after an actor's playback install (upper_body_pass.hpp):
    // reported (else the failure), the node rows written through 001ce7c0's
    // vt +38 (hand: node 20, the shots' hand node) and node 0's vt +3c.
    struct UpperBodyWrite {std::size_t actor{};bool reported{};std::string failure;std::uint32_t rows{};bool hand{},root{},look{};};
    std::vector<UpperBodyWrite> upper_body;
    std::vector<std::size_t> slot_writes;                                  // model slot 0 set to the null part (00141cd0)
    // 00186830's white-out 00260990(*004ffc30, 1, 0.0, 0.3, amount, amount), in call order (vision_fx.hpp).
    struct WhiteOut {std::size_t actor{};float amount{};};
    std::vector<WhiteOut> white_outs;
    std::vector<BulletEvent> scan_events;                                  // the projectiles' 0022ba30
    std::vector<CombatEvent> hit_events;                                   // 00188780 from those scans
    // Checkpoint 132ia1c: the air taser's calls (air_taser.hpp) in call order:
    // fire / fire busy (00266200: object = target, descriptor = the state
    // before, position = the aim point, code = subtarget), state (00262d90's
    // transitions: descriptor the new state, code the old, value +b4),
    // release (00263080), sound / stop sound (descriptor), effect (00185d40:
    // descriptor kind, code on), damage (vcall +100: descriptor type, code
    // category), statistic, camera / camera release (00131540 / 00131510),
    // create / destroy effect (the Tazer_Line_glo1 particles, presentation:
    // descriptor the taser.cc line), burn (00232120), flak (00242fa0) and
    // noise (0016b420: descriptor the owner ID, value the radius).
    struct TaserEvent {std::size_t actor{};std::string what;std::uint64_t object{};std::uint32_t descriptor{};RootVector position{};float value{};std::int32_t code{};};
    std::vector<TaserEvent> taser;
    ThrowWorldFrame world;                                                 // ThrowWorld::update
};

// The session's answers the throw flow needs (combat_play.cpp).
struct CombatThrowSession {
    ActorWorld* world{};
    ActorWorldCombat* actors{};
    CombatWorld* combat{};
    const WeaponDatabase* database{};
    const std::vector<TargetHandle>* registered{};                         // 004f78dc (0022c650)
    std::vector<std::int32_t> object_ids;                                  // actor +c
    std::function<std::uint64_t(std::int32_t id)> find;                    // 0040c540 (0: none)
    std::function<void(std::size_t actor,std::uint64_t target,bool flag)> fire_notice;   // AI controller +cc
    AgentStatistics* statistics{};                                         // 00333f10's agent (null: not hosted)
    // Checkpoint 132pc1: a registered object that is not an actor (a script's SetTarget node or GOB, 0022c650 stores
    // any class): its +c and its virtual +108 (00141850: 3 when trunc(+40) is 0, else 0). Nullopt: not hosted.
    struct BaseObject{std::int32_t id{};std::uint8_t life_108{};};
    std::function<std::optional<BaseObject>(std::uint64_t object)> base_object;
};

class CombatThrows {
public:
    // Installs the hooks on the world's actors and the adapter; the
    // destructor removes them.
    CombatThrows(CombatThrowSession session,CombatThrowConfig config);
    ~CombatThrows();
    CombatThrows(const CombatThrows&)=delete;
    CombatThrows& operator=(const CombatThrows&)=delete;
    // A run-time actor the world has just appended: its object ID, motion
    // registry, playback and senses (constructor state) and the hooks.
    void append_actor(std::int32_t object_id,std::optional<SecondaryMotionRegistry> registry);
    // ResetNpc's in-place rebuild (001b8130) of actor `a`: the same object
    // ID, the new registry, the constructor state of append_actor, the hooks.
    void reset_actor(std::size_t a,std::optional<SecondaryMotionRegistry> registry);
    // ThrowWorld::update with the events of the frame (after the actors and
    // the bullet manager; the inventory views collected).
    void update(float dt);
    // Checkpoint 132uk4: the bullet update's hold-8/9 block (00276460,
    // 002774e0..00277928). bullet_area is its 0025a2d0(new, owner, point,
    // record, 1): the area joins the throw world's list and the frame's
    // events, and this frame's update (which runs after the bullet manager)
    // updates it. bullet_blast is its 0021a200(owner, 1, 1, point, 1, 1,
    // 0.0, 10.0, 10.0) through CombatThrowConfig::explode. Checkpoint 132uk6:
    // also the explosive branch's 0021a200(owner, +64, 1, point, 0, 1, 1.0,
    // +c4, +c8) (00277388); object is the source's +14 / +e4 when it is a
    // hosted non-actor shooter (a tank, 003cd260).
    void bullet_area(const BulletAreaCall& call);
    void bullet_blast(const BulletBlastCall& call,const BulletObjectShooter* object=nullptr);
    // Checkpoint 132uk6: the discharge's launched projectile (0025d730 /
    // 0025d4b0, ThrowWorld::launch_discharge); the discharge runs in the
    // actor update, so this frame's update (after the actors) moves it first.
    void launch_discharge(const ObjectWeakHandle& owner,const std::array<RootVector,4>& frame,std::int32_t item,
        const RootVector& point,float arc);
    // The spawned NPC destructor's (001b57c0) calls on this actor's state:
    // 00185d40(actor, kind, 0, 1.0f) for an effect slot, and the stun's
    // sound stop 0020eba0(msg, 004f12d0, actor) (a stun_sounds entry, as
    // the stun's own stop).
    void effect_off(std::size_t actor,std::uint32_t kind);
    void stop_stun_sound(std::size_t actor);
    // game::ShockToDeath(GOBJ) 00185280 on the GOBJ's actor (003cc820,
    // identity; every world actor is of the human class *0049d010): nothing
    // when its life state (virtual +108 & 0xff) is 2 or 3 or it is already
    // stunned (!(+33bc < 0)); otherwise the stun 00184ce0(actor, lethal 1,
    // notify 1, 0.5, 10000.0), whose end deals the lethal type-4 damage.
    // Returns whether the stun started.
    bool shock_to_death(std::size_t actor);
    // Checkpoint 132ia1c: the air taser (hold class 5, air_taser.hpp) on
    // actor's inventory object +0: the entry 00266200 (from the discharge),
    // the dispatcher 00262d90 (from the inventory update) and the release
    // 00263080 (from the item notice 00143770). Each throws without the
    // object (only roles 1 / 3 have one).
    bool air_taser_fire(std::size_t actor,std::uint64_t target,bool aim_view,std::int32_t subtarget);
    void air_taser_update(std::size_t actor,float dt);
    void air_taser_release(std::size_t actor);
    // A FIR: zone of the level (make_level_fire_zone): the loader 001c0b00
    // registers each (0040c9d0) in record order at level load, before any
    // actor's effect object; its key is the record ID. The traversal updates
    // it (00261df0) while present (+2d) and enabled (+2c); its damage source
    // is the zone and its +e4 level_fire_zone_position_e4.
    void add_level_fire_zone(const FireZone& zone);
    // The zone tracker's 00224330 / 00224180 and Enable / Disable(FireObj)
    // on a level fire zone (set_fire_zone_present / set_fire_zone_enabled);
    // false when the key is not one.
    bool set_level_fire_zone_present(std::uint32_t key,bool present);
    bool set_level_fire_zone_enabled(std::uint32_t key,bool enabled);
    bool level_fire_zone(std::uint64_t key) const;
    // Checkpoint 132ms8: a level fire zone's state (its +2c / +2d / +2e for the sound owner view); null when the
    // key is not one.
    const FireZone* level_fire_zone_state(std::uint32_t key) const;
    // A burning actor's zone (002624a0 by 00185d40; +0c 0, so no script
    // record names it), still an effect object of the world.
    bool actor_fire_zone(std::uint64_t key) const;
    CombatThrowFrame take_frame();
    const ThrowWorld& world() const;
    // Checkpoint 132sx11: 0025a120 (game::ResetLevel), its events added to the frame.
    void finish_areas();
    const SecondaryPlayback& playback(std::size_t actor) const;
    const ActorSenses& senses(std::size_t actor) const;
    // Actor +33bc / +33c0 / +33c8 (the stun 00184ce0).
    const ActorStunState& stun(std::size_t actor) const;
    // Actor +354c..+3570 / +34e0 (00185d40; its burning_326a is not the
    // actor's: +326a is the health's, health().burning_326a).
    const ActorEffectSlotState& effect_slots(std::size_t actor) const;
    // Actor +31f4 / +3480 / +3484 / +3488 as the ignition 00186360 left them
    // (the three bytes are copied from the health at each ignition).
    const ActorIgnitionState& ignition(std::size_t actor) const;
    // The burning actor (actor_burning.hpp): actor +2df6 (the model glow),
    // the effect objects / fire zones by key (null: not one), the world-object
    // registration (00571748) in order, and the particle emitters.
    std::int16_t glow_2df6(std::size_t actor) const;
    // The actor's render model attach points (+60 +2410).
    const ActorModelSlots& model_slots(std::size_t actor) const;
    const FireEffectObject* fire_effect(std::uint32_t object) const;
    const FireZone* fire_zone(std::uint32_t object) const;
    const std::vector<std::uint32_t>& registered_objects() const;
    struct FireEmitter {
        std::size_t actor{};
        std::uint32_t object{};                 // the effect object
        std::string name;
        FireMatrix matrix{};
        float weight{};
        RootVector velocity{};
        float strength{},floor_5c{};
        bool faded{};
    };
    const std::map<std::uint32_t,FireEmitter>& emitters() const;
    // An effect object or fire zone as the perception's object view reads it
    // (nullopt: not one of them).
    std::optional<AwarenessObjectView> object_view(std::uint64_t object) const;
    // The takedown camera G +d4 while it is active (+2d set: pushed by the
    // actor's takedown 00131540 and not yet removed by 00131510): the last
    // view given to it (its update 0017a640 does not run: the constructor
    // 0017f9f0 leaves +200 = 0) and its half angle +60 = fov / 2.
    struct ActiveTakedownCamera {TakedownCamera camera;float half_fov_60{};};
    std::optional<ActiveTakedownCamera> takedown_camera(std::size_t actor) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
