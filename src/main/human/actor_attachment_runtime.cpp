#include "actor_attachment_runtime.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
void check(bool x,const char* message){if(!x)throw std::runtime_error(message);}
void next_revision(std::uint64_t value){check(value<std::numeric_limits<std::uint64_t>::max(),"Attachment revision exhausted");}
AnimationPlayerAnchor& resolve(std::vector<AnimationPlayerAnchor>& anchors,const RootAnchorKey& selected){
    const auto at=std::find_if(anchors.begin(),anchors.end(),[&](const auto& value){return value.key==selected;});
    check(at!=anchors.end(),"Actor attachment root absent from candidate model");
    check(std::count_if(anchors.begin(),anchors.end(),[&](const auto& value){return value.key==selected;})==1,"Duplicate candidate root key");
    return *at;
}
using SynchronizedRoots=std::vector<std::pair<RootAttachmentId,RootAnchorKey>>;
SynchronizedRoots synchronize(RootAttachments& tree,const std::map<RootAnchorKey,RootAttachmentId>& bindings,
    std::vector<AnimationPlayerAnchor>& anchors,std::span<const RootAttachmentId> seeds,bool preserve_dirty=false){
    std::vector<RootAttachmentId> ordered;std::set<RootAttachmentId> visited;
    for(const auto seed:seeds){
        std::vector<RootAttachmentId> ancestors;
        for(auto at=std::optional<RootAttachmentId>{seed};at;at=tree.node(*at).parent)ancestors.push_back(*at);
        for(auto at=ancestors.rbegin();at!=ancestors.rend();++at)if(visited.insert(*at).second)ordered.push_back(*at);
        auto pending=tree.node(seed).children;
        for(std::size_t i=0;i<pending.size();++i){if(visited.insert(pending[i]).second)ordered.push_back(pending[i]);
            const auto& children=tree.node(pending[i]).children;pending.insert(pending.end(),children.begin(),children.end());}
    }
    SynchronizedRoots result;
    for(const auto id:ordered){
        const auto bound=std::find_if(bindings.begin(),bindings.end(),[&](const auto& value){return value.second==id;});
        if(bound==bindings.end())continue;
        auto& anchor=resolve(anchors,bound->first);const auto parent=tree.node(id).parent;
        check(parent.has_value()==anchor.parent.has_value(),"Actor attachment candidate parent relationship differs");
        if(parent){const auto world=tree.world(*parent).placement;
            check(world.orientation==anchor.parent->orientation&&world.position==anchor.parent->position,"Actor attachment resolved parent placement differs");}
        if(preserve_dirty)tree.synchronize_local(id,anchor.state);
        else tree.set_local(id,anchor.state);
        result.push_back({id,bound->first});
    }
    return result;
}
void publish(RootAttachments& tree,std::vector<AnimationPlayerAnchor>& anchors,const SynchronizedRoots& roots){
    for(const auto& [id,key]:roots){auto& anchor=resolve(anchors,key);const auto parent=tree.node(id).parent;
        anchor.parent=parent?std::optional<RootParentTransform>{tree.world(*parent).placement}:std::nullopt;}
    for(const auto& [id,key]:roots)resolve(anchors,key).state=tree.node(id).local;
}
}
ActorAttachmentRuntime::ActorAttachmentRuntime(RootAttachments tree,std::map<RootAnchorKey,RootAttachmentId> bindings)
    :tree_(std::move(tree)),bindings_(std::move(bindings)){
    check(!bindings_.empty()&&bindings_.size()<=256,"Actor attachment binding count");std::set<RootAttachmentId> distinct;
    for(const auto& [key,id]:bindings_){(void)key;(void)tree_.node(id);check(distinct.insert(id).second,"Actor attachment root bound more than once");}
}
void ActorAttachmentRuntime::set_local(RootAttachmentId id,const RootAnimationState& local){
    next_revision(revision_);tree_.set_local(id,local);++revision_;
}
void ActorAttachmentRuntime::set_world_position(RootAttachmentId id,const RootVector& position){
    next_revision(revision_);tree_.set_world_position(id,position);++revision_;
}
ActorAttachmentDraft ActorAttachmentRuntime::begin_frame()const{return ActorAttachmentDraft(*this);}
ActorAttachmentDraft::ActorAttachmentDraft(const ActorAttachmentRuntime& source)
    :source_(&source),revision_(source.revision_),tree_(source.tree_),bindings_(source.bindings_){}
