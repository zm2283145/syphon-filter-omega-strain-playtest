#pragma once
// The CombatActors host over an ActorWorld (COMBAT.md, "CombatActors over
// ActorWorld"): each accessor reads the actor's native state (positions, eye
// nodes, bounds, flags, health, life state) or the per-actor model data the
// host supplies (the hit-volume part meshes and inverse binds, the hard-mesh
// attach points of 0038e100), keeps one secondary (upper-body) component per
// actor and routes the damage virtual +100 to SoloActorSimulation::apply_damage.
// What is not modelled throws with the missing piece named: the aim-view
// shot frame without a camera (camera +50 / +da0), characters without a hard mesh (default attach
// frames of 0038e100), bullet static collision and the static object's +c /
// +100 unless a host supplies them, the effect tables 00236f20 / 003ebfb0
// (unless the labelled diagnostic effects_not_drawn) and the float at 005383d8
// (the video object's measured frame rate +1e8) unless the host supplies it.
#include "actor_hit_volumes.hpp"
#include "actor_world.hpp"
#include "combat_world.hpp"
#include "model_attach_points.hpp"
#include "secondary_aim.hpp"
#include "secondary_component.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// A character's model data for the combat path, built by the host when the
// part meshes are decoded (the meshes must outlive the adapter).
struct CombatActorModel {
    std::vector<HitVolumePart> parts;            // render records +1ec0 / +20c0, in render order
    std::vector<ShotFrame> inverse_bind;         // skeleton nodes +d0
    std::optional<ModelAttachSlots> attach;      // 0038e100 (hard mesh); nullopt: not built
};
struct ActorWorldCombatConfig {
    std::vector<std::int32_t> object_ids;                    // actor +c per world index
    std::vector<std::optional<CombatActorModel>> models;     // per world index (nullopt: not supplied)
    // 00332f90(agent data, 0): the agent-data unlock the unarmed attack reads
    // (a game with no unlocks: false). Explicit; there is no default.
    std::optional<bool> agent_unlock_0;
    // 00277e20's LOS (00148290 -> 003c3a60 with the bullet masks); absent: throws.
    std::function<std::optional<BulletStaticHit>(const RootVector& start,const RootVector& displacement,std::uint64_t excluded)> static_hit;
    // A static hit object's +c and damage virtual +100 (absent: throws).
    std::function<std::int32_t(std::uint64_t object)> static_object_id;
    std::function<void(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount)> static_damage;
    // Diagnostic (presentation only): the effect table 00236f20 answers -1
    // and the effect service 003ebfb0(0x18) is absent, so the muzzle effect
    // and blood decal events are not produced. Off: both throw.
    bool effects_not_drawn{};
    // Checkpoint 131b: the effect table 004f7a50 (00236f20(i)'s entry +0; the
    // table is loaded, so null never comes back). With it the effect service
    // 003ebfb0 is present (operator new) and the muzzle effects are produced.
    std::function<std::int32_t(std::int32_t index)> effect_table;
    // Checkpoint 122f: actor +38 as the last scene frame 0013e1b0 left it (it
    // stores 0x80, then 2 for a shown object, then 003c8990's cull code when
    // vcall +88 gives bounds); absent: 0x80, the value cGOBJ_ctor (003cf0e0)
    // stores, as no scene frame is hosted.
    std::function<std::uint32_t(std::size_t actor)> scene_cull_38;
    // Checkpoint 122f: the float at 005383d8, the video object's +1e8
    // (005381f0 + 0x1e8): 0037a9f0's 576000 / (T1 ticks + 00533870), the
    // frame rate measured before the +1e4 limiter (0 until +1d8 reaches 2;
    // 0037e780 clears it). Absent: reading it throws.
    std::function<float()> frame_rate_1e8;
    // The camera +50 transform (its +10 matrix; +da0 is the copy) the player
    // controller's virtual +64 (00176400, a3 != 0) reads for the aim-view shot
    // frame; absent: an aim-view shot throws.
    std::function<ShotFrame(std::size_t actor)> aim_view_camera;
    // The light manager's pools (00535d20) for the torch branch of 00144080
    // (torch_light.hpp); nullptr: not hosted (holding a torch throws).
    LevelLights* lights{};
    // Checkpoint 131: 00385240's +2410[point] test and 00385370 (the attached
    // model's +130, added to the muzzle origin); absent: no model is attached.
    std::function<std::optional<RootVector>(std::size_t actor,std::uint8_t point)> attached_muzzle;
    // Checkpoint 131c: 00189560(bullet +d0), the static hit surface's material
    // row; absent: static impacts carry no presentation.
    std::function<const MaterialRow*(const BulletStaticHit& hit)> static_material;
};

