#pragma once
// N actors over one shared static collision world, stepped as the original
// world update does (NPC_SPAWN_AND_AI.md, "What a multi-actor native host
// needs"; PLAYER_INPUT.md): the pump of queued (transport 0) messages, then
// the first traversal over the actor array (actor +4c, which calls the +58
// component's virtual +3c, then actor logic +48 = 001b1710 when the actor is
// eligible: +2c and +2d set, +2e clear), then the second traversal (model,
// gather and physical frame, +50 = 00392aa0, for every eligible actor), then
// the frame-update event (hotbox trackers). Messages sent with transport 1
// are delivered inside the sender's call.
//
// Each actor keeps its own physical frame, animation model, roots, controls,
// priority actions, heading, hotbox tracker and message queue (a
// SoloActorSimulation), its own object handle (0x100000 + index) and +58
// component handle (0x200000 + index); actor 0 has the handles of a solo
// simulation, so a one-actor world steps exactly like SoloActorSimulation.
// The game RNG (00127dd0) and the frame counter 005383c8 are shared.
//
// Actor-versus-actor collision is the movement pass 001e7f40 / 001e8760 each
// actor's physical response runs on its endpoint (ACTOR_MOVEMENT_ADJUSTMENT.md,
// Checkpoint 128): the world hosts its services for every actor. The global
// list 004ea190 (the NPC system's think list, one AI component per element;
// set_movement_list) and player slot 0 (003fb8d0: the first player actor;
// slots 1..3 are empty in single player) are the other actors it reads; the
// list pass's action-7 message (0016b380 to the AI Sound System, queued) goes
// to set_movement_contact. The relation check 001e8ca0 compares the roots of
// the relation trees +2fc0 (a carried body is its carrier's child, 001ad8c0).
//
// The body carry (Checkpoint 132zf8) resolves its peer actors through the
// world (set_actor_peers; a deleted actor resolves to none) and the body's
// backpack through set_carry_hooks.
//
// Not hosted: cross-actor messages other than those an AI controller sends
// itself, and re-activation of an actor that was deactivated (001b5190
// destroys and 001b5560 recreates the physical component, which is not
// modelled).
#include "solo_actor_simulation.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace omega {
class ActorWorld {
public:
    explicit ActorWorld(SoloCollisionWorld world);
    ActorWorld(const ActorWorld&)=delete;
    ActorWorld& operator=(const ActorWorld&)=delete;
    ~ActorWorld();
    // An actor with the player component (pad producer). present: root object
    // byte +2d after load (1 for the local player, 001c1e70).
    std::size_t add_player(const SoloActorConfig& config,SoloActorAnimation* animation=nullptr,
        std::optional<ActorAnimationTuning> tuning=std::nullopt,SoloActorControlFields initial={},bool present=true);
    // An actor with an AI component (controller.ai must be set). Role-2 NPCs
    // load inactive (+2d = 0) until an activation.
    std::size_t add_ai(const SoloActorConfig& config,SoloActorAnimation* animation,ActorAnimationTuning tuning,
        const SoloActorController& controller,bool present,SoloActorControlFields initial={});
    // ResetNpc 00148600 (checkpoint 132sx10): the destructor 001b57c0 (virtual
    // +8 with -1: no free, no removal from the traversal tree) and the rebuild
    // 001b8130 at the same address. The AI actor at index is replaced by a new
    // one with the same handles, so it keeps its place in the traversal and
    // every reference to it (a handle, an index) names the rebuilt actor, as a
    // pointer to the original's record does. Not during a step (a script
    // native runs between steps), not for a removed or deleted actor.
    void rebuild_ai(std::size_t index,const SoloActorConfig& config,SoloActorAnimation* animation,ActorAnimationTuning tuning,
        const SoloActorController& controller,bool present,SoloActorControlFields initial={});
    std::size_t size() const;
    SoloActorSimulation& actor(std::size_t index);
    const SoloActorSimulation& actor(std::size_t index) const;
    const SoloCollisionWorld& collision() const;
    // A COL owner's transform component moved (a pathed object's published
    // matrix, 00216c60 into its +50 component +10): the actors' next gathers
    // (001815e0 reads the owner +50 component's +10) see it.
    void set_collision_owner(std::uint64_t handle,const CollisionMatrix& matrix);
    // One world update; inputs and the returned frames are in actor order.
    // Ineligible actors get an empty frame and are not touched. An actor
    // added during the step (from the services or the later entries, e.g. a
    // generator's spawn) is visited by that step's first pass after the later
    // entries, then by its second traversal and frame-update event (not by
    // its pump); its frame follows the initial actors' in the result.
    std::vector<SoloActorFrame> step(std::span<const SoloActorFrameInput> inputs);
    // Actor virtual +5c / +60 (SoloActorSimulation::activate / deactivate);
    // effect_slots false: +24 (001b4970).
    void activate(std::size_t index);
    void deactivate(std::size_t index,bool effect_slots=true);
    // 0040b720(*004ffd44, actor): the actor's key +c is pushed on the pending
    // vector (+14) of the traversal array and of each parent; the frame loop's
    // flush 0040c5c0 -> 0040b4b0 (00167dac, after the event tick 003e9e30 and
    // before the next world update) erases it from the array. Here the flush
    // runs at the start of the next step: from then on the actor gets no +4c,
    // no logic, no second traversal and no frame-update event. It still gets
    // the pump (its queued messages and its AI component's): 003c8de0
    // delivers to the receiver pointer while its +4 holds the object magic
    // (003c9160..003c919c), whatever the tree. A second request is a no-op
    // (0040b510 skips a key no longer in the array).
    void remove(std::size_t index);
    // A removal was requested (pending or flushed): 0040bf50 deletes such an
    // object (found in the pending vector, or no longer in the tree).
    bool removed(std::size_t index) const;
    // The two states 0040bf50 tests: the key in the pending vector (requested,
    // not yet flushed) and the actor in the traversal tree (not flushed).
    bool removal_pending(std::size_t index) const;
    bool traversed(std::size_t index) const;
    // 0040bf50's delete (virtual +8(-1), then the free): the actor's
    // destructor ran (spawned_npc_teardown.hpp) and its memory is released,
    // so from the next step it gets nothing (not even the pump: its +4 no
    // longer holds the magic, 003cb330) and an empty frame. Only for an actor
    // whose removal was requested; the player is never deleted here.
    void destroy(std::size_t index);
    bool destroyed(std::size_t index) const;
    std::uint32_t frame_counter() const;
    std::uint64_t random_state() const;
    // The shared game RNG state (00127dd0), e.g. for constructors that draw from it.
    std::uint64_t& random();
    // The world's child services of the frame-update broadcast (the AI route
    // manager's update 00163ad0, npc_route.hpp), run each step after the
    // pump and before the first traversal; called with the step's frame
    // counter (005383c8).
    void set_frame_services(std::function<void(std::uint32_t frame)> services);
    // The traversal array's entries after the actors (the level's generators,
    // npc_generator.hpp), run each step between the first and the second
    // traversal; called with the step's frame counter.
    void set_after_first_traversal(std::function<void(std::uint32_t frame)> entries);
    // Every actor's elapsed time for the next steps (they share the world's dt).
    void set_step_dt(float dt);
    // The global list 004ea190 the movement pass 001e7f40 walks, in list
    // order, as actor indices (each element's +58 +30 owner); read once per
    // pass. A destroyed actor's element is stale (skipped). Empty: no list.
    void set_movement_list(std::function<std::vector<std::size_t>()> list);
    // The body's backpack operations of the body carry (SoloActorSimulation::CarryHooks).
    void set_carry_hooks(SoloActorSimulation::CarryHooks hooks);
    // The list pass's action-7 message (001e7f40 at 1e84b8..1e857c: an
    // adjusted pair, the mover's +14 set and the target's controller +58 or
    // +5c true): 0016b380(mover +c, target +c, 7) to the AI Sound System.
    void set_movement_contact(std::function<void(std::size_t mover,std::size_t target)> contact);
    // Port-only (--coop-partner, coop_partner.hpp AgentSlots): the actor in
    // player slot 1..3 (003fb8d0(slot)), which single player leaves empty and
    // the multiplayer session fills with the remote agents. The movement pass
    // reads it as a player slot, and as that agent's player component (vtable
    // 004d9cf0, virtual +54 = 1) whatever drives it; nullopt empties the slot.
    // Such an actor must not also be in the movement list.
    void set_agent_slot(int slot,std::optional<std::size_t> actor);
    bool in_agent_slot(std::size_t actor) const;
    // Checkpoint 132cp11: the world's water-effect manager (world +e4; 00137ab0 builds it with 002b3070 when the
    // world is set up, drawing the game RNG 48 times, before any actor). The actors' water calls (001e5cf0,
    // 001e98f0) reach it; the host updates it (002b1d20 at 001337e8) and draws it (002b1cb0).
    WaterEffectManager& water();
    WaterEffectActorHost& water_actors();
    // The sink of the manager's record resources (002b2a40: effect table entries 0x28 / 0x27 and the ripple
    // texture's frame count), its start / stop messages and its splash effects (00232120(0x11 / 0x12)). The
    // default answers {0x28, 0x27, 1} and drops the rest (the standalone simulation's stand-in).
    void set_water_sink(WaterEffectSink* sink);
    WaterEffectSink& water_sink();
    class MovementHost;
    class WaterActors;
    class DefaultWaterSink;
private:
    std::size_t add(const SoloActorConfig& config,SoloActorAnimation* animation,std::optional<ActorAnimationTuning> tuning,
        SoloActorControlFields initial,const SoloActorController& controller,bool present);
    std::unique_ptr<SoloActorSimulation> make(std::size_t index,const SoloActorConfig& config,SoloActorAnimation* animation,
        std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial,const SoloActorController& controller,bool present);
    bool stepping_{};
    std::shared_ptr<SoloCollisionWorld> world_;
    std::vector<std::unique_ptr<SoloActorSimulation>> actors_;
    std::vector<bool> removal_requested_,removed_,destroyed_;
    // The weak-owner serial of each slot (a water record's +4): rebuild_ai's new actor (001b57c0, 001b8130) no longer
    // matches the records of the one it replaced.
    std::vector<std::int32_t> serials_;
    std::uint64_t random_{};
    std::uint32_t frame_counter_{};
    std::function<void(std::uint32_t)> services_;
    std::function<void(std::uint32_t)> after_first_traversal_;
    std::function<std::vector<std::size_t>()> movement_list_;
    SoloActorSimulation::CarryHooks carry_hooks_;
    std::function<void(std::size_t,std::size_t)> movement_contact_;
    std::optional<std::size_t> first_player_;
    std::array<std::optional<std::size_t>,3> agent_slots_{};
    std::unique_ptr<MovementHost> movement_;
    std::unique_ptr<WaterEffectManager> water_;
    std::unique_ptr<WaterActors> water_actors_;
    std::unique_ptr<DefaultWaterSink> default_water_sink_;
};
}
