#pragma once
#include "animation_root.hpp"
#include <vector>

namespace omega {
using RootAttachmentId=std::size_t;
struct RootAttachmentWorld {
    RootParentTransform placement;
    RootVector motion{};
    RootParentTransform pose;
};
struct RootAttachmentNode {
    RootAnimationState local;
    std::optional<RootAttachmentId> parent;
    std::vector<RootAttachmentId> children;
    RootAttachmentWorld cached;
    // A parent root of another actor's collection (the body-carry link
    // 001a6d40 binds a carried body's 'edge' under the carrier's 'edge'): its
    // world placement as the owner last published it. Only for a node
    // without a parent in this collection.
    std::optional<RootParentTransform> external;
};
// Original quaternion-root relationship operations. Separate from matrix bone
// attachments, inventory ownership and a live object registry.
class RootAttachments {
public:
    RootAttachmentId add(const RootAnimationState& local);
    const RootAttachmentNode& node(RootAttachmentId id) const;
    RootAttachmentWorld world(RootAttachmentId id);
    // Placement getter must not resolve unrelated motion/pose caches.
    RootParentTransform world_placement(RootAttachmentId id);
    // Original001e56a0/001a7560: only parent placement and own pose cache.
    // Leaves own placement/motion and parent pose/motion caches untouched.
    RootParentTransform world_pose(RootAttachmentId id);
    void set_local(RootAttachmentId id,const RootAnimationState& local);
    // Position-only original publication: own transform becomes dirty;
    // every descendant's transform, motion and pose caches become dirty.
    void set_world_position(RootAttachmentId id,const RootVector& position);
    // Original 00191100, with optional caller's subsequent counter reset.
    // Only own transform and descendant caches become dirty.
    void set_world_placement(RootAttachmentId id,const RootParentTransform&,bool reset_counter=false);
    void set_world_orientation(RootAttachmentId id,const RootVector&,bool reset_counter=false);
    // Physical response 001e5120: replace each immediate child's local base
    // placement, dirty only its transform, then invalidate all descendant caches.
    // Empty child lists do not inspect the supplied placement.
    void set_children_offset(RootAttachmentId parent,const RootParentTransform& placement);
    // Authored import of a live model snapshot. Rebuilds clean world caches
    // while preserving its own dirty bits; descendants are invalidated.
    void synchronize_local(RootAttachmentId id,const RootAnimationState& local);
    void attach(RootAttachmentId parent,RootAttachmentId child,bool reset_local_placement=false);
    void detach(RootAttachmentId parent,RootAttachmentId child);
    // Original clear removes last children first; returned order exposes that.
    std::vector<RootAttachmentId> clear(RootAttachmentId parent);
    // The node has a parent root (in this collection or another actor's).
    bool parented(RootAttachmentId id) const;
    // 001a6d40 with a parent root of another collection: attach (keeping the
    // world placement, or with reset_local_placement the identity local), a
    // later publication of that parent's placement (the local is kept, as for
    // a parent in this collection), and the unlink (the world is kept).
    void attach_external(RootAttachmentId child,const RootParentTransform& parent,bool reset_local_placement=false);
    void update_external(RootAttachmentId child,const RootParentTransform& parent);
    void detach_external(RootAttachmentId child);
private:
    std::vector<RootAttachmentNode> nodes_;
    void invalidate_children(RootAttachmentId id);
    RootParentTransform placement(RootAttachmentId id);
    RootAttachmentWorld snapshot(RootAttachmentId id);
    void unlink(RootAttachmentId child);
};
}
