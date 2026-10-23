#pragma once
#include "animation_channel.hpp"
#include "animation_controller.hpp"
#include "animation_graph_manifest.hpp"
#include "actor_motion.hpp"
#include "actor_curve_update.hpp"
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace omega {
struct ActorChannelCommand;
class ActorAttachmentDraft;
// State orchestration for the concrete default scalar-channel model. Loaded
// graph/clip assets stay caller-owned and are supplied to each model step.
class AnimationModel {
public:
    AnimationModel(const SkeletonDefinition& skeleton,const AnimationGraphManifest& manifest);
    AnimationModel(const AnimationModel&)=delete;
    AnimationModel& operator=(const AnimationModel&)=delete;
    AnimationModel(AnimationModel&&)=default;
    AnimationModel& operator=(AnimationModel&&)=default;
    // Host transaction snapshot; unlike original channel COPY, preserves timers.
    AnimationModel clone_for_transaction() const;
    const AnimationPlayerState& player() const {return player_;}
    const AnimationController& controller() const {return controller_;}
    const AnimationChannelState& channel(std::size_t index) const;
    std::size_t channel_count() const {return channels_.size();}
    const std::vector<std::size_t>& completed_channels() const {return completed_;}
    void set_value(std::size_t index,float value);
    void approach(std::size_t index,float target,float maximum_step,bool wrapped=false);
    void set_target(std::size_t index,float target,float duration,float damping,bool wrapped=false);
    void select(std::size_t index,std::size_t value);
    void set_playback_rate(float rate);
    void apply_commands(std::span<const ActorChannelCommand> commands);
    void play(std::string_view name,MotionInstallMode mode=MotionInstallMode::replace,
        std::optional<float> duration=std::nullopt);
    void queue(std::string_view name,std::optional<float> duration=std::nullopt);
    void play(const MotionRequest& request,MotionInstallMode mode);
    // Original actor registry/override path. External actor transition state
    // is consumed before installation, including a frame-locked no-op. The
    // owning actor must stage that state with the surrounding transaction.
    bool play_actor_motion(const ActorMotionRegistry&,const ActorMotionSelection&,
        ActorMotionTransitionState&,const MotionRequest& payload,MotionInstallMode mode);
    void clear();
    // Hooks receive the candidate state at send time, not the public last-
    // committed snapshots. Receiver-owned effects require caller transactions.
    void set_delivery_hooks(MotionDeliveryHooks hooks);
    // Placement remains local to a caller-resolved parent. Published aggregate
    // placement is recalculated with the next successful model frame.
    void set_anchor_placement(const RootAnchorKey& key,const RootVector& orientation,const RootVector& position);
    void set_anchor_world_orientation(const RootAnchorKey& key,const RootVector& orientation,bool reset_counter=false);
    void set_anchor_world_position(const RootAnchorKey& key,const RootVector& position);
    void set_anchor_world_placement(const RootAnchorKey& key,const RootParentTransform& placement,bool reset_counter=false);
    void set_anchor_state(const RootAnchorKey& key,const RootAnimationState& state);
    void set_anchor_parent(const RootAnchorKey& key,std::optional<RootParentTransform> parent);
    void set_anchor_constraint(const RootAnchorKey& key,std::optional<RootMotionConstraint> constraint);
    // Late actor-logic curve stage, before the subsequent model frame resets
    // sampled weights. Uses retained base world placement, not pose aggregate.
    // Disabled position updates do not require a resolvable anchor key.
    void update_actor_curves(AnimationRotationCurveState& heading,ActorCurveUpdateState& state,
        const RootAnchorKey& key,bool position_enabled,float elapsed,ActorAttachmentDraft* attachments=nullptr);
    // Original 003ab510 computes a reciprocal before stepping all channels.
    // The bounded host therefore requires finite positive elapsed time.
    void step(const AnimationGraph& graph,const SkeletonDefinition& skeleton,
        const AnimationClipBank& clips,float elapsed,const RootMotionConstraint& constraint={});
    // 001ce7c0's node vt +38 (001cee20: rows[k] into node k's local rotation
    // rows +60..+8c, w words included; node 0 is root_local, node k > 0 the
    // pose bone k - 1) and node 0's vt +3c (001cf160: the translation row
    // +90). Both mark the node and its dependents dirty (+50, 001cef50); the
    // skeleton-space pose is recomputed here from every node's local matrix.
    void write_node_locals(const SkeletonDefinition& skeleton,std::span<const std::optional<std::array<float,12>>> rows,
        const std::optional<RootVector>& root_translation);
private:
    AnimationModel()=default;
    float default_crossfade_{};
    std::vector<GraphNamedRoot> motions_;
    std::vector<AnimationChannel> channels_;
    AnimationController controller_;
    AnimationPlayerState player_;
    std::vector<std::size_t> completed_;
    MotionDeliveryHooks delivery_;
    bool operation_active_{},stepping_{};
    AnimationChannel& checked_channel(std::size_t index);
    AnimationPlayerAnchor& checked_anchor(const RootAnchorKey& key);
    void controller_operation(const std::function<void(AnimationController&,const MotionDeliveryHooks&)>& operation);
};
}
