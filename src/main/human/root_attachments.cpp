#include "root_attachments.hpp"
#include "morph_resources.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
float finite(float x){check(std::isfinite(x)&&(!x||std::isnormal(x)),"Invalid root attachment arithmetic");return x;}
void validate(const RootAnimationState& state){
    for(const auto* vector:{&state.orientation,&state.position,&state.motion_cache,&state.pose_orientation,&state.pose_position})for(float x:*vector)finite(x);
    check(state.position[3]==1&&state.pose_position[3]==1,"Root attachment requires affine retained placements");
}
RootVector motion_to_world(const RootParentTransform& parent,const RootVector& local){
    const auto matrix=root_placement_matrix(parent);auto result=local;
    for(unsigned r=0;r<3;++r){float x=finite(matrix[r]*local[0]);
        x=finite(x+finite(matrix[4+r]*local[1]));result[r]=finite(x+finite(matrix[8+r]*local[2]));}
    return result;
}
RootVector motion_to_local(const RootParentTransform& parent,const RootVector& world){
    const auto& q=parent.orientation;
    const RootVector conjugate{finite(0.0F-q[0]),finite(0.0F-q[1]),finite(0.0F-q[2]),q[3]};
    // Original001a7370 rotates with the conjugate, preserving input W.
    return transform_root_vector({conjugate,{0,0,0,1}},world);
}
RootParentTransform pose_world(const RootAnimationState& local,const RootParentTransform& parent){
    auto temporary=local;temporary.orientation=local.pose_orientation;temporary.position=local.pose_position;
    return root_world_placement(temporary,&parent);
}
void restore_unparented(RootAnimationState& state,const RootAttachmentWorld& world){
    set_root_world_placement(state,world.placement);
    state.motion_cache=world.motion;state.dirty_motion=true;
    state.pose_orientation=world.pose.orientation;state.pose_position=world.pose.position;state.dirty_pose=true;
}
// A node without a parent in its collection: its local placement, through
// the other collection's parent when it has one.
RootParentTransform base(const RootAttachmentNode& n){
    if(n.external)return root_world_placement(n.local,&*n.external);
    return {n.local.orientation,n.local.position};
}
}
RootAttachmentId RootAttachments::add(const RootAnimationState& local){
    validate(local);check(nodes_.size()<4096,"Root attachment node limit");
    const auto id=nodes_.size();nodes_.push_back({local,std::nullopt,{}, {}});return id;
}
const RootAttachmentNode& RootAttachments::node(RootAttachmentId id)const{
    check(id<nodes_.size(),"Root attachment index");return nodes_[id];
}
void RootAttachments::invalidate_children(RootAttachmentId id){
    std::vector<RootAttachmentId> pending=nodes_[id].children;
    while(!pending.empty()){const auto next=pending.back();pending.pop_back();auto& child=nodes_[next];
        child.local.dirty_transform=child.local.dirty_motion=child.local.dirty_pose=true;
        pending.insert(pending.end(),child.children.begin(),child.children.end());}
}
RootParentTransform RootAttachments::placement(RootAttachmentId id){
    std::vector<RootAttachmentId> chain;auto cursor=id;
    while(nodes_[cursor].parent&&nodes_[cursor].local.dirty_transform){chain.push_back(cursor);cursor=*nodes_[cursor].parent;}
    for(auto it=chain.rbegin();it!=chain.rend();++it){auto& child=nodes_[*it];const auto& parent=nodes_[*child.parent];
        const RootParentTransform parent_world=parent.parent?parent.cached.placement:base(parent);
        child.cached.placement=root_world_placement(child.local,&parent_world);child.local.dirty_transform=false;}
    const auto& result=nodes_[id];return result.parent?result.cached.placement:base(result);
}
RootAttachmentWorld RootAttachments::snapshot(RootAttachmentId id){
    auto& n=nodes_[id];
    if(!n.parent&&n.external)return {base(n),motion_to_world(*n.external,n.local.motion_cache),pose_world(n.local,*n.external)};
    if(!n.parent)return {{n.local.orientation,n.local.position},n.local.motion_cache,{n.local.pose_orientation,n.local.pose_position}};
    const auto base=placement(id);const auto parent=placement(*n.parent);
    if(n.local.dirty_motion){n.cached.motion=motion_to_world(parent,n.local.motion_cache);n.local.dirty_motion=false;}
    if(n.local.dirty_pose){n.cached.pose=pose_world(n.local,parent);n.local.dirty_pose=false;}
    return {base,n.cached.motion,n.cached.pose};
}
RootAttachmentWorld RootAttachments::world(RootAttachmentId id){
    (void)node(id);auto next=*this;const auto result=next.snapshot(id);*this=std::move(next);return result;
}
RootParentTransform RootAttachments::world_placement(RootAttachmentId id){
    (void)node(id);auto next=*this;const auto result=next.placement(id);*this=std::move(next);return result;
}
RootParentTransform RootAttachments::world_pose(RootAttachmentId id){
    (void)node(id);auto next=*this;auto& selected=next.nodes_[id];
    if(!selected.parent&&selected.external)return pose_world(selected.local,*selected.external);
    if(!selected.parent)return {selected.local.pose_orientation,selected.local.pose_position};
    if(selected.local.dirty_pose){
        const auto parent=next.placement(*selected.parent);
        selected.cached.pose=pose_world(selected.local,parent);
        selected.local.dirty_pose=false;
    }
    const auto result=selected.cached.pose;*this=std::move(next);return result;
}
void RootAttachments::set_local(RootAttachmentId id,const RootAnimationState& local){
    (void)node(id);validate(local);auto next=*this;next.nodes_[id].local=local;
    auto& state=next.nodes_[id].local;state.dirty_transform=state.dirty_motion=state.dirty_pose=true;
    next.invalidate_children(id);*this=std::move(next);
}
void RootAttachments::set_world_position(RootAttachmentId id,const RootVector& position){
    (void)node(id);auto next=*this;auto& selected=next.nodes_[id];
    const auto parent=selected.parent?std::optional<RootParentTransform>{next.placement(*selected.parent)}:selected.external;
    set_root_world_position(selected.local,position,parent?&*parent:nullptr);
    validate(selected.local);next.invalidate_children(id);*this=std::move(next);
}
void RootAttachments::set_children_offset(RootAttachmentId parent,const RootParentTransform& offset){
    (void)node(parent);if(nodes_[parent].children.empty())return;
    auto next=*this;
    for(const auto child:next.nodes_[parent].children){
        auto& local=next.nodes_[child].local;
        set_root_placement(local,offset.orientation,offset.position);
        validate(local);next.invalidate_children(child);
    }
    *this=std::move(next);
}
void RootAttachments::set_world_placement(RootAttachmentId id,const RootParentTransform& placement_value,bool reset_counter){
    (void)node(id);auto next=*this;auto& selected=next.nodes_[id];
    const auto parent=selected.parent?std::optional<RootParentTransform>{next.placement(*selected.parent)}:selected.external;
    set_root_world_placement(selected.local,placement_value,parent?&*parent:nullptr);
    validate(selected.local);next.invalidate_children(id);
    if(reset_counter)selected.local.normalization_counter=0;
    *this=std::move(next);
}
void RootAttachments::set_world_orientation(RootAttachmentId id,const RootVector& orientation,bool reset_counter){
    (void)node(id);auto next=*this;auto& selected=next.nodes_[id];
    const auto parent=selected.parent?std::optional<RootParentTransform>{next.placement(*selected.parent)}:selected.external;
    set_root_world_orientation(selected.local,orientation,parent?&*parent:nullptr);
    validate(selected.local);next.invalidate_children(id);
    if(reset_counter)selected.local.normalization_counter=0;
    *this=std::move(next);
}
void RootAttachments::synchronize_local(RootAttachmentId id,const RootAnimationState& local){
    (void)node(id);validate(local);auto next=*this;
    next.nodes_[id].local=local;
    auto& imported=next.nodes_[id].local;
    imported.dirty_transform=imported.dirty_motion=imported.dirty_pose=true;
    next.invalidate_children(id);
    // Cache values from another model are not copied by the retained-state
    // snapshot. Reconstruct them before honoring its originally clean bits.
    (void)next.snapshot(id);
    imported.dirty_transform=local.dirty_transform;
    imported.dirty_motion=local.dirty_motion;
    imported.dirty_pose=local.dirty_pose;
    *this=std::move(next);
}
void RootAttachments::unlink(RootAttachmentId child){
    auto& node=nodes_[child];if(!node.parent)return;
    const auto saved=snapshot(child);auto& siblings=nodes_[*node.parent].children;
    const auto found=std::find(siblings.begin(),siblings.end(),child);check(found!=siblings.end(),"Root attachment parent invariant");
    *found=siblings.back();siblings.pop_back();node.parent.reset();
    restore_unparented(node.local,saved);invalidate_children(child);
}
void RootAttachments::attach(RootAttachmentId parent,RootAttachmentId child,bool reset_local_placement){
    (void)node(parent);(void)node(child);
    for(auto ancestor=std::optional<RootAttachmentId>{parent};ancestor;ancestor=nodes_[*ancestor].parent)
        check(*ancestor!=child,"Root attachment cycle");
    check(!nodes_[child].external,"Root attachment child has a parent in another collection");
    auto next=*this;next.unlink(child);const auto saved=next.snapshot(child);const auto parent_world=next.placement(parent);
    auto& n=next.nodes_[child];n.parent=parent;next.nodes_[parent].children.push_back(child);
    set_root_world_placement(n.local,saved.placement,&parent_world);
    next.invalidate_children(child);
    n.local.motion_cache=motion_to_local(parent_world,saved.motion);n.local.dirty_motion=true;
    auto temporary=n.local;set_root_world_placement(temporary,saved.pose,&parent_world);
    n.local.pose_orientation=temporary.orientation;n.local.pose_position=temporary.position;n.local.dirty_pose=true;
    if(reset_local_placement){set_root_placement(n.local,{0,0,0,1},{0,0,0,1});next.invalidate_children(child);}
    validate(n.local);*this=std::move(next);
}
void RootAttachments::detach(RootAttachmentId parent,RootAttachmentId child){
    (void)node(parent);(void)node(child);if(nodes_[child].parent!=parent)return;
    auto next=*this;next.unlink(child);*this=std::move(next);
}
std::vector<RootAttachmentId> RootAttachments::clear(RootAttachmentId parent){
    (void)node(parent);auto next=*this;std::vector<RootAttachmentId> removed;
    while(!next.nodes_[parent].children.empty()){const auto child=next.nodes_[parent].children.back();next.unlink(child);removed.push_back(child);}
    *this=std::move(next);return removed;
}
bool RootAttachments::parented(RootAttachmentId id)const{
    const auto& n=node(id);return n.parent.has_value()||n.external.has_value();
}
void RootAttachments::attach_external(RootAttachmentId child,const RootParentTransform& parent,bool reset_local_placement){
    (void)node(child);check(!nodes_[child].parent&&!nodes_[child].external,"Root attachment external parent of a parented node");
    auto next=*this;const auto saved=next.snapshot(child);auto& n=next.nodes_[child];
    (void)root_world_placement(n.local,&parent);
    n.external=parent;
    set_root_world_placement(n.local,saved.placement,&parent);
    n.local.motion_cache=motion_to_local(parent,saved.motion);n.local.dirty_motion=true;
    auto temporary=n.local;set_root_world_placement(temporary,saved.pose,&parent);
    n.local.pose_orientation=temporary.orientation;n.local.pose_position=temporary.position;n.local.dirty_pose=true;
    if(reset_local_placement)set_root_placement(n.local,{0,0,0,1},{0,0,0,1});
    validate(n.local);next.invalidate_children(child);*this=std::move(next);
}
void RootAttachments::update_external(RootAttachmentId child,const RootParentTransform& parent){
    (void)node(child);check(nodes_[child].external.has_value(),"Root attachment has no external parent");
    (void)root_world_placement(nodes_[child].local,&parent);
    auto next=*this;next.nodes_[child].external=parent;next.invalidate_children(child);*this=std::move(next);
}
void RootAttachments::detach_external(RootAttachmentId child){
    (void)node(child);if(!nodes_[child].external)return;
    auto next=*this;const auto saved=next.snapshot(child);auto& n=next.nodes_[child];
    n.external.reset();restore_unparented(n.local,saved);validate(n.local);next.invalidate_children(child);
    *this=std::move(next);
}
}