ActorAttachmentDraft::ActorAttachmentDraft(ActorAttachmentDraft&& source) noexcept
    :source_(std::exchange(source.source_,nullptr)),revision_(source.revision_),tree_(std::move(source.tree_)),
     bindings_(std::move(source.bindings_)),pending_(std::move(source.pending_)),committed_(std::exchange(source.committed_,true)){}
ActorAttachmentDraft& ActorAttachmentDraft::operator=(ActorAttachmentDraft&& source) noexcept{
    if(this!=&source){
        source_=std::exchange(source.source_,nullptr);revision_=source.revision_;tree_=std::move(source.tree_);
        bindings_=std::move(source.bindings_);pending_=std::move(source.pending_);committed_=std::exchange(source.committed_,true);
    }
    return *this;
}
void ActorAttachmentDraft::stage_external_root(RootAttachmentId id,const RootAnimationState& state){
    check(source_&&!committed_,"Attachment draft is inactive");
    check(std::none_of(bindings_.begin(),bindings_.end(),[&](const auto& item){return item.second==id;}),"Bound attachment root requires live model candidate");
    tree_.set_local(id,state);
}
void ActorAttachmentDraft::set_root_world_position(const RootAnchorKey& key,const RootVector& position,
    const MotionDeliveryView& view){
    check(source_&&!committed_,"Attachment draft is inactive");
    check(view.candidate_,"Attachment position publication requires live model candidate");
    const auto binding=bindings_.find(key);check(binding!=bindings_.end(),"Actor attachment root key unresolved");
    auto next=tree_;auto anchors=view.candidate_->anchors;
    const std::array<RootAttachmentId,1> seeds{binding->second};
    const auto synchronized=synchronize(next,bindings_,anchors,seeds,true);
    next.set_world_position(binding->second,position);
    // Refresh parent snapshots of bound descendants before the next model
    // consumer. Unbound descendants stay in the staged tree with dirty caches.
    publish(next,anchors,synchronized);
    tree_=std::move(next);view.candidate_->anchors=std::move(anchors);
}
void ActorAttachmentDraft::clear_root_children(const RootAnchorKey& key,const MotionDeliveryView& view){
    check(source_&&!committed_,"Attachment draft is inactive");check(view.candidate_,"Attachment clear requires live model candidate");
    const auto binding=bindings_.find(key);check(binding!=bindings_.end(),"Actor attachment root key unresolved");
    auto next=tree_;auto releases=pending_;auto anchors=view.candidate_->anchors;
    // Synchronize the ancestors first, then the affected subtree. A bound
    // parent or child can have changed in an earlier callback this operation.
    const std::array<RootAttachmentId,1> seeds{binding->second};
    const auto synchronized=synchronize(next,bindings_,anchors,seeds);
    for(const auto child:next.clear(binding->second)){
        const auto world=next.world(child);releases.push_back({child,next.node(child).local,world});
    }
    // Bound children are live model anchors, so subsequent receivers in this
    // same operation must observe the detached state immediately. External
    // child publication remains private until the outer owner commits.
    publish(next,anchors,synchronized);
    tree_=std::move(next);pending_=std::move(releases);view.candidate_->anchors=std::move(anchors);
}
void ActorAttachmentDraft::attach_root_children(const RootAnchorKey& key,std::span<const RootAttachmentId> children,
    const MotionDeliveryView& view){
    check(source_&&!committed_,"Attachment draft is inactive");check(view.candidate_,"Attachment requires live model candidate");
    check(children.size()<=4096,"Related attachment count");
    const auto binding=bindings_.find(key);check(binding!=bindings_.end(),"Actor attachment root key unresolved");
    auto next=tree_;auto changes=pending_;auto anchors=view.candidate_->anchors;
    std::vector<RootAttachmentId> seeds{binding->second};seeds.insert(seeds.end(),children.begin(),children.end());
    const auto synchronized=synchronize(next,bindings_,anchors,seeds);
    // The original list is an ordered pointer vector. Repeated child entries
    // repeat detach/attach/reset; neither sorting nor deduplication is valid.
    for(const auto child:children){
        next.attach(binding->second,child,true);const auto world=next.world(child);
        changes.push_back({child,next.node(child).local,world,binding->second});
    }
    publish(next,anchors,synchronized);
    tree_=std::move(next);pending_=std::move(changes);view.candidate_->anchors=std::move(anchors);
}
std::vector<ActorAttachmentChange> ActorAttachmentDraft::commit(ActorAttachmentRuntime& destination){
    check(!committed_&&source_==&destination&&revision_==destination.revision_,"Stale or wrong-owner attachment draft");
    next_revision(destination.revision_);destination.tree_=std::move(tree_);++destination.revision_;committed_=true;
    return std::move(pending_);
}
}
