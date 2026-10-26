#pragma once
#include "actor_animation.hpp"
#include "animation_model.hpp"
#include "morph_host.hpp"
#include "morph_resources.hpp"
#include "actor_attachment_runtime.hpp"
#include <memory>
#include <optional>

namespace omega {
enum class CharacterMorphBinding { none, face, hand0, hand1 };
// Port-only (132vp1, memory: research/VITA.md "Playability, memory and coverage"): a part's decoded morph shapes,
// read only, so the parts bound to the same SKF can share one decode. Reads like the std::optional<MorphData> it
// replaces.
class SharedMorphData {
public:
    SharedMorphData() = default;
    SharedMorphData(std::nullopt_t) {}
    SharedMorphData(MorphData data) : data_(std::make_shared<const MorphData>(std::move(data))) {}
    SharedMorphData(std::shared_ptr<const MorphData> data) : data_(std::move(data)) {}
    bool has_value() const { return data_ != nullptr; }
    explicit operator bool() const { return has_value(); }
    const MorphData& operator*() const { return *data_; }
    const MorphData* operator->() const { return data_.get(); }
    const std::shared_ptr<const MorphData>& shared() const { return data_; }
private:
    std::shared_ptr<const MorphData> data_;
};
struct AnimatedCharacterPart {
    std::string name;
    SkinnedMesh mesh;
    SharedMorphData morph;
    CharacterMorphBinding binding{CharacterMorphBinding::none};
};
struct AnimatedCharacterGeometry {
    std::string name;
    SkinnedMeshGeometry mesh; // World placement already applied to vertices.
};
struct CharacterGazeInput {
    bool model_present{true};
    std::optional<RootVector> live_target;
    RootVector fallback{};
};
struct AnimatedCharacterResourceInput {
    std::optional<MorphGripMap> grip_map;
    MorphEquipment equipment;
    std::optional<CharacterGazeInput> gaze;
};
// Optional already-enabled actor logic tail. Its retained curve/control state
// commits with model, morph, geometry and any attachment draft. The caller runs
// the earlier gameplay/heading producers and owns the outer actor update gate.
struct CharacterActorCurveFrame {
    AnimationRotationCurveState& heading;
    ActorCurveUpdateState& state;
    RootAnchorKey root;
    bool position_enabled{};
};
// Connects caller-loaded assets and already-produced command batches. Actor
// event stages, equipment resolution, clock/RNG and collision remain callers.
class AnimatedCharacter {
public:
    AnimatedCharacter(SkeletonDefinition skeleton,const AnimationGraphManifest& manifest,
        std::vector<AnimatedCharacterPart> parts,MorphHostConfig morph_config,
        std::vector<SkinMatrix> scales={});
    AnimatedCharacter(const AnimatedCharacter&)=delete;
    AnimatedCharacter& operator=(const AnimatedCharacter&)=delete;
    const AnimationModel& model() const {return model_;}
    const MorphHostState& morph() const {return morph_;}
    const std::vector<AnimatedCharacterGeometry>& geometry() const {if(geometry_stale_)materialize_geometry();return geometry_;}
    // Port-only (checkpoint 132vt2d, speed only; the PS Vita play host turns it on): step and write_node_locals
    // leave the skinning to the first geometry() after them, so a pose replaced before any read (the upper-body
    // pass rewrites the step's nodes) is not skinned. The geometry is the same function of the committed pose,
    // morph and parts; only a skinning overflow throws at the read instead of rolling the step back.
    static void set_deferred_skinning(bool on){deferred_skinning_=on;}
    static bool deferred_skinning(){return deferred_skinning_;}
    const std::vector<AnimatedCharacterPart>& parts() const {return parts_;}
    // ActorModel_Setup 0038c920 on a built character (game::SwitchBodies / RestoreBody, 001b469c /
    // 001b46cc): the part list replaced under the constructor's checks; the skeleton, pose, morph
    // state and scales stay, and the geometry is skinned again from the committed pose. Idle only.
    void replace_parts(std::vector<AnimatedCharacterPart> parts);
    // The committed pose's skinning of other meshes for the parts (the far LOD records +20c0 that
    // 003870c0 selects): meshes[i] stands for part i, null leaves it out (empty geometry). Rigid
    // skinning only: a part's morph binding is not applied.
    std::vector<AnimatedCharacterGeometry> skin_parts(std::span<const SkinnedMesh* const> meshes) const;
    // The same into a caller-owned list (checkpoint 132win10, port-side speed only: capacity reused per frame).
    void skin_parts(std::span<const SkinnedMesh* const> meshes,std::vector<AnimatedCharacterGeometry>& output) const;
    // Port-only (checkpoint 132md2, mod model replacement): the committed pose's placed palette, the one geometry()
    // and skin_parts skin by. Not game code.
    std::vector<SkinPaletteBone> committed_palette() const;
    // Port-only (132vc1 hw6, a profile bench; not game code): geometry()'s skinning of the committed pose and morph
    // into `output` (scratch: one SkinnedMesh per part), timed with clock_us: us[0] the placed palette, us[1] the
    // parts without a morph binding, us[2] the face / hand parts (their deform included).
    void timed_skinning(std::uint64_t (*clock_us)(),std::vector<AnimatedCharacterGeometry>& output,
                        std::vector<SkinnedMesh>& scratch,std::uint64_t (&us)[3]) const;
    void set_morph_state(const MorphHostState& state);
    void set_base_shape(std::uint8_t shape,bool enabled);
    // 00391810 (start_ms: +2d6c) and the speech pointer +2d68 cleared (001c76c0 / 001c8e10).
    void start_speech(const MorphSpeechTracks& tracks,std::int32_t start_ms=0);
    void stop_speech();
    void set_value(std::size_t channel,float value);
    void approach(std::size_t channel,float target,float maximum_step,bool wrapped=false);
    void set_target(std::size_t channel,float target,float duration,float damping,bool wrapped=false);
    void select(std::size_t selector,std::size_t value);
    void set_playback_rate(float rate);
    void apply_commands(std::span<const ActorChannelCommand> commands);
    void play(std::string_view motion,MotionInstallMode mode=MotionInstallMode::replace,
        std::optional<float> duration=std::nullopt);
    void play(const MotionRequest& request,MotionInstallMode mode);
    bool play_actor_motion(const ActorMotionRegistry&,const ActorMotionSelection&,
        ActorMotionTransitionState&,const MotionRequest& payload,MotionInstallMode mode);
    void queue(std::string_view motion,std::optional<float> duration=std::nullopt);
    void clear();
    // Coupled publication for attachment-mutating callbacks. Supply the same
    // draft captured by those callbacks; returned external root changes become
    // visible only after both owned model and attachment state can commit.
    std::vector<ActorAttachmentChange> play_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,
        const MotionRequest& request,MotionInstallMode mode);
    std::vector<ActorAttachmentChange> play_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,
        std::string_view motion,MotionInstallMode mode=MotionInstallMode::replace,std::optional<float> duration=std::nullopt);
    std::vector<ActorAttachmentChange> clear_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner);
    void set_delivery_hooks(MotionDeliveryHooks hooks);
    void set_anchor_placement(const RootAnchorKey& key,const RootVector& orientation,const RootVector& position);
    void set_anchor_world_orientation(const RootAnchorKey& key,const RootVector& orientation,bool reset_counter=false);
    void set_anchor_world_position(const RootAnchorKey& key,const RootVector& position);
    void set_anchor_world_placement(const RootAnchorKey& key,const RootParentTransform& placement,bool reset_counter=false);
    void set_anchor_state(const RootAnchorKey& key,const RootAnimationState& state);
    void set_anchor_parent(const RootAnchorKey& key,std::optional<RootParentTransform> parent);
    void set_anchor_constraint(const RootAnchorKey& key,std::optional<RootMotionConstraint> constraint);
    const SkeletonDefinition& skeleton() const {return skeleton_;}
    // AnimationModel::write_node_locals on the committed model (idle only);
    // the geometry is reskinned from the new pose.
    void write_node_locals(std::span<const std::optional<std::array<float,12>>> rows,const std::optional<RootVector>& root_translation);
    // Entire owned frame commits after commands, model, morph, deformation,
    // skinning and placement succeed. Receiver-owned side effects are external
    // transactions and cannot be undone by this wrapper.
    void step(const AnimationGraph& graph,const AnimationClipBank& clips,
        std::span<const ActorChannelCommand> commands,const MorphHostInput& morph_input,
        float elapsed,const RootMotionConstraint& constraint={},const AnimatedCharacterResourceInput& resources={},
        CharacterActorCurveFrame* actor_curves=nullptr);
    std::vector<ActorAttachmentChange> step_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,
        const AnimationGraph& graph,const AnimationClipBank& clips,std::span<const ActorChannelCommand> commands,
        const MorphHostInput& morph_input,float elapsed,const RootMotionConstraint& constraint={},
        const AnimatedCharacterResourceInput& resources={},CharacterActorCurveFrame* actor_curves=nullptr);