// Checkpoint 132b: one 00188140 effect request 00232180(00188760(effect.table_index),
// effect.position, effect.direction) by the hit volumes of `actor`, and its place
// among the frame's combat events: spawned before the event at `event` (after
// every event when it is past them). A request made inside the bullet update
// first carries the index into that update's bullet events (`bullet`); the
// session resolves it.
struct ActorHitEffect {
    struct Cursor {
        std::size_t event{};
        bool bullet{};
    };
    std::size_t actor{};
    HitVolumeEffect effect{};
    Cursor at{};
};

class ActorWorldCombat final:public CombatActors {
public:
    ActorWorldCombat(ActorWorld& world,ActorWorldCombatConfig config);
    std::size_t size() const override;
    std::uint64_t object(std::size_t actor) const override;
    std::int32_t object_id(std::size_t actor) const override;
    bool player(std::size_t actor) const override;
    bool port_agent_slot(std::size_t actor) const override;
    bool byte_14(std::size_t actor) const override;
    bool alive_handle(std::size_t actor) const override;
    ActorFireState fire_state(std::size_t actor) override;
    ShotFrame shot_frame(std::size_t actor,bool second_hand,bool aim_view) override;
    // 00385240(&M, actor +60, k) for attach point k: the point's skeleton
    // node through the model component, then the point's offset, plus the
    // attached model's muzzle +130 (00385370) when +2410[k] holds one
    // (ActorWorldCombatConfig::attached_muzzle). shot_frame's chain.
    ShotFrame attach_point_frame(std::size_t actor,std::uint8_t index,const WeaponAttachPoint& point);
    // The model's 0038e100 attach slots (nullptr: not built; no model throws).
    const ModelAttachSlots* attach_slots(std::size_t actor) const;
    // An attach point's world matrix (00385240's 0019f350 of the point's local
    // rows through its node's world matrix): matrix 0 of 003870c0's GUN
    // matrices. nullopt when the actor has no model data, no 0038e100 table
    // or the point was not written.
    std::optional<ShotFrame> attach_point_world(std::size_t actor,std::uint8_t point);
    // The world matrix of an attach point's node (0019f350 of node byte +2450 + point through
    // the component), the hand history 0038a278 reads; nullopt like attach_point_world.
    std::optional<ShotFrame> attach_node_world(std::size_t actor,std::uint8_t point);
    std::optional<std::array<ShotFrame,2>> shell_spawn_frames(std::size_t actor,std::size_t hand) override;
    RootVector position_e4(std::size_t actor) override;
    RootVector eye_c0(std::size_t actor,std::int32_t subtarget) override;
    std::array<RootVector,2> bounds_88(std::size_t actor) override;
    std::uint32_t flags_3204(std::size_t actor) override;
    std::int32_t armor_33a0(std::size_t actor) override;
    bool invulnerable_33a4(std::size_t actor) override;
    std::uint8_t life_state(std::size_t actor) override;
    BulletHitVolumes hit_volumes(std::size_t actor,const RootVector& origin,const RootVector& direction,std::int32_t mode,float volume) override;
    void damage(std::size_t victim,std::optional<std::size_t> shooter,std::uint8_t category,std::uint8_t type,
        std::int32_t& penetration,float amount) override;
    void damage_from_object(std::size_t victim,std::uint64_t source,const RootVector& source_e4,std::uint8_t category,
        std::uint8_t type,std::int32_t& penetration,float amount) override;
    // The same virtual +100 with a null penetration pointer (t0 = 0: the
    // melee takedown 00196480), recorded like the others.
    void damage_without_penetration(std::size_t victim,std::optional<std::size_t> shooter,std::uint8_t category,std::uint8_t type,
        float amount);
    // Virtual +100 with a request built by the caller (the burn damage
    // 001b1fa0, the fire zone 00261df0), recorded like the others.
    void damage_request(std::size_t victim,const SoloDamageRequest& request);
    // The +58 controller predicates of an actor's damage party.
    ActorDamageController damage_controller(std::size_t actor);
    std::optional<BulletStaticHit> static_hit(const RootVector& start,const RootVector& displacement,std::uint64_t excluded) override;
    std::int32_t static_object_id(std::uint64_t object) override;
    void static_damage(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount) override;
    std::int32_t effect_id(std::int32_t index) override;
    const MaterialRow* static_material(const BulletStaticHit& hit) override;
    bool effect_service() override;
    std::uint32_t word_38(std::size_t actor) override;
    float value_005383d8() override;
    // 00197560 / 00197140 through the throw host (set_throw_start); without
    // one, CombatActors::start_throw (throws).
    void start_throw(std::size_t actor,std::int32_t hold_class,const ActorFireRequest& request) override;
    // 001414d0 / 00196480 through the melee host (set_melee_attack); without
    // one, CombatActors::melee_attack (throws).
    void melee_attack(std::size_t actor) override;
    void set_melee_attack(std::function<void(std::size_t actor)> melee);
    // The torch branch's host (lights set) or nullptr; the flicker object's
    // presentation calls are recorded for take_torch_calls.
    const TorchLightHost* torch_host(std::size_t actor) override;
    void set_torch_lights(LevelLights* lights);
    std::vector<std::pair<std::size_t,TorchFlickerCall>> take_torch_calls();
    // The throw flow's hosts (combat_throw_play.hpp; empty: not hosted, the
    // default): the throw start, and what 001cb160 does after its state step
    // (the step's posts delivered, then 001d7480 / 001d6120), called by the
    // secondary update with `first` = "+310 or +311 was 0 before the step".
    void set_throw_start(std::function<void(std::size_t actor,std::int32_t hold_class,const ActorFireRequest& request)> start);
    void set_secondary_playback(std::function<void(std::size_t actor,const SecondaryStep& step,bool first,float dt)> playback);

