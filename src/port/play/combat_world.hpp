#pragma once
// Combat over a set of actors (COMBAT.md C1b): per-actor weapon inventories,
// the actors' aim blocks, fire requests (action 0x3b kind 0xb, queued with
// transport 0 and handled at the next pump by 001960c0), the bullet pool and
// the bullet hit entry 00188780 into the actors' damage virtual. The actors
// themselves stay in their host (CombatActors): positions, bounds, shot
// frames, flags, health and damage are read and written through it. Order in
// the original world update: the message pump (fire requests) runs first,
// then the actor traversals, then the bullet manager 00275670 (update).
#include "actor_damage.hpp"
#include "actor_fire.hpp"
#include "bullet_hit.hpp"
#include "bullet_pool.hpp"
#include "shell_eject.hpp"
#include "weapon_aim.hpp"
#include "weapon_models.hpp"
#include "weapon_update.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace omega {

class CombatActors {
public:
    virtual ~CombatActors()=default;
    virtual std::size_t size() const=0;
    virtual std::uint64_t object(std::size_t actor) const=0;          // actor object handle
    virtual std::int32_t object_id(std::size_t actor) const=0;        // actor +c
    virtual bool player(std::size_t actor) const=0;                   // controller +54 (else an AI controller: +58)
    // Port-only (checkpoint 132ob7): the actor holds a port agent slot 1..3 (the --coop-partner), counted as a
    // player by the bullet hit's player-to-player test only (BulletHitVictim::port_agent_slot). Default: none.
    virtual bool port_agent_slot(std::size_t actor) const;
    virtual bool byte_14(std::size_t actor) const=0;
    virtual bool alive_handle(std::size_t actor) const=0;             // the weak handle resolves
    virtual ActorFireState fire_state(std::size_t actor)=0;
    // Controller virtual +64: the player's 00176400 (muzzle node or camera) /
    // the NPC's 0014f380 (muzzle node). Not native: a host input.
    virtual ShotFrame shot_frame(std::size_t actor,bool second_hand,bool aim_view)=0;
    virtual RootVector position_e4(std::size_t actor)=0;
    virtual RootVector eye_c0(std::size_t actor,std::int32_t subtarget)=0;
    virtual std::array<RootVector,2> bounds_88(std::size_t actor)=0;  // actor +2e80 minimum / maximum
    virtual std::uint32_t flags_3204(std::size_t actor)=0;            // stance +c8 (00188e60)
    virtual std::int32_t armor_33a0(std::size_t actor)=0;
    virtual bool invulnerable_33a4(std::size_t actor)=0;
    virtual std::uint8_t life_state(std::size_t actor)=0;             // virtual +108
    // 00188140: the model hit volumes (not resolved; a host without them throws).
    virtual BulletHitVolumes hit_volumes(std::size_t actor,const RootVector& origin,const RootVector& direction,std::int32_t mode,float volume)=0;
    // Virtual +100 (00187360, apply_actor_damage) with the shooter as the source.
    virtual void damage(std::size_t victim,std::optional<std::size_t> shooter,std::uint8_t category,std::uint8_t type,
        std::int32_t& penetration,float amount)=0;
    // Checkpoint 132uk6: virtual +100 from 00188780 with a source that is not an actor (the tank,
    // BulletObjectShooter): no +58 controller, the source's virtual +e4 given. Default: throws.
    virtual void damage_from_object(std::size_t victim,std::uint64_t source,const RootVector& source_e4,std::uint8_t category,
        std::uint8_t type,std::int32_t& penetration,float amount);
    // Static collision for 00277e20 and the static object's +100 at the end of travel.
    virtual std::optional<BulletStaticHit> static_hit(const RootVector& start,const RootVector& displacement,std::uint64_t excluded)=0;
    virtual std::int32_t static_object_id(std::uint64_t object)=0;
    virtual void static_damage(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount)=0;
    // *00236f20(index) (effect ids) and 003ebfb0(0x18) (the effect service exists).
    virtual std::int32_t effect_id(std::int32_t index)=0;
    virtual bool effect_service()=0;
    // 00189560(bullet +d0) for a static hit (Checkpoint 131c): its surface's
    // material row; null when the host has no material table (the impact is
    // then reported without its presentation).
    virtual const MaterialRow* static_material(const BulletStaticHit& hit);
    // The muzzle smoke gates of 00144080: actor +38 and the float at 005383d8.
    virtual std::uint32_t word_38(std::size_t actor)=0;
    virtual float value_005383d8()=0;
    // 00197560 (hold class 1) / 00197140 (hold class 7) with the request
    // (actor_throw.hpp). A host without the throw flow throws.
    virtual void start_throw(std::size_t actor,std::int32_t hold_class,const ActorFireRequest& request);
    // 001960c0's melee branch (hold classes 3 / 4 / 10 / 11): 001414d0 on the
    // actor's inventory, then 00196480 (actor_melee.hpp). A host without the
    // melee throws.
    virtual void melee_attack(std::size_t actor);
    // The torch branch of 00144080 (hold class 10, torch_light.hpp): the light
    // manager, the flicker object's presentation, the attach point and the
    // actor's visibility. nullptr (the default): not hosted, and holding a
    // torch throws naming 00144330.
    virtual const TorchLightHost* torch_host(std::size_t actor);
    // 00384a70's matrices for a hand (Checkpoint 131c): 00385420's attach matrix of point
    // `hand` and the model's node 0 world matrix (0019f350 through the component). nullopt
    // (the default): not hosted, and a shell eject from a gun with a shell record throws.
    virtual std::optional<std::array<ShotFrame,2>> shell_spawn_frames(std::size_t actor,std::size_t hand);
};

