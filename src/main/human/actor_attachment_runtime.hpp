#pragma once
#include "root_attachments.hpp"
#include "animation_controller.hpp"
#include <map>

namespace omega {
struct ActorAttachmentChange {
    RootAttachmentId node{};
    RootAnimationState local;
    RootAttachmentWorld world;
    std::optional<RootAttachmentId> parent;
};
class ActorAttachmentDraft;
class ActorAttachmentRuntime {
public:
    ActorAttachmentRuntime(RootAttachments tree,std::map<RootAnchorKey,RootAttachmentId> bindings);
    ActorAttachmentRuntime(const ActorAttachmentRuntime&)=delete;
    ActorAttachmentRuntime& operator=(const ActorAttachmentRuntime&)=delete;
    ActorAttachmentRuntime(ActorAttachmentRuntime&&)=delete;
    ActorAttachmentRuntime& operator=(ActorAttachmentRuntime&&)=delete;
    const RootAttachments& tree() const {return tree_;}
    void set_local(RootAttachmentId id,const RootAnimationState& local);
    void set_world_position(RootAttachmentId id,const RootVector& position);
    ActorAttachmentDraft begin_frame() const;
private:
    RootAttachments tree_;
    std::map<RootAnchorKey,RootAttachmentId> bindings_;
    std::uint64_t revision_{};
    friend class ActorAttachmentDraft;
};
// Caller-owned frame transaction: receiver callbacks operate on this draft,
// and detached-child publication is returned only from commit after the owning
// character/actor frame succeeds. No callbacks run from core tree operations.
class ActorAttachmentDraft {
public:
    ActorAttachmentDraft(const ActorAttachmentDraft&)=delete;
    ActorAttachmentDraft& operator=(const ActorAttachmentDraft&)=delete;
    ActorAttachmentDraft(ActorAttachmentDraft&& source) noexcept;
    ActorAttachmentDraft& operator=(ActorAttachmentDraft&& source) noexcept;
    // External models belong to the caller's actor transaction. Bound roots
    // instead synchronize from the actual delivery candidate at each operation.
    void stage_external_root(RootAttachmentId id,const RootAnimationState& state);
    void set_root_world_position(const RootAnchorKey& key,const RootVector& position,
        const MotionDeliveryView& view);
    void clear_root_children(const RootAnchorKey& key,const MotionDeliveryView& view);
    void attach_root_children(const RootAnchorKey& key,std::span<const RootAttachmentId> children,
        const MotionDeliveryView& view);
    std::vector<ActorAttachmentChange> commit(ActorAttachmentRuntime& destination);
private:
    explicit ActorAttachmentDraft(const ActorAttachmentRuntime& source);
    const ActorAttachmentRuntime* source_{};
    std::uint64_t revision_{};
    RootAttachments tree_;
    std::map<RootAnchorKey,RootAttachmentId> bindings_;
    std::vector<ActorAttachmentChange> pending_;
    bool committed_{};
    friend class ActorAttachmentRuntime;
};
}