private:
    SkeletonDefinition skeleton_;
    std::vector<AnimatedCharacterPart> parts_;
    MorphHostConfig morph_config_;
    std::vector<SkinMatrix> inverse_bind_,scales_;
    AnimationModel model_;
    MorphHostState morph_;
    mutable std::vector<AnimatedCharacterGeometry> geometry_;
    mutable bool geometry_stale_{false};   // 132vt2d: geometry_ awaits materialize_geometry() (deferred skinning)
    inline static bool deferred_skinning_{false};
    void materialize_geometry() const;
    void materialize_before_change();
    // Checkpoint 132win10 (port-side speed only): the previous frame's geometry and the morph deformation's
    // mesh, kept for their storage (step_impl skins into spare_geometry_ and swaps it with geometry_ at commit).
    mutable std::vector<AnimatedCharacterGeometry> spare_geometry_;
    mutable std::vector<SkinnedMesh> morph_scratch_;   // 132vc1 hw6: one per part (a deformed copy reuses its storage)
    AnimationModel* candidate_{};
    AnimationModel& command_model();
    void require_idle() const;
    void check_parts(const std::vector<AnimatedCharacterPart>& parts) const;
    std::vector<ActorAttachmentChange> controller_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,
        const std::function<void(AnimationModel&)>& operation);
    std::vector<ActorAttachmentChange> step_impl(const AnimationGraph& graph,const AnimationClipBank& clips,
        std::span<const ActorChannelCommand> commands,const MorphHostInput& morph_input,float elapsed,
        const RootMotionConstraint& constraint,const AnimatedCharacterResourceInput& resources,
        ActorAttachmentDraft* attachments,ActorAttachmentRuntime* attachment_owner,CharacterActorCurveFrame* actor_curves);
    std::vector<SkinPaletteBone> placed_palette(const AnimationPlayerState& player) const;
    std::vector<AnimatedCharacterGeometry> build_geometry(const AnimationPlayerState& player,const MorphHostState& morph) const;
    void build_geometry_into(const AnimationPlayerState& player,const MorphHostState& morph,
        std::vector<AnimatedCharacterGeometry>& output,std::vector<SkinnedMesh>& morph_scratch) const;
};
}
