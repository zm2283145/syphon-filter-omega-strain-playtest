#pragma once
// The mission scripts' NPC natives (MissionScriptNpcs, mission_script_host.hpp)
// on an ActorWorld whose AI NPCs are actors with NpcAiController components
// (NPC_SPAWN_AND_AI.md N2/N3): the play sandbox with --mission-scripts and the
// synthetic chain tests. The NPCs are built inactive, as the loader 001c1e70
// leaves role-2 records (+2d = 0); the hosted natives activate them.
#include "actor_world.hpp"
#include "mission_script_host.hpp"
#include "npc_world.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace omega {

class ActorWorldScriptNpcs final : public MissionScriptNpcs {
public:
    // player_actor: the local player's actor (the only player slot of
    // 003fb8d0 in single player).
    ActorWorldScriptNpcs(ActorWorld& world, NpcAiSystem& ai_system, std::size_t player_actor);
    ActorWorldScriptNpcs(const ActorWorldScriptNpcs&) = delete;
    ActorWorldScriptNpcs& operator=(const ActorWorldScriptNpcs&) = delete;
    // The NPC record with this population ID is actor `actor`, driven by
    // `controller`. Both actor constructors write +30a4 = -FLT_MAX (001b6ab0
    // at 001b6b48, 001b7b00 at 001b7bb0; GENERATOR_RUNTIME.md "Checkpoint
    // 119b"). An NPC added after connect (a generator's spawn) gets its
    // script filters at once.
    void add(Word npc, std::size_t actor, NpcAiController& controller);
    // Each NPC actor's pump offers its script AI notifications and hotbox link
    // messages to the host (MissionScriptHost::deliver_ai_notification /
    // deliver_link_message) first (003c8de0 filter +0x38, 003cc850); the
    // DamageMsg its base damage queued goes to deliver_damage, also for the
    // player actor (whose queue takes no other AI notification).
    void connect(MissionScriptHost& host);
    // Port-only (--coop-partner, coop_partner.hpp CoopLinkOverlay): node
    // indices from `first` on are overlay nodes the original never had. A
    // waypoint / destination notification (path events 5 / 6) naming one is
    // not offered to the scripts (only recorded, as an unhandled one), since
    // no script object exists for it. -1 (default): no overlay. The connected
    // host's node natives take it too (MissionScriptHost::set_port_overlay_first_node).
    void set_port_overlay_first_node(std::int32_t first) {
        port_overlay_first_ = first;
        if (host_) host_->set_port_overlay_first_node(first);
    }
    // The population IDs of the hosted NPCs: the actors type-1/6/7 box links
    // may name (SoloActorConfig::hotbox_link_actors, select_hosted_hotboxes).
    std::vector<Word> link_actor_keys() const;
    // Routes a hotbox link message (EnableMsg / DisableMsg, transport 0) to
    // its NPC's actor (queue_link_message); another receiver goes to other
    // (the play's generators: true when it took the message), and one neither
    // takes throws. connect_hotbox_links makes it the box owner's link sink
    // and routes its boxes' load-time DisableMsgs (00172ae0), to be delivered
    // by the first pump.
    using OtherLinkReceiver = std::function<bool(const HotboxLinkMessage&)>;
    void route_link_message(const HotboxLinkMessage& message);
    void connect_hotbox_links(SoloActorSimulation& box_owner, OtherLinkReceiver other = {});
    NpcAiComponent* component(Word npc) override;
    void activate(Word npc) override;
    bool activate_component(Word npc) override;
    void deactivate_anonymous(Word npc) override;
    bool deactivate(Word npc) override;
    // game::ResetNpc (00148600): nothing for a generator-spawned NPC (+3598);
    // otherwise Deactivate (+4c) and the reset host's destructor and rebuild
    // (the play's figure, actor and controller) at the same actor index (the
    // original rebuilds the record at its address); the NPC's script ID then
    // names the rebuilt controller, connected as the old one was.
    // False (deferred) without a reset host.
    struct ResetActor {
        std::size_t actor{};
        NpcAiController* controller{};
    };
    using ResetHost = std::function<ResetActor(Word npc, std::size_t old_actor)>;
    void set_reset_host(ResetHost host) { reset_ = std::move(host); }
    bool reset_npc(Word npc) override;
    // Checkpoint 132ln5h8 (the play's LAN remote human): the NPC's actor was
    // rebuilt in place (ActorWorld::rebuild_ai) with a new controller; the
    // script ID names it from now on, connected as the old one was.
    void replace_controller(Word npc, NpcAiController& controller);
    bool deactivate_actor(Word npc) override;
    // 0014ad30 on the component; the radius check 0015a440 then reads
    // actor_closest_player_distance_squared(actor) (003ce850).
    void activate_by_radius(Word npc, float radius) override;
    void set_not_gasable(Word npc, bool value) override;
    // The perception natives on the NPC's NpcAiController (false: its
    // perception link is not set; object nullopt = the player's actor).
    bool add_awareness(Word npc, std::optional<Word> object) override;
    bool add_awareness_of_players(Word npc) override;
    bool set_target(Word npc, std::optional<Word> object) override;
    bool set_target_node(Word npc, std::int32_t node) override;
    bool add_awareness_node(Word npc, std::int32_t node) override;
    bool add_awareness_object(Word npc, Word object) override;
    bool set_target_object(Word npc, Word object) override;
    bool set_grenade_target(Word npc, const GobjTarget& target) override;
    bool resume_ai_targeting(Word npc) override;
    bool resume_ai_mode(Word npc) override;
    bool look_around(Word npc) override;
    bool stop_and_look_around(Word npc) override;
    bool request_action(Word npc, std::uint8_t action) override;
    bool request_facial(Word npc, std::optional<Word> object, float duration) override;
    bool request_facial_node(Word npc, std::int32_t node, float duration) override;
    bool request_facial_object(Word npc, Word object, float duration) override;
    bool request_facial_hotbox(Word npc, Word box, float duration) override;
    bool request_look_at(Word npc, std::optional<Word> object, float duration) override;
    bool request_look_at_node(Word npc, std::int32_t node, float duration) override;
    bool request_look_at_object(Word npc, Word object, float duration) override;
    bool request_look_at_hotbox(Word npc, Word box, float duration) override;
    bool resume_look_at(Word npc) override;
    bool deactivate_dead(Word npc, bool value) override;
    // EquipGoggles through the goggles host (set_goggles_host: the play's
    // combat session owns the secondary components and inventories); false
    // without one.
    bool equip_goggles(Word npc, bool on) override;
    void set_goggles_host(std::function<void(std::size_t actor, bool on)> host) { goggles_ = std::move(host); }
    // MoveToNode / the three-argument SetMode on the NPC's NpcAiController
    // (the route itself needs NpcAiSystem::routes when mode 7 enters).
    std::optional<std::uint8_t> move_to_node(Word npc, std::int32_t node) override;
    std::optional<std::uint8_t> move_to_object(Word npc, Word object) override;
    bool set_mode_at_node(Word npc, std::uint32_t mode, std::int32_t node) override;
    std::optional<std::int32_t> hit_points(Word npc) override;
    // On the NPC's controller; false while its combat link is not set.
    bool add_weapon(Word npc, std::int32_t item) override;
    bool draw_weapon(Word npc, std::int32_t item) override;
    // cNPC_Kill 0014b370 on the NPC's actor through the damage host
    // (set_damage_host: the play's combat actors' virtual +100, source =
    // victim); false without one.
    bool kill(Word npc) override;
    // Checkpoint 132zf4 (TORONTO2 SEWER): SetHitPoints / SetInvulnerable on
    // the NPC's actor fields, DontDropWeapons / ResumeMovement /
    // RestrictMovementToNodeGroup on its controller, SetOnFire through its
    // fire hooks' ignite (false without them, or without the combat link's
    // inventory for DontDropWeapons).
    bool set_hit_points(Word npc, std::int32_t hp) override;
    bool set_invulnerable(Word npc, bool value) override;
    bool dont_drop_weapons(Word npc) override;
    bool set_on_fire(Word npc) override;
    bool resume_movement(Word npc) override;
    // Checkpoint 132zf9: CarryBodyToNode (0014bd60).
    std::optional<std::uint8_t> carry_body_to_node(Word npc, Word body, std::int32_t node) override;
    // Checkpoint 132zf11: Follow (001488f0).
    bool follow(Word npc, std::optional<Word> target, float a, float b) override;
    bool restrict_to_node_group(Word npc, std::int32_t node) override;
    // Checkpoint 132zf5: Teleport on the NPC's controller (0014afe0).
    bool teleport(Word npc, std::int32_t node) override;
    // Checkpoint 132zf7: SetUnconscious (through the damage host),
    // SetPickupable, ActivateByRadiusOff, AddGroupToPathRegion.
    bool set_unconscious(Word npc) override;
    bool set_pickupable(Word npc, bool value) override;
    bool set_has_disguise(Word npc, bool value) override;
    bool activate_body_toss(Word object, bool flag) override;
    bool activate_by_radius_off(Word npc) override;
    bool add_group_to_path_region(Word npc, std::int32_t node) override;
    bool remove_group_from_path_region(Word npc, std::int32_t node) override;
    // Checkpoint 132zf5: the four-argument ForceWeaponFire (00149b50 -> 0014b470).
    bool force_weapon_fire(Word npc, std::int32_t shots, std::int32_t item, float delay) override;
    // Checkpoint 132sx8: the perception natives on the NPC controller's
    // perception (false when the perception link is not hosted), StoreWeapon
    // (false without the combat link's inventory), SetNotTaserable /
    // SetFireInvulnerability on the actor fields and StopMovement.
    bool remove_awareness(Word npc, const AiObject& object) override;
    bool resend_new_awareness(Word npc, const std::optional<AiObject>& object) override;
    bool clear_target(Word npc) override;
    std::optional<bool> in_view_cone(Word npc, const AiObject& object) override;
    bool set_view_cone_radius(Word npc, float radius) override;
    bool set_view_cone_angle(Word npc, float angle) override;
    bool store_weapon(Word npc) override;
    bool remove_weapon(Word npc, std::int32_t item) override;
    bool send_new_awareness_of_players(Word npc) override;
    bool force_holster(Word npc) override;
    std::optional<std::int32_t> active_weapon(Word npc) override;
    bool set_not_taserable(Word npc, bool value) override;
    bool set_fire_invulnerability(Word npc, bool value) override;
    bool stop_movement(Word npc, float duration) override;
    bool set_footstep_sound_radius(Word npc, float radius) override;
    // Checkpoint 132zf5: the interaction triggers and CAS codes an NPC's actor
    // sends an InteractGOBJ (the NotifyDone / NotifyInterrupted of its
    // PerformAction_WithObject motion). As for the player (003c8de0), the
    // object's script filter takes the trigger first, with the NPC as Who;
    // the rest go to the native receiver 00216580 (trigger, keyed by the
    // NPC's population ID; false when unset, which throws in the actor).
    using InteractTrigger = std::function<bool(Word npc, const HotboxTrigger&)>;
    using InteractCode = std::function<void(Word npc, std::uint64_t object, std::int32_t code)>;
    void set_interact_receiver(InteractTrigger trigger, InteractCode code) {
        interact_trigger_ = std::move(trigger);
        interact_code_ = std::move(code);
    }
    bool has_npc(Word npc) const { return npcs_.count(npc) != 0; }
    using DamageHost = std::function<void(std::size_t victim, std::uint8_t category, std::uint8_t type, float amount)>;
    void set_damage_host(DamageHost host) { damage_ = std::move(host); }
    // 00132860 on the NPC's actor (SoloActorSimulation::position_e4: the load
    // placement before its first frame); nullopt when not hosted.
    std::optional<RootVector> position_e4(Word npc) override;
    // 001710a0's reads on the NPC's actor: the transform component +40
    // (ActorWorld component()) and +2e80 (human_context_bounds of the
    // skeleton, as ActorWorldCombat::bounds_88); nullopt when not hosted.
    std::optional<InteractableInputs> interactable_box_inputs(Word npc) override;
    // The population ID of the NPC whose actor has this handle; nullopt for
    // the player's actor; otherwise the record the other-source resolver
    // names (a FIR: zone as a damage source); throws for any other object.
    std::optional<Word> record_of_actor(std::uint64_t handle) const;
    void set_other_source(std::function<std::optional<Word>(std::uint64_t)> resolver) { other_source_ = std::move(resolver); }
    // A damage source no record stands for (an actor's burning zone): its
    // DamageMsg / AIDeadMsg go out with the Attacker getter unanswered.
    void set_unnamed_source(std::function<bool(std::uint64_t)> test) { unnamed_source_ = std::move(test); }
    bool unnamed_source(std::uint64_t handle) const;
    // A population GOB's object handle (gob_host_handle) by record ID; unset:
    // RequestFacial of a GOB throws.
    void set_object_handle(std::function<std::optional<std::uint64_t>(Word)> resolver) { object_handle_ = std::move(resolver); }
    // A hotbox's object handle (hotbox_host_handle) by its +0c key; nullopt
    // (or unset) when the world does not host the box: RequestFacial of it is
    // then deferred.
    void set_hotbox_handle(std::function<std::optional<std::uint64_t>(Word)> resolver) { hotbox_handle_ = std::move(resolver); }
    float closest_player_distance_squared(Word npc) override { return actor_closest_player_distance_squared(actor_of(npc)); }
    NpcAiSystem& system() override { return system_; }
    // 00188d80(actor, -1): a present actor's skeleton node 5 (00132910 through
    // its model), otherwise its model transform component's translation.
    RootVector eye_position(std::size_t actor) const;
    // 003ce850 for an actor of the world over the single player slot.
    float actor_closest_player_distance_squared(std::size_t actor) const;
    std::size_t actor_of(Word npc) const;
    NpcAiController& controller(Word npc);

private:
    struct Entry {
        std::size_t actor{};
        NpcAiController* controller{};
    };
    void connect_one(Word npc, const Entry& e);
    const Entry& entry(Word npc) const;
    std::uint64_t actor_handle(std::optional<Word> npc) const;
    // 003cc820 (identity) of a script GOBJ argument as the NPC's perception
    // names it.
    std::uint64_t ai_object_handle(const Entry& e, const AiObject& object, const char* native) const;
    ActorWorld& world_;
    NpcAiSystem& system_;
    std::size_t player_actor_;
    std::map<Word, Entry> npcs_;
    MissionScriptHost* host_{};
    std::int32_t port_overlay_first_{-1};
    std::function<void(std::size_t actor, bool on)> goggles_;
    DamageHost damage_;
    OtherLinkReceiver other_link_receiver_;
    std::function<std::optional<Word>(std::uint64_t)> other_source_;
    std::function<bool(std::uint64_t)> unnamed_source_;
    std::function<std::optional<std::uint64_t>(Word)> object_handle_;
    std::function<std::optional<std::uint64_t>(Word)> hotbox_handle_;
    InteractTrigger interact_trigger_;
    InteractCode interact_code_;
    ResetHost reset_;
};

}
