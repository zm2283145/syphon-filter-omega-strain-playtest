#pragma once
// The native AI component (npc_ai.hpp) as the +58 component of an actor in an
// ActorWorld (actor_world.hpp): component virtual +3c is the radius check
// 0015a440, +40 the update 00159590 (patrol subset), +48 Activate 00157df0.
// Messages the component sends itself with transport 0 (path events, the
// mode-change notice, AI events 3/5) are delivered by the pump of the next
// frame through the dispatcher 00151d30; transport 1 ones at once. Actor
// actions go to 001a92a0 (SoloActorSimulation::receive_actor_action) and the
// navigation request to the bridge (receive_navigation_request). The script
// notifications to the actor (Activated, WaypointReached,
// DestinationReached; transport 0) are queued on the actor
// (SoloActorSimulation::queue_ai_notification), whose pump offers them to
// its script filter; TimeElapsed (to the component) is only recorded. The
// weapon-fired notice (to the component, transport 0) is queued with the
// component's messages and offered to its script filter by the pump.
#include "actor_world.hpp"
#include "npc_ai.hpp"
#include "npc_attack.hpp"
#include "npc_dead_drop.hpp"
#include "npc_death_notice.hpp"
#include "npc_follow.hpp"
#include "npc_carry_body.hpp"
#include "npc_script_control.hpp"
#include "npc_perception.hpp"
#include "npc_perception_think.hpp"
#include "npc_remote.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include "population.hpp"
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace omega {
// The node collection 004ffbe0 from the NOD: records (0015c390): position
// (node +80), type bits (+64), links {target, type} (+68), the record
// parameter (+7c). A node's association object (+60, parent index through
// 0040c480) is not resolved natively: a node with a parent index has an
// association of unknown eligibility (read by 0014bff0 only while the actor
// is airborne, where it then throws), one without has none.
NpcNodeGraph make_npc_node_graph(const Population& population);
// What the controller's hit and death notices need from the combat host.
struct NpcCombatLink {
    // 0016c9d0's view of an attacker (+c, virtual +108, +4c, +2c/+2d/+2e,
    // +3468, virtual +e8 row 3); nullopt throws.
    std::function<std::optional<AwarenessObjectView>(std::uint64_t object)> object_view;
    // Port-only (port_cheats.hpp): an attacker this NPC ignores (no hit notice). Empty: none.
    std::function<bool(std::uint64_t object)> port_ignored;
    // The actor's weapon inventory (+3524) and the weapon database; the
    // controller calls inventory_changed after changing it (Activate, the
    // death notice, the dead-state drop) so the host can publish it.
    std::function<WeaponInventory*()> inventory;
    std::function<void()> inventory_changed;
    const WeaponDatabase* database{};
    std::function<std::optional<NpcKillerView>(std::uint64_t killer)> killer;
    std::function<bool(std::uint64_t killer)> killer_hostile;          // 003cc8e0(killer, actor)
    AgentStatistics* statistics{};                                     // the local agent record (00333f10 / 00333ee0)
    // 0018c770(killer, line): the player killer's friendly-kill voice line (00186f30). Empty: recorded only.
    std::function<void(std::uint64_t killer,std::int32_t line)> voice_line;
    std::function<std::optional<std::array<std::uint8_t,2>>()> secondary;   // +3314 present: +310 / +311
    std::function<std::uint32_t()> random;                             // 00127dd0 (the shared game RNG)
    std::function<RootVector()> eye_c0;                                // the actor's virtual +c0(actor, -1)
    // Checkpoint 132d2: actor +38 as the last scene frame (0013e1b0) left it (00159020's corpse removal);
    // absent: cGOBJ_ctor's 0x80 (003cf0e0, no scene frame hosts the actor).
    std::function<std::uint32_t()> word_38;
    // actor +60 +2410[point] non-null (the inventory's model moves).
    std::function<bool(std::uint8_t point)> attached;
    // Checkpoint 131: runs each inventory event as it is made (the weapon
    // model library's 00145710 / 00145630 and the +2410 table; see
    // WeaponInventoryHost::apply); absent: models are not hosted.
    std::function<void(const WeaponInventoryEvent&)> apply;
    // 001a3520's world side (npc_dead_drop.hpp): the item list +3528 test,
    // the backpack object 00257640(actor, 1, 0) and its contents.
    std::function<bool()> items_3528;
    std::function<std::uint64_t()> create_backpack;
    std::function<void(std::uint64_t backpack,std::int32_t item,std::int32_t spare,std::int32_t clip)> add_backpack_weapon;
    std::function<void(std::uint64_t backpack,std::int32_t armor,std::int32_t max_armor)> add_backpack_armor;
    // The destroy 00254770 of a backpack this actor owns writes the actor's
    // +3464 = 0: true once that happened to the given backpack (unset: never).
    std::function<bool(std::uint64_t backpack)> backpack_cleared_3464;
    // N5b (npc_attack.hpp); with these and the perception link the update
    // runs the aim, look-at, posture and firing stages. aim: 0022f5b0 on the
    // actor's aim block (virtual +60; the block's skill +38 is the
    // component's) and aim_block its progress +20 / accuracy +24;
    // actor_action: the actor actions the AI sends with transport 0 that the
    // combat host handles (0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f; the host
    // queues them for its pump of 001a92a0); discharge: the gate's direct
    // 0013ff40(inv, 0, -1, 0, 0), returning the fire notices it made;
    // fire_notice_hook: the controller registers its fire notice +cc
    // (0014f460) for the discharges the host runs from the pump (001960c0)
    // and the burst continuation (00144080); eye_c0_of / position_f4_of: an
    // object's virtual +c0(subtarget) and +f4 row 3; byte_33e4 and
    // secondary_slot1_415: the actor's +33e4 and its secondary component's
    // aim slot 1 active byte (+415).
    std::function<void(std::uint64_t target,std::int32_t item,std::int32_t subtarget,float skill)> aim;
    std::function<std::array<float,2>()> aim_block;
    std::function<void(const NpcAiSend&)> actor_action;
    std::function<std::vector<NpcFireNoticeCall>()> discharge;
    std::function<void(std::function<void(std::uint64_t target,bool continuation)>)> fire_notice_hook;
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> eye_c0_of;
    std::function<RootVector(std::uint64_t object)> position_f4_of;
    std::function<std::uint8_t()> byte_33e4;
    std::function<bool()> secondary_slot1_415;
    // N5c, mode 3 AttackFromCover: 00142b50 of an actor-class object's
    // inventory +3524 (its weapon rating).
    std::function<std::int32_t(std::uint64_t object)> weapon_rating_of;
    // Checkpoint 132zf12, mode 9 Follow (00153870, npc_follow.hpp): an
    // object's virtual +f4 matrix, an actor-class object's +3584 ?
    // (+3584)+3c : +30e0, its +58 virtual +74 (nullopt: no +58), and an AI
    // actor's path follower +4c. Empty: the read throws.
    std::function<std::array<RootVector,4>(std::uint64_t object)> world_f4_of;
    std::function<float(std::uint64_t object)> speed_of;
    std::function<std::optional<RootVector>(std::uint64_t object)> velocity_74_of;
    std::function<std::int32_t(std::uint64_t object)> hold_4c_of;
    // Checkpoint 132uk5, mode 8 CarryBody (00153150, npc_carry_body.hpp): an
    // actor's skeleton node 14 world matrix (00133ed0(00132a30(o), *0049d024)).
    std::function<std::array<RootVector,4>(std::uint64_t object)> node_14_of;
};
// N4a/N4b: what the perception reads from the world (npc_perception_think.hpp).
// sense builds the observer's view for the actor (its eye, head node 14
// matrix, +e8 row 1, +33a8, +3470), the object views, the think list
// 004ea190 as actor handles, the local player and player slots, the team
// matrix, the visibility cache 0022cb10 (NpcVisibilityQuery) and the region
// lookups; object_by_id is 0040c540. Without a link the think frame's
// 0016b580 is skipped and the perception natives are deferred.
struct NpcPerceptionLink {
    std::function<NpcPerceptionHost(SoloActorSimulation& actor)> sense;
    std::function<std::uint64_t(std::int32_t id)> object_by_id;
};
struct NpcAiControllerFrame {
    std::vector<NpcAiMessage> delivered;        // pumped from the component's queue
    std::vector<NpcAiSend> sends;               // everything the component sent, in order
    std::vector<std::string> skipped;           // stages of the original not run
    bool think{};
    std::vector<PerceptionSend> perception_sends;
    std::vector<NpcDeathNoticeEvent> death_notice;
    std::vector<WeaponInventoryEvent> inventory_events;     // Activate's and the death notice's inventory calls
    // Checkpoint 132sx: the AIWeaponFiredMsgs (004ff2a0) this pump offered to the script filter.
    struct WeaponFired{std::uint64_t target{};bool script_handled{};};
    std::vector<WeaponFired> weapon_fired;
    // Checkpoint 132zf7: the AIPickupableAwarenessMsgs (004ff2a8) offered to the script filter.
    struct PickupableAwareness{std::uint64_t object{};bool script_handled{};};
    std::vector<PickupableAwareness> pickupable_awareness;
    // Checkpoint 132sx9: the cAIModeChangeMsgs (004ea3e8) offered to the script filter.
    struct ModeChange{std::uint8_t mode{};bool script_handled{};};
    std::vector<ModeChange> mode_change;
};
// 0014ac90's store (AI component virtual +1d8): the actor's +3469 after the
// call, given whether the actor is present (+2d, +2c, !+2e), the byte before
// and a1 (stored as its low byte when it differs from the current value).
std::uint8_t npc_disguise_store(bool present,std::uint8_t byte_3469,std::uint32_t a1);
class NpcAiController final:public ActorAiController {
public:
    // The AI component as 001588d0 -> 0016f5b0 -> 00158bc0 leaves it (an
    // interpreter dump of the constructor; npc_ai.hpp defaults and +141 =
    // 0x34, interpreter goldens over filled blocks), with the path
    // follower 00161840(pf, actor, 100, start node). The constructor's one
    // draw of the game RNG (00127dd0, for the aggressiveness +6c = 0.25 + 0.5
    // r / 2^31) is taken from the given state. actor_id is the actor's +c.
    // Without a combat link the AI's later draws (0014d730) take the same
    // state, which must outlive the controller.
    NpcAiController(NpcAiSystem& system,const NpcNodeGraph& graph,std::int32_t start_node,std::uint32_t actor_id,
        std::uint64_t& random_state);
    // The route request handle refers to this object.
    NpcAiController(const NpcAiController&)=delete;
    NpcAiController& operator=(const NpcAiController&)=delete;
    void always_run(SoloActorSimulation& actor,float dt) override;
    void update(SoloActorSimulation& actor,float dt) override;
    void activated(SoloActorSimulation& actor) override;
    // Deactivate (component +4c, 00157ad0) as the actor's deactivation calls
    // it (deactivate_npc_ai: the Deactivated notification queued on the actor,
    // the think list, +159 = 1).
    void deactivated(SoloActorSimulation& actor) override;
    // DeactivateAnon (component +c0, 00157510; DeactivateForAllPlayers):
    // the actor's deactivation (+60(1)) when it is present.
    void deactivate_anonymous(SoloActorSimulation& actor);
    // Deactivate (component +4c, 00157ad0) called by a script
    // (Deactivate(NPC), 0014a050): a present actor's deactivation (+60(1)),
    // which re-enters +4c through deactivated(); otherwise the body itself.
    void deactivate(SoloActorSimulation& actor);
    // Component virtual +44 (00157a40): true unless the actor's life state
    // (+108) is dying or dead (2, 3), when it is the negation of virtual +164
    // (0014b610, npc_corpse_removable_164 on corpse_view).
    bool alive_or_eligible(SoloActorSimulation& actor) override;
    // Virtual +84 (0014f3f0, static): with a hold node (path follower +4c >= 0)
    // whose first link of type 7 exists (0015d280), 0015fe80 makes the link's
    // target the one-node route and sends path event 2 (transport 1, the
    // dispatcher at once: npc_route_ready starts the route, whose type-7 link
    // event makes +114 = 7, so the dying actor keeps following it); the code
    // becomes 2 (dataDeathFallOffLedge). Without a node array the code is
    // returned unchanged, as it is without such a link.
    std::uint8_t death_code_84(SoloActorSimulation& actor,std::uint8_t code) override;
    // The kill's +1d6 = 1 (00186df8), read by the 0014bff0 tail (its sound
    // 0014d1a0 once actor +3204 bit 7 is set), and the dead landing's
    // +114 = 0 (0019e978; virtual +58 is 0015b7e0, which returns 1).
    void set_byte_1d6() override{component_.byte_1d6=1;}
    void clear_action_114() override{component_.action_114=0;}
    // Virtual +78 (0014f970): the perception's awareness of the attacker
    // (0016c9d0, cause 8, value 0.0) and AI event 8 (+28 = -1) to the
    // component, transport 0 (the dispatcher 00151d30 and Patrol 00156f90
    // ignore it). Needs the combat link's object view. 0016c9d0's dead-object
    // notice (AIPickupableAwarenessMsg 004ff2a8, to the component, transport
    // 0) is sent before the event.
    void hit_notice_78(SoloActorSimulation& actor,std::uint64_t source,std::int32_t amount) override;
    // Virtual +7c (0014f680, npc_death_notice.hpp) with the combat link's
    // inventory, killer view and RNG. The actor's +3468 (SetPickupable,
    // 00149230 -> virtual +1d4 = 0014a890; no TORONTO1 call site) is 0 from the
    // constructor and +3488 is written only by the ignition 00186360, which
    // is not hosted, so both are 0 here. The script death notice (004ff278)
    // is queued on the actor as an AI notification.
    void death_notice_7c(SoloActorSimulation& actor,const ActorDeathNotice& notice) override;
    // 001a3520 with the combat link's inventory and backpack host. Actor
    // +3464 is kept here (001a3520 reads and writes it; the backpack's
    // destroy 00254770 clears it, NpcCombatLink::backpack_cleared_3464); +3468
    // is 0 (see death_notice_7c).
    void dead_state_drop(SoloActorSimulation& actor) override;
    std::uint64_t backpack_3464() const{return backpack_3464_;}
    // The animation controller message (0055a1d8): queued for the pump (the
    // dispatcher ignores it for a dead actor, life state 3).
    void controller_message(SoloActorSimulation& actor,std::int32_t code) override;
    // The same message delivered at once (transport 1): 00151d30 now.
    void controller_message_immediate(SoloActorSimulation& actor,std::int32_t code) override;
    // Port-only (--coop-partner, coop_partner.hpp): while held, the animation
    // controller code 6 (JumpGrabLedge's token 0x14) is kept instead of being
    // answered (00151ad0 answers it at once with 0x11 / 0x13); release
    // delivers a kept code 6 through controller_message. A second player's
    // component (00176660) answers code 6 only with the hang bit and climbs
    // later from the hang; the co-op partner's driver releases it there.
    void port_hold_code6(bool hold){port_hold_code6_=hold;}
    bool port_code6_held() const{return port_code6_held_;}
    void port_release_code6(SoloActorSimulation& actor);
    // Port-only (checkpoint 132ob7, --coop-partner fire discipline): while held, the firing gate 00150340 makes
    // its decision but does not shoot (NpcAttackHost::port_hold_fire); its cooldown and burst pause still run.
    void port_hold_fire(bool hold){port_hold_fire_=hold;}
    // PORT-ONLY (checkpoint 132ln5h): a LAN remote human's figure. The original's remote human is a human whose
    // controller (002d85d0, vtable 004de530) replays the other console's network block; the port stands it in with
    // an NPC figure, so its AI think (update) and activation radius (always_run) are skipped and only the remote
    // controller's actions (LanNetCore) drive it.
    void port_remote_controlled(bool on){port_remote_controlled_=on;}
    bool port_is_remote_controlled() const{return port_remote_controlled_;}
    // 132ln5h13: 0014bff0's network game hooks (NpcAiHost::network_contact / network_speed / network_heading),
    // copied into every host(); set by the play for a generator NPC of a LAN game.
    struct NetworkHooks {
        std::function<void(std::uint8_t contact_158,const RootVector& position)> contact;
        std::function<void(float speed)> speed;
        std::function<void(float yaw)> heading;
    };
    void set_network_hooks(NetworkHooks hooks){network_hooks_=std::move(hooks);}
    // 132ln5h13 (npc_remote.hpp): the remote NPC controller 0x004de2a0 as this controller's remote mode.
    // set_remote is 001b3d98's kind 2 swap: 002d6030 (the base 00158740: a fresh component with the path follower at
    // the start node, +141 = 0x34 and the aggressiveness draw; the install's copies from this AI), then 001ae450's
    // destruction of the AI (00158550 with the actor not eligible: 00157ad0's body when active, else 00163c50, then the
    // route buffer), and the remote's Activate 002d40a0 when the actor is eligible. make_fresh_ai is kind 1: 00158800
    // (a generator's actor; +150 stays on the block) or 001588d0, the remote's destructor 002d5fa0, and the AI's
    // Activate 00157df0 when eligible. In remote mode update is 002d5160, activated 002d40a0, deactivated 002d4460,
    // the radius check is not made (+3c is the AI's but 0015a440 is not reached for it, PORT-ONLY: skipped), +78 does
    // nothing, +7c is 002d4640 (throws) and +84 answers 1.
    void set_remote_link(NpcRemoteLink link){remote_link_=std::move(link);}
    void set_remote(SoloActorSimulation& actor);
    void make_fresh_ai(SoloActorSimulation& actor);
    bool is_remote() const{return remote_.has_value();}
    // 132ln5h14c: +54 / +58 / +5c for the damage path: the remote NPC controller's 002d2a90 / 002d2aa0 / 002d2b20
    // answer 0 / 0 / 1; the LAN remote human's figure (PORT-ONLY stand-in for 0x004de530, whose +54 is 002d7c70) 1 / 0 / 0;
    // the AI's 0 / 1 / 0.
    ActorDamageController damage_predicates() const override{
        if(port_remote_controlled_)return {true,false,false};
        if(remote_)return {false,false,true};
        return {false,true,false};
    }
    const std::optional<NpcRemoteState>& remote_state() const{return remote_;}
    std::uint8_t controller_88() const{return remote_?2:1;}
    bool port_fire_held() const{return port_hold_fire_;}
    // Shots the gate decided while held, counted for the proofs.
    std::uint32_t port_held_shots() const{return port_held_shots_;}
    // Registers the fire notice with the link's fire_notice_hook (N5b).
    void set_combat_link(NpcCombatLink link);
    // The fire notice +cc (0014f460) for a discharge the combat host ran (the
    // pump's 001960c0, the burst continuation of 00144080), with the actor
    // this controller was last hosted with.
    void fire_notice(std::uint64_t target,bool continuation);
    void set_perception_link(NpcPerceptionLink link){perception_link_=std::move(link);}
    // The component's script filter +38 (0014a6c0 -> 003e1aa0) for the
    // weapon-fired notice (AIWeaponFiredMsg, 004ff2a0) the pump 003c8de0
    // offers it first; true: a script handler took it. Unset: none (the
    // message then goes to 00151d30, which does not test its key).
    void set_weapon_fired_filter(std::function<bool(std::uint64_t target,bool thrown)> filter){weapon_fired_filter_=std::move(filter);}
    // The same filter for the perception's dead-object notice
    // (AIPickupableAwarenessMsg, 004ff2a8; object = +24). Unhandled it goes
    // to 00151d30, which never loads its key (003cb210).
    void set_pickupable_awareness_filter(std::function<bool(std::uint64_t object)> filter){pickupable_awareness_filter_=std::move(filter);}
    // Checkpoint 132sx9: the same filter for the mode-change notice the AI
    // event 1 sends to self (cAIModeChangeMsg 004ea3e8, vtable 004d92e0; +24
    // the new mode, read by GetAiMode 0014a6e0). Unhandled it goes to
    // 00151d30, whose default handler 003cb210 returns 0 for it.
    void set_mode_change_filter(std::function<bool(std::uint8_t mode)> filter){mode_change_filter_=std::move(filter);}
    // The actor was created by a generator (001b7b00), not the level load:
    // an unwritten route buffer's point then throws (NpcAiHost::level_loaded).
    void set_generator_spawned(){level_loaded_=false;}
    // The owner's +3598 (its virtual +7c, 00184cd0), which ResetNpc 00148600 tests first.
    bool generator_spawned() const{return !level_loaded_;}
    bool perception_hosted() const{return static_cast<bool>(perception_link_.sense);}
    NpcPerception& perception(){return perception_;}
    // The AI script natives on the perception and modes (npc_alert.hpp); they
    // need the perception link. object: the target's actor handle.
    void script_add_awareness(SoloActorSimulation& actor,std::uint64_t object);
    void script_add_awareness_node(SoloActorSimulation& actor,std::int32_t node);
    void script_add_awareness_of_players(SoloActorSimulation& actor);
    void script_set_target(SoloActorSimulation& actor,std::uint64_t object);
    void script_set_target_node(SoloActorSimulation& actor,std::int32_t node);
    // SetGrenadeTarget 00149810 -> +19c = 0014b410(c, object, 4.0, 0) (132sx10).
    void script_set_grenade_target(SoloActorSimulation& actor,std::uint64_t object);
    void script_set_grenade_target_node(SoloActorSimulation& actor,std::int32_t node);   // the node object npc_node_object(node)
    void script_resume_ai_targeting(SoloActorSimulation& actor);
    void script_resume_ai_mode(SoloActorSimulation& actor);
    void script_look_around(SoloActorSimulation& actor);
    void script_stop_and_look_around(SoloActorSimulation& actor);           // 00149e20 -> +138 (132sx10)
    // RequestAction (00149ce0: lbu of the enum word, virtual +150 = 0014d8f0:
    // f12 = 0, then 0014d900(c, action, 100, 0)).
    void script_request_action(SoloActorSimulation& actor,std::uint8_t action);
    // RequestFacial (00149e80 -> +12c = 0014dea0, look entry 2). The entry is
    // aged by the look-at selection (0014e0c0) and faced by 0014bff0 with the
    // object's eye +c0, both of the attack stages: facial_hosted is the
    // perception link with the attack stages (otherwise the native is
    // deferred).
    bool facial_hosted() const{
        return perception_link_.sense&&combat_.aim&&combat_.aim_block&&combat_.actor_action&&combat_.discharge&&combat_.inventory&&combat_.database;
    }
    void script_request_facial(SoloActorSimulation& actor,std::uint64_t object,float duration);
    // The same with a node object (N5c: ID +c, eye +c0 -> +e4 = +80, w 1).
    void script_request_facial_node(SoloActorSimulation& actor,std::int32_t node,float duration);
    // RequestLookAt (00149ed0 -> +128 = 0014dfb0, look entry 0 and +80), aged
    // and faced by the same attack stages as RequestFacial (facial_hosted).
    // ResumeAILookAt (00149df0 -> +13c = 0014de30(c, 100)) and DeactivateDead
    // (00149fe0 -> +160 = 0014b680: +141 bit 2) write the component only.
    void script_request_look_at(SoloActorSimulation& actor,std::uint64_t object,float duration);
    void script_request_look_at_node(SoloActorSimulation& actor,std::int32_t node,float duration);
    void script_resume_look_at();
    void script_deactivate_dead(bool deactivate);
    // N5a (npc_script_control.hpp): MoveToNode (0014a5c0 -> +100 = 0014be50)
    // and the three-argument SetMode (0014a4a0 -> +dc = 0014bcf0 with a
    // node, which routes at once through NpcAiSystem::routes).
    std::uint8_t script_move_to_node(std::int32_t node);
    // Checkpoint 132ob8: the go-to-object virtual +104 (0014be10) with an
    // object's ID: mode 7 at priority 100 with +f4 = the ID and +138 = -1.
    // Also the script's MoveToGobj (0014a580; checkpoint 132sx10).
    std::uint8_t move_to_object(std::int32_t object_id);
    // The component's StopMovement (virtual +154 = 0014d4d0: 0014d510(c, 0,
    // priority), +7c = duration) and ResumeMovement (virtual +158 = 0014d470;
    // also called when +7c runs out). Used by the port-only co-op partner.
    void stop_movement(SoloActorSimulation& actor,std::int32_t priority,float duration);
    void resume_movement(SoloActorSimulation& actor);
    // Port-only (--coop-partner, coop_partner.hpp): no script native routes a
    // live NPC to a point. For an NPC already in mode 7 (after MoveToNode, no
    // mode request pending) this makes the request mode 7's enter makes toward
    // a body (MoveToGobj 0014be10, 00154660..0015480c): the region of the
    // point (0015d790 from the path follower's +5c), the route 0015fa70 and
    // +e8 = -1. The route answer (path event 2) then starts the movement as
    // for MoveToNode; path event 3 (no node with a clear line to the point)
    // leaves it standing. Throws in any other mode. 0015d790's f20 is the dt
    // of this controller's last update, as in mode 7's enter (132uk6d: the
    // base update sends the enter with f20 = its dt, 0016f2ec).
    std::optional<std::uint8_t> port_route_to_point(SoloActorSimulation& actor,const RootVector& point);
    // Port-only: the last route answer (path event 2 or 3) delivered to this
    // controller since port_route_to_point, taken once.
    std::optional<std::uint8_t> port_take_route_event(){auto e=port_route_event_;port_route_event_.reset();return e;}
    // Port-only (checkpoint 132ob5, --coop-partner): this actor's route requests also search the co-op link
    // overlay (NpcNodeGraph::port_overlay, coop_partner.hpp).
    void port_route_overlay(bool on);
    // Port-only (checkpoint 132ob10, --coop-partner): npc_port_relocate_path_follower (TeleportToNode's 0015ff90
    // step) after the port moved the actor beside `node` itself.
    void port_relocate_path_follower(SoloActorSimulation& actor,std::int32_t node);
    // Port-only (checkpoint 132ob11): npc_port_drop_path_segment.
    void port_drop_path_segment();
    void script_set_mode_at_node(SoloActorSimulation& actor,std::uint32_t mode,std::int32_t node);
    // AddWeapon (00149970 -> component +1b8 = 0014b890: 00143df0(+3524, item,
    // 1, -, 0, 0), ammunition without preload) and DrawWeapon (00149a10 ->
    // +1ac = 0014bae0, npc_select_item) on the combat link's inventory;
    // DrawWeapon's burst draws the link's RNG. weapons_hosted: the link is
    // set (otherwise the natives are deferred). Both throw on an inventory
    // event the play host does not model (attach 0038c7d0 from record +13c,
    // the slot-6/7 byte +24a2).
    bool weapons_hosted() const{return combat_.inventory&&combat_.database;}
    void script_add_weapon(std::int32_t item);
    // Port-only (the CPU partner's kit, checkpoint 132pw5fix): AddWeapon with
    // 00143df0's preload a2 set, as a player's loadout Accept adds its items
    // (00145710(db, item, 0) before the attach). The kit's items need not be
    // in the level's preload list (001457c0), and without it record +13c is 0:
    // no attach, so nothing is drawn in the partner's hands or holsters.
    void port_add_weapon_loaded(std::int32_t item);
    void script_draw_weapon(std::int32_t item);
    // StoreWeapon (00149a80 -> +1a8 = 0014bbd0, npc_store_weapon) on the
    // combat link's inventory; the reroll draws the link's RNG when armed.
    void script_store_weapon();
    // Port-only caller (checkpoint 132ln5h14b, research/NET_INLEVEL.md 4.25):
    // 001b1710's 001b2d20 on a non-owner's NPC, 001439b0(+3524, item) on the
    // combat link's inventory: -1 holsters (00143b60), else the item's slot
    // (00143d20) is equipped (00142c50) only when it holds that item; no
    // burst reroll. True: the item is held.
    bool net_hold_item(std::int32_t item);
    // RemoveWeapon(NPC, int) 00149930 -> +1bc = 0014b790 (npc_remove_weapon).
    void script_remove_weapon(std::int32_t item);
    // SendNewAwarenessOfPlayers(NPC) 0014a2f0 -> +f8 = 0014a320: the
    // perception's +21 bit 0 (the perception think then takes non-enemy
    // players as candidates, npc_perception_think.cpp).
    void script_send_new_awareness_of_players();
    // game::ForceHolster 00185410 -> 00143cc0(actor +3524): 00143b60 unless +84 is 6 (no reroll).
    void script_force_holster();
    // game::GetActiveWeapon 001855c0: the held slot's item (+3524 +8 + 16 * +84), -1 when +84 is 6.
    std::int32_t script_active_weapon() const;
    // Checkpoint 132sx8 perception natives (need the perception link):
    // RemoveAwareness (0014a3c0 -> +e8 = 0014a400 -> 0016c8d0),
    // ResendNewAwarenessMessage (0014a390 / 0014a340 -> +f4 = 0014a380 ->
    // 0016c590; object 0: every record), InViewCone (00149270 -> +188 =
    // 0014afc0 -> 0016c1e0(p, object, 1)), ClearTarget (0014a1e0:
    // 0016ddb0(p, 0, 100)) and SetViewConeRadius / SetViewConeAngle
    // (00149320 / 001492b0 -> +180 / +184: perception +10 / +14).
    void script_remove_awareness(SoloActorSimulation& actor,std::uint64_t object);
    void script_resend_awareness(SoloActorSimulation& actor,std::uint64_t object);
    bool script_in_view_cone(SoloActorSimulation& actor,std::uint64_t object);
    void script_set_view_cone_radius(float radius);
    void script_set_view_cone_angle(float angle);
    // The node object of a node-collection index (0015c120 identity; the
    // index is validated against the level's graph).
    std::uint64_t script_node_object(std::int32_t node);
    // Checkpoint 132zf4 (TORONTO2 SEWER): ResumeMovement (00149c70 -> +158 =
    // 0014d470, resume_npc_movement), RestrictMovementToNodeGroup (00149500:
    // 0015c120 (identity) -> +1f4 = 0014bea0, npc_clear_allowed_regions) and
    // DontDropWeapons (00149040 -> +200 = 0014a7d0: *(actor +3524) +a8 = 1,
    // the byte the dead drop 001a3520 tests; false without the combat link's
    // inventory, then deferred).
    void script_resume_movement(SoloActorSimulation& actor);
    void script_restrict_to_node_group(std::int32_t node);
    bool script_dont_drop_weapons();
    // Checkpoint 132zf7 (TORONTO3 MALL): AddGroupToPathRegion (001494c0 ->
    // 0015c120 (identity) -> +1f8 = 0014bf60, npc_allow_region),
    // ActivateByRadiusOff (00148eb0 -> +1d0 = 0014ae30: +98 = 0, +141 bit 7
    // clear; the 005721c8 send is skipped in solo) and SetPickupable
    // (00149230 -> +1d4 = 0014a890: actor +3468, then +160 = 0014b680(c,
    // !value) in solo; for an actor in life state 2 or 3, a value moves the
    // component from the dead list 004ea370 to the think list 004ea190
    // (0015af50, 00138ab0) and 0 moves it back (0015ad70, 0014ab90)).
    void script_add_group_to_path_region(std::int32_t node);
    // RemoveGroupFromPathRegion (00149480 -> 0015c120 -> +1fc = 0014bf00,
    // npc_forbid_region; 132sx10).
    void script_remove_group_from_path_region(std::int32_t node);
    void script_activate_by_radius_off();
    void script_set_pickupable(SoloActorSimulation& actor,bool value);
    // Checkpoint 132sx10: the AI component's virtual +1d8 = 0014ac90 (both AI
    // vtables 004d9380 / 004de2a0): the current value is +3469 != 0 for a
    // present actor (+2d, +2c, !+2e), else 0; when (a1 & 0xff) differs it
    // stores the byte a1 at +3469 (the multiplayer send 002d7290(actor, 0x39,
    // 4, a1 & 0xff, 0, 0, 0) under 005721c8 && a2 is skipped in solo).
    // SetHasDisguise 001491f0 calls it with (arg != 0, 1); the pickup menu's
    // kind-4 take 00252298 with (0, 1). Goldens: tests/ai_disguise_goldens.hpp.
    static void script_set_disguise(SoloActorSimulation& actor,std::uint32_t a1);
    // game::ActivateBodyToss (00476740: 003cc820(object), message key
    // 00588038 with +24 = flag != 0, 003c9420 transport 0) sent to an NPC
    // actor: its receiver (001b3bf0..001b3ca8) hands it to the controller's
    // virtual +c when the controller's +88 is neither 2 nor 4 (the AI
    // component's is 1), so it is queued for this controller's pump.
    void script_activate_body_toss(bool flag);
    // Checkpoint 132zf5: Teleport (00149c30 -> 0015c120 (identity) -> +10c =
    // 0014afe0, npc_teleport_to_node).
    void script_teleport(SoloActorSimulation& actor,std::int32_t node);
    // Checkpoint 132zf5: the four-argument ForceWeaponFire (00149b50: a1 =
    // shots, a2 = item, f12 = delay -> +198 = 0014b470, npc_force_weapon_fire
    // with the attack stages' held item +1a4 and select +1ac). An item < 0
    // only stores; an item needs the attack stages (weapons_hosted and the
    // perception and combat links), otherwise it throws.
    void script_force_weapon_fire(SoloActorSimulation& actor,std::int32_t shots,std::int32_t item,float delay);
    // 0016c9d0(+1ac, source, cause, value) from outside the AI, e.g. the
    // flashlight's per-frame Sight requests (FlashlightSightRequest: cause 6,
    // 0.0; FLASHLIGHT.md section 6.2). Needs the perception link.
    void perception_awareness(SoloActorSimulation& actor,std::uint64_t source,std::uint8_t cause,float value);
    // 00159020's view of this component and its actor: +3468 is the actor's
    // SetPickupable byte, +354c is 0 natively (the +354c attachment writers
    // 00185e70 / 001860c0 are not hosted), +38 is 0x80 (constructor and draw
    // pass), the eye comes from the combat link (throws without one).
    NpcCorpseView corpse_view(SoloActorSimulation& actor) const;
    const NpcPerception& perception() const{return perception_;}
    // +141 bit 7 (ActivateByRadius).
    bool radius_controlled() const override{return (component_.flags_141&0x80U)!=0;}
    void snapshot() override;
    void pump(SoloActorSimulation& actor) override;
    const NpcAiComponent& component() const{return component_;}
    NpcAiComponent& component(){return component_;}
    float aggressiveness() const{return aggressiveness_6c_;}
    // ActivateByRadius (0014ad30); the squared eye distance of 003ce850
    // must then be supplied (the +c0 eye positions are not native).
    void activate_by_radius(float radius,std::function<std::optional<float>()> closest_player_distance_squared);
    // The record of the frame being stepped (cleared by the pump).
    const NpcAiControllerFrame& frame() const{return frame_;}
    const std::vector<NpcAiControllerFrame>& history() const{return history_;}
    void keep_history(bool keep){keep_history_=keep;}
    // Actor +346b, written by SetNotGasable (00148f80 -> component +210 =
    // 0014a750 -> 00199040(actor, value != 0, 0); the multiplayer send is
    // skipped in solo). Gas is not hosted, so nothing reads it natively.
    std::uint8_t not_gasable_346b() const{return not_gasable_346b_;}
    void set_not_gasable(bool value){not_gasable_346b_=value?1:0;}
private:
    NpcAiHost host(SoloActorSimulation& actor);
    void script_weapon(const char* what,const std::function<void(WeaponInventoryOps&)>& body);
    // npc_remote.cpp
    void remote_update(SoloActorSimulation& actor,float dt);                 // 002d5160
    void remote_activate(SoloActorSimulation& actor);                        // 002d40a0
    void remote_deactivate(SoloActorSimulation& actor,bool destructor);      // 002d4460 (002d5fa0: +150 = -1 first)
    void remote_dead(SoloActorSimulation& actor,NpcAiHost& h);               // 002d5b58
    void remote_relocate(NpcAiHost& h);                                      // 0015ffd0(pf, +208)
    void fresh_component();                                                  // 00158740 / 00158bc0's fields and draw
    void destroy_component(SoloActorSimulation& actor);                      // 00158550
    std::function<void()> generator_release();                               // NpcAiDeactivationHost::generator_release
    std::int32_t start_node_{-1};
    std::optional<NpcRemoteState> remote_;
    NpcRemoteLink remote_link_;
    NpcAiSystem& system_;
    const NpcNodeGraph& graph_;
    std::uint32_t actor_id_;
    std::uint64_t* random_state_;   // the game RNG state (00127dd0) when the combat link has none
    bool level_loaded_{true};
    std::shared_ptr<NpcRouteClient> route_client_;
    SoloActorSimulation* route_actor_{};
    std::optional<std::uint8_t> port_route_event_;
    bool port_hold_code6_{},port_code6_held_{};
    bool port_remote_controlled_{};
    NetworkHooks network_hooks_;
    std::optional<float> port_last_dt_;                                         // port-only (132uk6d): port_route_to_point's f20
    bool port_hold_fire_{};std::uint32_t port_held_shots_{};
    NpcAiComponent component_;
    float aggressiveness_6c_{};
    std::deque<NpcAiMessage> queue_,pending_;
    NpcAiControllerFrame frame_;
    std::vector<NpcAiControllerFrame> history_;
    bool keep_history_{};
    std::function<std::optional<float>()> player_distance_;
    std::uint8_t not_gasable_346b_{};
    NpcPerception perception_;
    NpcCombatLink combat_;
    NpcPerceptionLink perception_link_;
    std::function<bool(std::uint64_t,bool)> weapon_fired_filter_;
    std::function<bool(std::uint64_t)> pickupable_awareness_filter_;
    std::function<bool(std::uint8_t)> mode_change_filter_;
    std::optional<NpcPerceptionHost> sense_;
    std::unique_ptr<NpcAttackHost> attack_;
    std::unique_ptr<NpcCarryHost> carry_;                      // Checkpoint 132uk5: mode 8 (npc_carry_body.hpp)
    std::unique_ptr<NpcGoToBodyHost> go_to_body_;              // Checkpoint 132ms4: mode 7 toward a body (npc_script_control.hpp)
    std::unique_ptr<NpcFollowHost> follow_;                    // Checkpoint 132zf12: mode 9 (npc_follow.hpp)                    // built once both links are complete
    std::uint64_t backpack_3464_{};
};
}