struct CombatEvent {
    std::size_t actor{};                          // shooter (discharge) or victim (hit / damage)
    std::optional<WeaponDischargeEvent> discharge;
    std::optional<BulletEvent> bullet;
    std::optional<BulletHitEvent> hit;
    std::optional<ActorFireResult> fire;
};

class CombatWorld {
public:
    // `random` is the shared game RNG state (00127dd0).
    CombatWorld(const WeaponDatabase& database,CombatActors& actors,std::uint64_t& random);
    // 00144dc0 for every actor (with the 0xd0 object and item 0x67 for roles 1 and 3).
    void construct_inventories(const std::vector<bool>& with_object);
    // The inventory of a run-time actor the adapter has just appended (its
    // 00144dc0 call with the same flag), in actor order.
    void append_inventory(bool with_object);
    // ResetNpc's rebuild (001b8130 -> 00143df0 -> 00144dc0) of actor i's
    // inventory in place, after the destructor's 00144c70 released the old one.
    void reset_inventory(std::size_t i,bool with_object);
    WeaponInventory& inventory(std::size_t actor);
    const WeaponInventory& inventory(std::size_t actor) const;
    WeaponAimState& aim(std::size_t actor);
    // Inventory operations with the actor's host answers (valid until the next
    // call for the same actor). Pass the events to check_inventory_events.
    WeaponInventoryOps inventory_ops(std::size_t actor,bool increased_ammo,std::vector<WeaponInventoryEvent>& events);
    // The same host answers by value.
    WeaponInventoryHost inventory_host(std::size_t actor,bool increased_ammo);
    // Refuses model attachment, which needs the model's attachment table.
    static void check_inventory_events(const std::vector<WeaponInventoryEvent>& events);
    // Checkpoint 131: with the weapon model library (set before
    // construct_inventories) the hosts keep each actor's attachment table
    // (actor +60 +2410): their events run at once (WeaponInventoryHost::apply):
    // preload 00145710 / unload 00145630 on the library and attach 0038c7d0 /
    // detach 0038c7c0 / move 0038c790 on the table; the discharge's +411 test
    // reads the attached model. Without it the tables stay empty and an attach
    // is refused (check_inventory_events).
    void set_weapon_models(WeaponModelLibrary* library);
    // 132fx2: NpcDmgAimThresh 00493988 for the hit roll 003ea4a0, read on every roll; unset: 0.9 (registered).
    void set_npc_dmg_aim_thresh(std::function<float()> value){npc_dmg_aim_thresh_=std::move(value);}
    WeaponModelLibrary* weapon_models() const{return models_;}
    // check_inventory_events, except with the library (the events ran).
    void check_events(const std::vector<WeaponInventoryEvent>& events) const;
    // Runs one event of the actor's inventory (library mode only).
    void apply_event(std::size_t actor,const WeaponInventoryEvent& event);
    const ModelAttachTable& attach_table(std::size_t actor) const;
    // Checkpoint 132cp10: the render object's bytes +2442 + point (actor +24a2 + point), which
    // 003855d0 reads to show the light part +412 (gun_drawn_parts). 00142e00(inventory, slot, b)
    // writes slot s's byte at point 00489d70[s] (6 -> 6, 7 -> 7) when the slot holds an item (the
    // callers check); the add of a slot-6/7 item clears it (clear_attach_24a2).
    void write_slot_render_byte(std::size_t actor,std::uint8_t slot,std::uint8_t value);
    std::uint8_t render_point_byte(std::size_t actor,std::uint8_t point) const;
    // Checkpoint 132ms2: game::SetHardpointVis 00185550 -> 00142e40(inventory, slot, vis): the
    // slot's byte +4 + 16 * slot (WeaponSlot::byte_0) = vis (0 / 1) and the render object's hidden
    // byte +2438 + point (actor +2498 + 00489d70[slot]) = !vis, which 003870c0 tests at 00388b24
    // before drawing the point's model (gun_model_drawn).
    void set_hardpoint_vis(std::size_t actor,std::uint8_t slot,bool visible);
    std::uint8_t hidden_point_byte(std::size_t actor,std::uint8_t point) const;
    // Checkpoint 131c: the actor model's ejected shells (shell_eject.hpp). With the library the
    // discharge's flash spawns them (00384a70, with the game RNG); the host runs the model
    // update's history 0038a278 and update 0038a3a8 and draws them.
    ShellEjectState& shells(std::size_t actor);
    const ShellEjectState& shells(std::size_t actor) const;
    // Action 0x3b kind 0xb sent with transport 0: queued for the next pump.
    void queue_fire(std::size_t actor,const ActorFireRequest& request);
    // The pump: the queued fire requests in order (001a92a0 -> 001aa000 -> 001960c0).
    void pump(std::vector<CombatEvent>& events);
    // A direct 0013ff40(inv, target, subtarget, aim view, continuation) of an
    // actor's inventory (the AI firing gate 00150340 with target none,
    // subtarget -1): returns the discharge's result.
    bool discharge(std::size_t actor,const WeaponDischargeInput& input,std::vector<CombatEvent>& events);
    // 00144080 for every actor's inventory (the actor update 001b1d6c runs it
    // during the actor traversal, before the bullet manager).
    void update_inventories(float dt,std::vector<CombatEvent>& events);
    // 00144080 for one actor (the actor update calls it inside actor logic).
    void update_inventory(std::size_t actor,float dt,std::vector<CombatEvent>& events);
    // Checkpoint 132zf11: 00277e20 for a shooter that is not an actor (the
    // tank's fire 00286690); its events are the shooter's (actor field 0).
    // Checkpoint 132uk6: `shooter` is what 00188780 reads of it when a bullet
    // reaches an actor (kept for the bullet's life, by object).
    std::uint64_t request_bullet(const ShotRequest& request,const BulletObjectShooter& shooter,std::vector<CombatEvent>& events);
    // The bullet manager 00275670(dt).
    void update_bullets(float dt,std::vector<CombatEvent>& events);
    const BulletPool& bullets() const{return pool_;}
    // Checkpoint 132b: inside update_bullets, the count of bullet events so far
    // (a hit entry's work comes before the candidate event that follows it);
    // after it, the count of hit events the update listed ahead of its bullet
    // events.
    std::optional<std::size_t> bullet_event_cursor() const{
        return updating_bullets_?std::optional<std::size_t>(updating_bullets_->size()):std::nullopt;}
    std::size_t last_bullet_hit_events() const{return last_bullet_hits_;}
    // The world-object answers of the bullet scan 0022ba30 (candidates over
    // the actors' bounds, the hit entry 00188780, static objects through the
    // actors' host) for another caller of 0022ba30 (the thrown projectile's
    // actor scan); its events go to `events`.
    BulletPoolHost scan_host(std::vector<CombatEvent>& events){return pool_host(events);}
    // The list the bullet scan 0022ba30 walks: the target manager's 004f78dc
    // (the registrations 0022c650, in list order; target_list_candidates).
    // Without one (the scenario worlds, whose actors are all present and
    // registered at load) every actor is offered in actor order.
    struct ObjectList {
        std::function<std::vector<BulletCandidate>()> candidates;
        // A listed object that is not an actor: a node (004d95b0), a GOB
        // (004db4c0 / 004db280 / 004db160 / 004db3a0) or a hotbox (004d9ac0).
        // Their +104 is 0015c0a0 (`jr ra; move v0, zero`: category 0) and
        // +c0 is 0014d340 (-> +e4).
        std::function<bool(std::uint64_t object)> base_object;
        std::function<RootVector(std::uint64_t object)> base_eye_c0;
    };
    void set_object_list(ObjectList list){list_=std::move(list);}
    const ObjectList& object_list() const{return list_;}
    // Checkpoint 132uk4: the hold-8/9 block's area object 0025a2d0 and blast
    // 0021a200 (the throw flow's world, CombatThrows). Without them a bullet
    // that reaches the block throws, naming the address. Checkpoint 132uk6:
    // the explosive branch (00277388) also calls blast; a source that is a
    // hosted non-actor shooter (a tank) comes with its +14 / +e4.
    struct BulletEffects {
        std::function<void(const BulletAreaCall& call)> area;
        std::function<void(const BulletBlastCall& call,const BulletObjectShooter* object)> blast;
        // Checkpoint 132uk6: the discharge's launched projectile (record +d4
        // 1 / 2): 0025d730(new, the actor, &request, record) and 0025d4b0(obj,
        // &point, arc). Empty: such a discharge throws.
        std::function<void(const ObjectWeakHandle& owner,const ShotRequest& request,const RootVector& point,float arc)> launch;
    };
    void set_bullet_effects(BulletEffects effects){effects_=std::move(effects);}
    // Checkpoint 132ia1c: the air taser (hold class 5, air_taser.hpp) on an
    // actor's inventory object +0: the discharge's 00266200 (fire), the
    // inventory update's 00262d90 (update) and 00263080 (release, from the
    // item notice 00143770). Without them a taser shot or a set object state
    // throws.
    struct AirTaser {
        std::function<bool(std::size_t actor,std::uint64_t target,bool aim_view,std::int32_t subtarget)> fire;
        std::function<void(std::size_t actor,float dt)> update;
        std::function<void(std::size_t actor)> release;
    };
    void set_air_taser(AirTaser taser){air_taser_=std::move(taser);}
    const AirTaser& air_taser() const{return air_taser_;}
    // The bullet rule's game::HitInnocentNpcChance (*0048a2b8, read per hit at
    // 00188918). Empty: its boot value 0.3.
    void set_innocent_hit_chance(std::function<float()> chance){innocent_chance_=std::move(chance);}
private:
    WeaponDischargeHost discharge_host(std::size_t actor,std::vector<CombatEvent>& events);
    BulletPoolHost pool_host(std::vector<CombatEvent>& events);
    std::optional<std::size_t> actor_of(std::uint64_t object) const;
    std::uint8_t target_stance_c8(std::uint64_t object,const char* what);
    RootVector projectile_target_e4(std::uint64_t object);
    const WeaponDatabase& db_;
    CombatActors& actors_;
    std::uint64_t& random_;
    std::vector<WeaponInventory> inventories_;
    std::vector<WeaponAimState> aims_;
    std::function<float()> npc_dmg_aim_thresh_;
    std::vector<WeaponInventoryHost> inventory_hosts_;
    WeaponModelLibrary* models_{};
    std::vector<ModelAttachTable> attach_;
    std::vector<std::array<std::uint8_t,10>> point_bytes_;
    std::vector<std::array<std::uint8_t,10>> hidden_bytes_;
    std::vector<ShellEjectState> shells_;
    std::deque<std::pair<std::size_t,ActorFireRequest>> queue_;
    BulletPool pool_;
    ObjectList list_;
    std::map<std::uint64_t,BulletObjectShooter> object_shooters_;   // request_bullet's shooters (132uk6)
    BulletEffects effects_;
    AirTaser air_taser_;
    std::function<float()> innocent_chance_;
    const std::vector<BulletEvent>* updating_bullets_{};
    std::size_t last_bullet_hits_{};
};

}
