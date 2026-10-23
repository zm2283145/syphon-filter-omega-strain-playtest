#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <vector>

namespace omega {
enum class ActorChannelOperation { set, approach, target, select };
struct ActorChannelCommand {
    ActorChannelOperation operation{};
    std::size_t index{};
    float value{}, parameter{}, damping{};
    bool wrapped{};
};
// Explicit local-player snapshot. Equipment fields are the results of the
// original inventory predicate and selected weapon-definition lookup, not a
// guessed keyboard or weapon-name mapping.
struct ActorDirectionInput {
    float elapsed{}, desired_x{}, desired_y{}, speed{};
    std::array<float,18> current_channels{};
    std::uint32_t flags3034{}, flags3210{}, flags3248{};
    bool physical_present{}, contact_dc{}, facing_positive{}, has_equipment{};
    std::uint8_t weapon_class{};
};
struct ActorDirectionPlan {
    float approach_step{}; // Original actor +31e0, for subsequent producer stages.
    std::vector<ActorChannelCommand> commands;
};
// Original local-player preparation slice0019a0a8..0019a5e0 only. Does not
// advance channels, install motions, execute actor messages or update heading.
// atan2 uses portable host arithmetic, not execution-verified PS2 libm.
ActorDirectionPlan prepare_actor_direction_channels(const ActorDirectionInput& input);
struct ActorAnimationTuning {
    float accumulation_duration{};
    // Runtime globals0049cf48..0049cfe8 at eight-byte stride. The executable's
    // initial storage is zero; callers must provide verified initialized values.
    std::array<float,21> channel_timing{};
};
ActorAnimationTuning read_actor_animation_tuning(std::istream& input);
struct ActorOrdinaryInput {
    ActorDirectionInput direction;
    std::array<float,18> target_channels{};
    std::array<float,4> prior_direction{};
    float desired_z{}, desired_w{}, turn{}, accumulation{};
    std::uint32_t mode{}, flags3204{}, flags324c{};
    bool turn_correction_enabled{}, turn_pose_enabled{}, accumulate_enabled{}, contact_dd{};
    float physical_2c8{};
    std::optional<float> physical_height; // Resolved001e96a0 result; absent if sentinel.
    std::optional<std::uint8_t> secondary_311;
};
struct ActorOrdinaryPlan {
    std::vector<ActorChannelCommand> commands;
    float approach_step{}, accumulation{};
    std::array<float,4> desired_direction{}, prior_direction{};
    bool facing_positive{};
    std::uint32_t pending_set_bits{};
    bool world_message_candidate{};
};
// Channel/vector arithmetic for ordinary modes0/2 and flags3210 bits27/5. Includes early preparation;
// excludes entry flag bookkeeping, synchronous event delivery and final heading.
// Unknown special modes/branches reject; no named-motion substitution occurs.
// Commit returned actor fields only after the model command batch succeeds.
ActorOrdinaryPlan prepare_actor_ordinary_channels(const ActorOrdinaryInput& input,
    const ActorAnimationTuning& tuning);
struct ActorNeutralPolicy {
    bool preserve_turn{},preserve_selector{},preserve_channel17{},preserve_channel6{};
};
// Original0019dd80 helper. Does not include equipment preparation or accumulation.
ActorOrdinaryPlan prepare_actor_neutral_channels(const ActorOrdinaryInput& input,
    const ActorAnimationTuning& tuning,float approach_step,ActorNeutralPolicy policy={});
struct ActorAnimationSnapshot {
    ActorOrdinaryInput ordinary;
    std::uint32_t previous_flags{}, frame_counter{}, source_id{}, physical_flags810{};
    // Resolved original global005721c8 and component virtual+54 predicate.
    bool multiplayer_active{},component_predicate{true},actor_flag14{},physical_3d2{};
    std::uint64_t component_receiver{};
    float running_value{}; // Original actor+31f8.
    float approach_step{}; // Mutable actor+31e0, reread after message delivery.
};
struct ActorAnimationPatch {
    std::vector<ActorChannelCommand> commands;
    std::uint32_t pending_clear_bits{},pending_set_bits{},flags3248_clear_bits{},flags3248_set_bits{};
    bool physical_request3d1{},physical_clear38_bit0{},physical_set38_bit0{};
    std::optional<float> approach_step,accumulation;
    std::optional<std::array<float,4>> desired_direction,prior_direction;
    std::optional<bool> facing_positive;
};
struct ActorAnimationEvent {
    enum class Kind { stance, running_world } kind{};
    std::uint64_t receiver{};
    std::uint32_t action{};
    float value{};
    std::uint32_t source_id{}; // Running event copies actor+c, not its pointer.
    // Running payload also carries fixed amplitude1; transport flag0.
    // Stance payload carries auxiliary -1/0; transport flag1.
};
struct ActorAnimationHost {
    std::function<ActorAnimationSnapshot()> snapshot;
    // Commit each patch atomically; it is not an arbitrary actor event boundary.
    std::function<void(const ActorAnimationPatch&)> apply;
    // Synchronous, may change actor/model state and the next event's recipient.
    std::function<void(const ActorAnimationEvent&)> deliver;
    std::function<std::uint64_t()> resolve_running_receiver;
    // Required only for mode16: call the native staged ladder body on this actor.
    std::function<void(float)> ladder_body;
    // Original0019d950 handles bits3/6 after the mode7/8/16 special branches.
    std::function<void()> paired_body;
    // Original001a80e0, after pending entry flags and before early channels.
    std::function<void(float)> navigation_body;
};
// Bounded channel dispatch through accumulation. Navigation, ladder and paired
// bodies require explicit continuations when the original predicates select them. Reads
// fresh snapshots after synchronous delivery, while retaining original dt/gate.
// Receiver effects cannot be rolled back; exceptions stop at the current stage.
// Final heading/history is separate; ladder and paired bodies require continuations.
void update_actor_animation_channels(const ActorAnimationHost& host,
    const ActorAnimationTuning& tuning);
struct ActorHeadingInput {
    std::array<float,4> world_rotation{0,0,0,1};
    std::vector<float> history;
    std::size_t selected{};
    float saved_yaw{},turn{},elapsed{};
    std::uint32_t mode{},flags3204{},flags3210{},flags3248{};
    bool physical_present{},contact_dd{};
    std::uint32_t transition_previous{},transition_current{}; // Actor3234/3220.
    std::array<float,4> transition_rotation{0,0,0,1}; // Resolved root3294.
    bool euler_negate{}; // Explicit runtime global0049e9a0, not inferred from mode.
};
struct ActorHeadingCommand {
    enum class Kind { history, target, world_orientation, clear_root_160, stationary_latch } kind{};
    std::size_t index{};
    float value{}; // Heading/yaw or latch scalar according to kind.
    std::array<float,4> rotation{}; // World orientation command only.
    float duration{-1}; // Target command: negative retains timing, .5 on transition.
};
// Original final-heading producer, except mode16's staged path. World quaternion must already be
// resolved through the parent transform. Commands retain history/placement order;
// world_orientation requests original0019d3b0, not a local transform assignment.
// Target duration -1 preserves timing; the mode4/5 transition uses .5.
std::vector<ActorHeadingCommand> prepare_actor_heading(const ActorHeadingInput& input);
struct ActorHeadingSnapshot {
    ActorHeadingInput heading;
    std::array<float,4> ladder_reference_rotation{0,0,0,1}; // Resolved root3294.
};
struct ActorHeadingHost {
    std::function<ActorHeadingSnapshot()> snapshot;
    // Apply ordered commands; world placement must use the parent-aware setter.
    std::function<void(const ActorHeadingCommand&)> apply;
};
// Mode16 places root3294's world rotation on root3290, then reads root3290's
// resolved result before deriving history. Exceptions retain completed stages.
void update_actor_heading(const ActorHeadingHost& host);
struct ActorPairReference {std::uint64_t actor{};std::uint32_t generation{};};
struct ActorPairSnapshot {
    ActorAnimationSnapshot animation;
    std::uint32_t generation{};
    std::optional<ActorPairReference> partner;
};
struct ActorPairHost {
    std::function<std::optional<ActorPairSnapshot>(std::uint64_t)> snapshot;
    std::function<void(std::uint64_t,const ActorAnimationPatch&)> apply;
};
// Original0019d950. Revalidates the source's current linked actor before each
// original lookup. A disappearing reference after initial success rejects at
// that stage; earlier mutations are retained, rather than accessing null.
void update_actor_pair_channels(const ActorPairHost& host,std::uint64_t actor,
    const ActorAnimationTuning& tuning);

}
