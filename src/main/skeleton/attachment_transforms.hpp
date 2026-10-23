#pragma once
#include "skinned_mesh.hpp"

namespace omega {
using AttachmentNodeId=std::size_t;
struct AttachmentTransformNode {
    SkinMatrix local{},world{};
    std::optional<AttachmentNodeId> parent;
    std::vector<AttachmentNodeId> children;
    bool dirty{true};
};
// Concrete matrix-node parent/local/world semantics. Distinct from the game's
// quaternion root attachment list and from ownership of inventory/model parts.
class AttachmentTransforms {
public:
    AttachmentNodeId add(const SkinMatrix& local);
    const AttachmentTransformNode& node(AttachmentNodeId id) const;
    SkinMatrix world(AttachmentNodeId id);
    void set_local(AttachmentNodeId id,const SkinMatrix& local);
    void set_world(AttachmentNodeId id,const SkinMatrix& world);
    void attach(AttachmentNodeId child,std::optional<AttachmentNodeId> parent,bool preserve_world);
    void detach(AttachmentNodeId child,bool preserve_world);
private:
    std::vector<AttachmentTransformNode> nodes_;
    void dirty(AttachmentNodeId id);
    void update(AttachmentNodeId id);
    void write_world(AttachmentNodeId id,const SkinMatrix& world);
    void unlink(AttachmentNodeId id,bool preserve_world);
};
}