    // The secondary component +3314 of the actor (constructor 001d81a0: all
    // states 0). 001977f0(actor, raise) through 001cb120 on it.
    SecondaryComponent& secondary(std::size_t actor);
    SecondaryModeChoice apply_secondary_mode(std::size_t actor,const SecondaryModeInput& input);
    // The per-frame update 001cb160 of actor a: its aim half (secondary_aim.hpp)
    // over the actor's model (component placement, node matrices, node 0's
    // rotation cache, base aggregate) and aim slot 0, then the state step
    // 001cc8d4..001ccb18 with the aim angle. Recoil kicks draw the world's
    // game RNG. The upper-body IK after the state step is not ported.
    SecondaryStep step_secondary(std::size_t actor,float dt);
    // step_secondary for every eligible actor, in actor order (tests; the
    // original runs it inside each actor's second traversal: see
    // attach_secondary_updates).
    std::vector<SecondaryStep> step_secondaries(float dt);
    // Registers step_secondary as each actor's secondary update
    // (SoloActorSimulation::set_secondary_update), so ActorWorld::step runs it
    // after the actor's model update and physical step, as 00392aa0 does.
    // The steps are collected for take_secondary_steps. The adapter must
    // outlive the world's steps.
    void attach_secondary_updates();
    // A run-time actor the world has just appended (ActorWorld::add_ai; a
    // generator spawn, 001b7b00): its object ID +c and model, the
    // constructor-state entries above, and its secondary update once
    // attach_secondary_updates has run. Throws unless the world holds exactly
    // one actor more than the adapter.
    void append_actor(std::int32_t object_id,std::optional<CombatActorModel> model);
    // ResetNpc's in-place rebuild (001b8130) of actor `a`: the same object
    // ID, the new model and the constructor-state entries of append_actor.
    void reset_actor(std::size_t a,std::optional<CombatActorModel> model);
    // 001b5560's new +3314 (0x840 bytes, constructor 001d81a0) on a
    // re-activation: the secondary states, the aim half and the aim slots
    // +3a0 / +410 back to their constructor values (as append_actor); the
    // look fields of actor +60 are the actor's and stay.
    void recreate_secondary(std::size_t actor);
    // A body switch / restore on actor `actor` (00390fc0 / 00391210 then ActorModel_Setup 0038c920):
    // the hit model's render records are the new part list; the skeleton's inverse binds and the
    // attach slots stay (0038c920 does not run 0038e100). 00390940 frees each part's gore array
    // +2340 and the setup zeroes the pointers, so every part starts without gore; +2c08 stays.
    void replace_model_parts(std::size_t actor,std::vector<HitVolumePart> parts);
    std::vector<std::pair<std::size_t,SecondaryStep>> take_secondary_steps();
    // The aim half's retained state (+140..+30c, the layers) and aim slot 0
    // (+3a5 / +3b0, written by the aim actions, actor_aim_actions.hpp).
    SecondaryAimState& secondary_aim(std::size_t actor);
    ActorAimState& aim_state(std::size_t actor);
    // The last aim angle step_secondary computed for the actor.
    float last_aim_angle(std::size_t actor) const;
    // The whole result of that aim half (the stack vectors the pose pass after
    // the state step reads, upper_body_pass.hpp).
    const SecondaryAimResult& last_aim_result(std::size_t actor) const;
    // The animation receiver's view of +3524 (mode +84, +85, +a4, +ef, the
    // bursts +bc/+c0, the six slots' items and rounds, the object's byte +8)
    // filled from the combat inventory before the actors step, and the
    // fields the receiver writes copied back after it.
    void publish_inventory_views(const CombatWorld& combat);
    void collect_inventory_views(CombatWorld& combat);
    // The same for one actor.
    void publish_inventory_view(std::size_t actor,const CombatWorld& combat);
    void collect_inventory_view(std::size_t actor,CombatWorld& combat);
    // What the hit entry's +100 calls and the hit-volume queries produced
    // since the last take.
    std::vector<std::pair<std::size_t,SoloDamageOutcome>> take_damage();
    std::vector<ActorHitEffect> take_effects();
    // Checkpoint 132b: where a hit-volume effect falls among the frame's
    // combat events (the play's spawn order); unset: 0.
    void set_effect_cursor(std::function<ActorHitEffect::Cursor()> cursor){effect_cursor_=std::move(cursor);}
    // The actor's gore counter +2c08 (hit-volume gore state; 0 for an actor
    // the adapter does not hold). Nonzero forces draw LOD 0 (003870c0).
    std::uint32_t gore_2c08(std::size_t actor) const{return actor<gore_.size()?gore_[actor].level_2c08:0U;}
    // Checkpoint 132wb: the render's gore arrays +2340 (nullptr for an actor
    // the adapter does not hold) and 0038a118 on them (true when it
    // saturated, 0038b8d0). The attached secondary update runs it inside
    // 00392aa0 with the world's frame counter and the actor's draw mode.
    const HitVolumeGore* gore(std::size_t actor) const{return actor<gore_.size()?&gore_[actor]:nullptr;}
    bool update_gore(std::size_t actor,std::int8_t draw_mode_2bda,std::uint32_t frame);
    // Render +2bda: the man draw 003870c0 stores its mode (00387650..0038770c,
    // 00387998 / 003879cc) whenever it draws the model; ActorModel_Setup
    // 0038cac4 starts it at 2.
    void set_draw_mode(std::size_t actor,std::int8_t mode_2bda){draw_mode_2bda_.at(actor)=mode_2bda;}
    std::int8_t draw_mode(std::size_t actor) const{return draw_mode_2bda_.at(actor);}
    // Render +2d92 (actor +2df2): the hit volume sets it to 1 and the morph
    // step 0038b530 counts it up to 72 and back to 0 (the EMO_SHOT counter).
    std::uint8_t& expression_counter(std::size_t actor){return gore_2df2_.at(actor);}
    std::size_t gore_saturations() const{return gore_saturations_;}
private:
    ActorWorld& world_;
    ActorWorldCombatConfig config_;
    std::vector<SecondaryComponent> secondary_;
    std::vector<SecondaryAimState> aim_;
    std::vector<ActorAimState> aim_slots_;
    std::vector<float> aim_angle_;
    std::vector<SecondaryAimResult> aim_result_;
    std::vector<std::pair<std::size_t,SecondaryStep>> secondary_steps_;
    std::vector<HitVolumeGore> gore_;
    std::vector<std::uint8_t> gore_2df2_;
    std::vector<std::int8_t> draw_mode_2bda_;
    std::size_t gore_saturations_{};
    std::vector<std::pair<std::size_t,SoloDamageOutcome>> damage_;
    std::vector<ActorHitEffect> effects_;
    std::function<ActorHitEffect::Cursor()> effect_cursor_;
    std::function<void(std::size_t,std::int32_t,const ActorFireRequest&)> throw_start_;
    std::function<void(std::size_t)> melee_attack_;
    std::vector<TorchLightHost> torch_hosts_;
    std::vector<std::pair<std::size_t,TorchFlickerCall>> torch_calls_;
    std::uint32_t torch_objects_{};
    std::function<void(std::size_t,const SecondaryStep&,bool,float)> secondary_playback_;
    bool secondary_attached_{};
    const CombatActorModel& model(std::size_t actor) const;
    void attach_secondary_update(std::size_t actor);
};

// The receiver's view of +3524 in an actor state (the live fields or a
// receiver candidate): written from the combat inventory (the six slots'
// items and spare rounds, +84, +85, +ef, the bursts, +a4, the object's byte
// +8, +a0) and read back (the spare rounds, +84, +85, +ef, +a4, the bursts,
// +a0; a changed item throws). Throws without a view.
void write_inventory_view(const WeaponInventory& inventory,ActorAnimationReceiverState& state);
void read_inventory_view(const ActorAnimationReceiverState& state,WeaponInventory& inventory);

// 00188d80(out, actor, subtarget): the eye node of a present actor,
// 0049d020[0048a2a0[subtarget & 3]] (subtarget 0: node 14, 1: 6, 2: 4, 3
// and -1: 5).
int actor_eye_node(std::int32_t subtarget);

}
