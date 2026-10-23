#pragma once
#include "animation_controller.hpp"
#include "actor_model_slots.hpp"

namespace omega {
struct ActorAnimationObjectReference {std::uint64_t identity{};std::int32_t generation{};};
struct ActorAnimationInventoryEntry {std::int32_t item{-1},count{};};
struct ActorAnimationInventoryState {
    std::uint64_t owner{};
    std::array<ActorAnimationInventoryEntry,6> entries;
    std::uint8_t mode{6},saved_mode{6},byte_ef{};
    std::int32_t pending_bc{},pending_c0{},selection_a4{-1};
    bool owner_control_active{}; // Resolved inventory[0] object's byte+8.
    std::uint64_t control_component{}; // Resolved inventory[0] identity, zero is null.
};
struct ActorAnimationInventoryBindings {
    std::array<std::uint8_t,6> slots_70{},slots_78{};
    std::vector<std::optional<ActorModelPart>> definitions; // Original definition+13c.
};
// Caller-owned views of the specific retained fields touched by admitted
// branches. These are not a replacement physical controller or actor object.
struct ActorAnimationPhysicalState {
    std::uint32_t flags_38{};
    std::uint8_t byte_9c0{};
    float value_374{};
    std::uint32_t modes_2c{};
    std::int32_t highest_mode_28{-1};
    std::uint8_t byte_14{};
    std::uint8_t byte_3d0{};
    std::uint8_t byte_3d3{};
    std::uint8_t byte_dd{};
    float value_e4{};
    std::uint64_t model{}; // Physical+c model identity for001e96a0.
};
struct ActorAnimationReceiverState {
    std::uint32_t flags_3204{},active_actions{},changes_3218{},flags_3248{};
    std::uint32_t flags_324c{};
    std::int32_t highest_action{-1};
    std::uint8_t byte_33e5{},byte_33e8{};
    std::optional<ActorAnimationPhysicalState> physical;
    // Resolved actor+3524 object's byte+a0, not an invented attachment object.
    std::optional<std::uint8_t> attachment_byte_a0;
    ActorAnimationObjectReference linked_model; // Actor+3478/+347c.
    std::optional<ActorAnimationInventoryState> inventory;
    std::uint8_t byte_33e4{};
    std::uint64_t controller_3474{};
    std::uint8_t byte_14{},byte_326b{};
    std::array<std::uint32_t,4> history_31c0{};
    std::uint32_t history_31d0{},history_31d4{};
    std::uint8_t byte_31b7{};
    std::uint32_t target_33cc{}; // Original named-object key, not a pointer.
    RootVector aim_33d0{};
    float launch_33e0{};
    // The IR switch's argument (written by 001992b0 / 00199590, read by the
    // item trigger 00197e00 at 00198564).
    std::uint8_t byte_33e6{};
    // The goggles-on byte (the player's toggles 00199770 / 00199860, not
    // hosted; cleared by 00199590), the IR vision byte +346f (00199590), the
    // vision mode +3470 and the vision loop sound flag +33b9 (001b31b0,
    // actor_goggles.hpp). The constructors write 0 (001b5fe0).
    std::uint8_t byte_33e7{},byte_346f{},vision_3470{},loop_33b9{};
    // +346e: the local player's pad producer's 00241ec0 result (0x0024f928, the held record's +100 while the aim
    // view picks a reticle), read by 001b31b0 when +346f is 0.
    std::uint8_t byte_346e{};
};
struct ActorAnimationRootBindings {RootAnchorKey flor{},edge{};};
struct ActorAnimationReceiverConfig {
    std::uint64_t receiver{};
    std::optional<ActorAnimationRootBindings> roots;
    std::optional<float> physical_restore_374; // Verified executable word004a03e0.
    std::uint64_t controller_receiver{};
    std::optional<std::int32_t> actor_generation;
    std::map<std::int32_t,std::array<std::uint8_t,16>> sound_keys;
    std::optional<ActorAnimationInventoryBindings> inventory;
    // Runtime-resolved root keys used by each admitted detach token.
    std::map<std::int32_t,RootAnchorKey> detach_roots;
    std::optional<RootAnchorKey> related_root;
    std::optional<std::string> climb_finish_motion;
    std::optional<float> contact_height_sentinel;
    std::optional<RootAnchorKey> contact_root;
    // Token3 default, alternate and non-sentinel-height names, in that order.
    std::optional<std::array<std::array<std::uint8_t,16>,3>> contact_sound_keys;
    std::optional<std::int8_t> target_transform_selector;
    // The owner's pop_world_object callback never changes the inventory (the
    // solo host's world objects have no inventory effect), so the conservative
    // inventory-host preflight before the pop is not needed.
    bool world_pop_keeps_inventory{};
};
struct ActorAnimationControllerRequest {
    std::uint64_t receiver{};
    std::int32_t code{},parameter{-1};
    std::uint64_t object{};
    std::uint32_t flags{5};
};
struct ActorAnimationSoundRequest {
    enum class Kind {start,stop} kind{Kind::start};
    std::int32_t bank{};
    std::array<std::uint8_t,16> key{};
    std::uint64_t actor{};
    std::int32_t actor_generation{};
    std::uint32_t flags{1};
    std::uint32_t preset_word_180{};
    std::uint8_t preset_byte_186{};
};
struct ActorAnimationSoundPreset {
    std::int32_t bank{};
    std::array<std::uint8_t,16> key{};
    std::uint32_t word_180{};
    std::uint8_t byte_186{};
};
struct ActorAnimationInventoryOwner {std::uint64_t controller{};bool player{},local_active{};};
struct ActorAnimationReceiverHost {
    // Original mode0: capture an owned request; do not run its receiver now.
    std::function<void(ActorAnimationControllerRequest,const ActorAnimationReceiverState&,const MotionDeliveryView&)> enqueue_controller;
    std::function<void(const ActorAnimationControllerRequest&,ActorAnimationReceiverState&,const MotionDeliveryView&)> controller_immediate;
    // Original flags1/mode1: execute at this point and observe any changes to
    // the supplied actor candidate before subsequent receiver statements.
    std::function<void(const ActorAnimationSoundRequest&,ActorAnimationReceiverState&,const MotionDeliveryView&)> sound_immediate;
    std::function<std::optional<std::int32_t>(std::uint64_t)> object_generation;
    std::function<void(std::uint64_t,std::size_t,float)> set_other_model_channel;
    // Read-only live owner/slot queries and actual model-slot operations.
    std::function<ActorAnimationInventoryOwner(std::uint64_t)> inventory_owner;
    std::function<std::size_t(std::uint64_t)> model_slot_count;
    std::function<bool(std::uint64_t,std::size_t)> model_slot_present;
    std::function<void(std::uint64_t,std::size_t,std::size_t,const ActorAnimationReceiverState&)> swap_model_slots;
    std::function<void(std::uint64_t,std::size_t,const std::optional<ActorModelPart>&,const ActorAnimationReceiverState&)> set_model_slot;
    // Both original sends use mode0. The first is descriptor4ea150 with its
    // sole word+24 item identifier; the second uses the resolved selection
    // sound preset4f1950 and owner virtual+c0(-1) position. Neither is immediate.
    std::function<void(std::uint64_t,std::int32_t,const ActorAnimationReceiverState&)> enqueue_inventory_empty;
    std::function<void(std::uint64_t,const ActorAnimationReceiverState&)> enqueue_inventory_selection_sound;
    // Original direct child-list clear, at the token's actual call point.
    // The owning host stages attachment changes with its actor/model draft.
    std::function<void(RootAnchorKey,const ActorAnimationReceiverState&,const MotionDeliveryView&)> clear_root_children;
    std::function<bool(std::uint64_t)> controller_is_player;
    // Must execute the proved actor_world_stack helpers synchronously using
    // the owner's draft; activate/deactivate callbacks may change this actor.
    std::function<void(ActorAnimationReceiverState&,const MotionDeliveryView&)> pop_world_object;
    std::function<void(ActorAnimationReceiverState&,const MotionDeliveryView&)> finish_world_pending;
    // Resolve the actor's related-model pointer vector in original order and
    // bind those roots to this key with reset-local-placement=true.
    std::function<void(RootAnchorKey,const ActorAnimationReceiverState&,const MotionDeliveryView&)> attach_related_roots;
    std::function<std::optional<RootParentTransform>(std::string_view)> motion_end_placement;
    // Real synchronous dependencies of001a3780. Cancellation must implement
    // the active component's263080 teardown; a no-op is not an implementation.
    std::function<void(std::uint64_t,ActorAnimationReceiverState&,const MotionDeliveryView&)> cancel_inventory_component;
    std::function<void(ActorAnimationReceiverState&,const MotionDeliveryView&)> create_world_inventory;
    // Reads the initialized original four-word template at the actual copy
    // point, after dependency callbacks. No constant-zero assumption.
    std::function<std::array<std::uint32_t,4>()> reset_history_template;
    // Original flags1/mode0. Own the complete request without playing it now.
    std::function<void(ActorAnimationSoundRequest,const ActorAnimationReceiverState&,const MotionDeliveryView&)> enqueue_sound;
    std::function<ActorAnimationSoundPreset(std::int32_t)> sound_preset;
    std::function<RootVector(std::uint64_t,RootAnchorKey,ActorAnimationReceiverState&,const MotionDeliveryView&)> physical_root_position;
    std::function<std::uint64_t(std::uint32_t)> resolve_named_target;
    //236f20 descriptor's first word; null means absent descriptor, not index-1.
    std::function<std::optional<std::int32_t>(std::uint8_t)> effect_descriptor;
    std::function<std::array<float,16>(std::uint64_t,std::int8_t)> target_transform;
    //232180 receives position and direction (target matrix column1).
    std::function<void(std::int32_t,RootVector,RootVector,ActorAnimationReceiverState&,const MotionDeliveryView&)> emit_directional_effect;
    // Synchronous00141e20 dependency: real projectile factory/launch, model
    // slot removal, aim resolution, AI/world callbacks and inventory effects.
    // The float is captured by the caller; target/aim/inventory are live fields
    // of the actor candidate, corresponding to original pointer arguments.
    std::function<void(float,ActorAnimationReceiverState&,const MotionDeliveryView&)> launch_inventory_projectile;
    std::function<std::uint32_t()> null_target_key;
};
// Original001a77e0 header fast path and separate root-pose endpoint sampler.
// Missing/non-leaf registry entries return null. The final argument represents
// the shared translation template read by the original sampler constructor;
// its default is the proved zero-initialized state, not an immutable-global claim.
// 001901c0 is the same sampler for the start placement (called by 001a2470
// for DATA_ID_JumpGrabLedge with the same lookup and template arguments): its
// header fast path reads clip header +30/+40 instead of +50/+60, and without
// a phase offset it samples the other range endpoint (range_end when
// reversed, otherwise range_start). The rest is identical (static).
enum class MotionPlacementEndpoint { end, start };
std::optional<RootParentTransform> resolve_receiver_motion_placement(const AnimationGraph&,
    const AnimationClipBank&,std::string_view name,const RootVector& initial_translation={},
    MotionPlacementEndpoint endpoint=MotionPlacementEndpoint::end);
// Called by the host's immediate hook at the actual send point. Unsupported
// branches reject before changing actor/root state. Queue delivery is separate.
// Actor fields commit only after success; candidate model mutations belong to
// the surrounding frame transaction. External host effects cannot be rolled
// back here, so owning hosts must transact any external actor/model state.
void receive_actor_animation(ActorAnimationReceiverState& state,
    const ActorAnimationReceiverConfig& config,const MotionEffect& effect,
    const MotionDeliveryView& view,const ActorAnimationReceiverHost& host={});
// Priority helpers 001898c0/00189840/001897a0/00189740 on actor +320c: read the
// +3218 bit, set or clear it in +3210, rescan the highest set bit (or -1) into
// +320c, and OR the bit into +3218 when the read bit differs.
void set_actor_action(ActorAnimationReceiverState& state,unsigned index,bool enabled);
// 00143cf0 on the inventory view: with the saved slot +85 != 6, equip it
// (00142c50: holster through the switch gate, slot 1 first for slot 0 with a
// slot-1 item, the 004f1950 selection sound for a local player unless slot 1,
// the holster -> hand model swap, +a4 = -1). Uses config.inventory and the
// host's inventory members only.
void restore_actor_inventory(ActorAnimationReceiverState& state,const ActorAnimationReceiverConfig& config,
    const ActorAnimationReceiverHost& host);
// 00143b60 on the inventory view: through the switch gate 00143bc0, a held
// slot (+84 != 6) is hidden (00142bb0: the hand -> holster model swap, +85 =
// the slot, +84 = 6, +a4 = -1); nothing held stores +85 = 6.
void hide_actor_inventory(ActorAnimationReceiverState& state,const ActorAnimationReceiverConfig& config,
    const ActorAnimationReceiverHost& host);
}
