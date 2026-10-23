#pragma once
#include "animation_player.hpp"
#include "animation_transition.hpp"
#include <array>
#include <optional>

namespace omega {
enum class MotionInstallMode { queue=0, replace=1, replace_if_different=2, reset=3 };
struct MotionGroupSelector {
    std::optional<std::size_t> node;
    std::int32_t group{-1};
};
struct MotionToken { std::int32_t id{-1}; std::uint64_t receiver{}; };
struct MotionEmbeddedMessage {
    std::int32_t id{};
    std::uint64_t receiver{};
    std::uint32_t flags{5};
    bool sent{};
};
struct MotionPhaseHook { float start{},end{}; MotionToken token; };
struct MotionRequest {
    std::size_t node{};
    float width{},bias{},rate{},secondary_rate{},limit{-1},initial_phase{};
    MotionGroupSelector selector;
    std::array<MotionEmbeddedMessage,2> messages{};
    bool notifications_active{};
    std::array<MotionToken,7> notifications{};
    std::vector<MotionPhaseHook> phase_hooks;
};
struct MotionRecord {
    std::uint64_t host{};
    MotionRequest request;
    TransitionBlendState blend;
    bool starting{},leaving{};
};
struct MotionEffect {
    enum class Kind { embedded, token } kind{};
    std::uint64_t host{},receiver{};
    std::int32_t id{};
    int slot{},action{};
    std::uint32_t flags{5};
    float step{};
};
struct AnimationController {
    std::vector<MotionRecord> active,queued;
    std::uint64_t next_host{1};
    bool terminal_threshold_enabled{};
    float terminal_width{},terminal_bias{};
    std::vector<MotionEffect> effects;
    // Transient operation guards, never persisted as animation data.
    bool installation_locked{},delivery_active{};
};

struct MotionDeliveryView {
    const AnimationController& controller;
    // The actual transaction candidate. Standalone controller install/clear
    // has no player; AnimationModel binds its operation draft for those calls.
    const AnimationPlayerState* player{};
    bool installation_locked{};
    MotionDeliveryView(const AnimationController& c,AnimationPlayerState* p,bool locked)
        :controller(c),player(p),installation_locked(locked),candidate_(p){}
    // Proved same-frame root-placement capability. Other candidate state
    // remains read-only; placement uses the resolved anchor's local space.
    void set_anchor_placement(const RootAnchorKey& key,const RootVector& orientation,const RootVector& position) const;
    void set_anchor_orientation(const RootAnchorKey& key,const RootVector& orientation) const;
    // Actor token 0x15: orientation-only transfer followed by counter reset.
    // Parented world-to-local transfer requires a separately proved adapter.
    void copy_unparented_anchor_orientation(const RootAnchorKey& destination,const RootAnchorKey& source) const;
    void copy_unparented_anchor_placement(const RootAnchorKey& destination,const RootAnchorKey& source) const;
    void copy_anchor_world_orientation(const RootAnchorKey& destination,const RootAnchorKey& source) const;
    void copy_anchor_world_orientation(const RootAnchorKey& destination,const RootAnchorKey& source,const RootVector& pre_rotation) const;
    void copy_anchor_world_heading(const RootAnchorKey& destination,const RootAnchorKey& source,const RootVector& post_rotation) const;
    void copy_anchor_world_placement(const RootAnchorKey& destination,const RootAnchorKey& source) const;
    // Direct scalar write at the original send point. The evaluated graph is
    // retained; this changes the model channel and its published current value.
    void set_channel_value(std::size_t index,float value) const;
    bool can_set_channel_value(std::size_t index) const;
    using ChannelSetter=std::function<float(std::size_t,float)>;
    MotionDeliveryView with_channel_setter(ChannelSetter setter) const;
    using ChannelTargetSetter=std::function<void(std::size_t,float,float,float,bool)>;
    MotionDeliveryView with_channel_target_setter(ChannelTargetSetter setter) const;
    void set_channel_target(std::size_t index,float target,float duration,float damping,bool wrapped=false) const;
    bool can_set_channel_target(std::size_t index) const;
private:
    friend class ActorAttachmentDraft;
    AnimationPlayerState* candidate_{};
    ChannelSetter channel_setter_;
    ChannelTargetSetter channel_target_setter_;
};
struct MotionDeliveryHooks {
    using Sink=std::function<void(const MotionEffect&,const MotionDeliveryView&)>;
    Sink immediate; // Solo flags bit 0: invoke receiver now, bypass queue/filter.
    Sink enqueue;   // Bit 0 clear: capture a value copy for later delivery.
};

// Optional hooks run synchronously at each original send, before sent/consumed
// writes. The caller owns receiver lookup and external transactionality: already
// executed effects cannot be rolled back by this component if a callback throws.
// Frame-time installation is ignored by the original installation lock. Other
// recursive controller operations are outside this bounded host and rejected.
// Each operation replaces its diagnostic effect list; it is never flushed later.
MotionRequest make_motion_request(std::size_t node,float crossfade_duration);
void install_motion(AnimationController& controller,const MotionRequest& request,MotionInstallMode mode,
    const MotionDeliveryHooks& delivery={});
void clear_motions(AnimationController& controller,const MotionDeliveryHooks& delivery={});
// Atomic controller/player update. Graph traversal runs newest-first, effects
// unwind oldest-first, then surviving hosts sample oldest-first.
void update_animation_controller(AnimationController& controller,AnimationPlayerState& player,
    const AnimationGraph& graph,const SkeletonDefinition& skeleton,const AnimationClipBank& clips,
    float dt,const RootMotionConstraint& constraint={},const MotionDeliveryHooks& delivery={});
}
