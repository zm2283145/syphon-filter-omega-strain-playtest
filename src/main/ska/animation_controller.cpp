#include "animation_controller.hpp"
#include "morph_resources.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float finite(float value) {
    if(!std::isfinite(value)) throw std::runtime_error("Motion controller nonfinite value");
    return value;
}
float plus(float a,float b) { return finite(a+b); }
float minus(float a,float b) { return finite(a-b); }
float times(float a,float b) { return finite(a*b); }
struct OperationGuard {
    AnimationController& owner;
    explicit OperationGuard(AnimationController& c,bool locked=false):owner(c) {
        if(c.delivery_active)throw std::runtime_error("Recursive animation controller operation is unsupported");
        c.delivery_active=true;c.installation_locked=locked;
    }
    ~OperationGuard(){owner.delivery_active=false;owner.installation_locked=false;}
};
struct DeliveryContext {const MotionDeliveryHooks& hooks;AnimationPlayerState* player{};};
void emit(AnimationController& c,MotionEffect effect,const DeliveryContext& delivery) {
    c.effects.push_back(effect);
    const auto sink=(effect.flags&1)?delivery.hooks.immediate:delivery.hooks.enqueue;
    if(sink)sink(effect,MotionDeliveryView{c,delivery.player,c.installation_locked});
}
void validate(const MotionRequest& r) {
    for(auto value:{r.width,r.bias,r.rate,r.secondary_rate,r.limit,r.initial_phase}) finite(value);
    if((r.limit<0&&r.limit!=-1)||r.initial_phase<0||r.initial_phase>1||r.phase_hooks.size()>10000)
        throw std::runtime_error("Motion request outside bounded domain");
    for(const auto& hook:r.phase_hooks) {finite(hook.start);finite(hook.end);}
}
bool token(AnimationController& c,MotionRecord& r,int slot,const DeliveryContext& delivery,bool clear_pair=false,bool disable=false) {
    auto& t=r.request.notifications.at(static_cast<std::size_t>(slot));
    if(!r.request.notifications_active||t.id==-1||!t.receiver) return false;
    emit(c,{MotionEffect::Kind::token,r.host,t.receiver,t.id,slot,0,5,0},delivery);
    if(disable) r.request.notifications_active=false;
    if(clear_pair) t={};
    return true;
}
void embedded(AnimationController& c,MotionRecord& r,int slot,int action,float dt,const DeliveryContext& delivery) {
    auto& message=r.request.messages.at(static_cast<std::size_t>(slot));
    if(message.receiver&&!message.sent)
        emit(c,{MotionEffect::Kind::embedded,r.host,message.receiver,message.id,slot,action,message.flags,dt},delivery);
    if(action==4||action==5) message.sent=true;
}
void cancel(AnimationController& c,std::vector<MotionRecord>& records,const DeliveryContext& delivery) {
    for(auto& r:records) {embedded(c,r,0,5,0,delivery);embedded(c,r,1,5,0,delivery);token(c,r,6,delivery,false,true);}
}
MotionRecord record(AnimationController& c,const MotionRequest& request) {
    if(c.active.size()+c.queued.size()>=256||c.next_host==0||c.next_host==std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Motion controller record limit");
    MotionRecord r;
    r.host=c.next_host++;r.request=request;
    r.blend={0,request.rate,request.width,request.bias,0,request.limit};
    return r;
}
void append(AnimationController& c,MotionRecord r,float progress,bool notify,const DeliveryContext& delivery) {
    r.blend.progress=progress;r.starting=true;
    c.active.push_back(std::move(r));
    if(notify) {token(c,c.active.back(),0,delivery);c.active.back().starting=false;}
}
bool selector_valid(const MotionGroupSelector& s) {return s.node.has_value()||s.group!=-1;}
bool matches(const AnimationPlayerGroup& group,const MotionGroupSelector& selector,const AnimationGraph& graph) {
    // Filled from original one-member group identity; group number is the
    // fallback only when the contribution list does not have exactly one item.
    if(group.members.size()==1) {
        const auto node=group.members[0].node;
        const auto number=graph.nodes().at(node).leaf.group;
        if(number==-1) return selector.node==node&&selector.group==-1;
        return !selector.node&&selector.group==number;
    }
    return !selector.node&&selector.group==group.number;
}
const AnimationPlayerGroup* selected_group(const MotionRecord& record,
    const std::vector<AnimationPlayerGroup>& groups,const AnimationGraph& graph) {
    const auto at=std::find_if(groups.begin(),groups.end(),[&](const auto& group){
        return group.host==record.host&&matches(group,record.request.selector,graph);
    });
    return at==groups.end()?nullptr:&*at;
}
float remaining_time(const MotionRecord& r,const std::vector<AnimationPlayerGroup>& groups,const AnimationGraph& graph) {
    if(r.blend.limit!=-1) return minus(r.blend.limit,r.blend.elapsed);
    const auto* group=selected_group(r,groups,graph);
    if(!group||group->phase.boundary) return 0;
    return times(group->totals.weighted_duration,minus(1,group->phase.current));
}
bool complete(const MotionRecord& r,const std::vector<AnimationPlayerGroup>& groups,const AnimationGraph& graph) {
    if(r.blend.limit!=-1) return r.blend.limit==r.blend.elapsed;
    const auto* group=selected_group(r,groups,graph);
    return group&&group->phase.boundary;
}
bool crossing(float previous,float current,float start,float end) {
    const auto overlap=[](float a,float b,float c,float d){return d>=a&&c<b;};
    if(previous<=current&&start<=end) return overlap(previous,current,start,end);
    if(current<previous&&start<=end)
        return overlap(minus(previous,1),current,start,end)||overlap(previous,plus(current,1),start,end);
    if(previous<=current&&end<start)
        return overlap(previous,current,minus(start,1),end)||overlap(previous,current,start,plus(end,1));
    return overlap(previous,plus(current,1),start,plus(end,1));
}
struct Visited {std::uint64_t host;bool starting,leaving;TransitionBlendResult blend;};
MotionRecord& find_record(AnimationController& c,std::uint64_t host) {
    const auto found=std::find_if(c.active.begin(),c.active.end(),[&](const auto& r){return r.host==host;});
    if(found==c.active.end()) throw std::runtime_error("Motion controller lost active record");
    return *found;
}
}

MotionDeliveryView MotionDeliveryView::with_channel_setter(ChannelSetter setter) const {
    auto result=*this;result.channel_setter_=std::move(setter);return result;
}
MotionDeliveryView MotionDeliveryView::with_channel_target_setter(ChannelTargetSetter setter) const {
    auto result=*this;result.channel_target_setter_=std::move(setter);return result;
}
bool MotionDeliveryView::can_set_channel_target(std::size_t index) const {
    return candidate_&&channel_target_setter_&&index<candidate_->graph.channels.size();
}
void MotionDeliveryView::set_channel_target(std::size_t index,float target,float duration,float damping,bool wrapped) const {
    if(!can_set_channel_target(index))throw std::runtime_error("Animation callback has no resolved target channel");
    channel_target_setter_(index,target,duration,damping,wrapped);
}
void MotionDeliveryView::set_channel_value(std::size_t index,float value) const {
    if(!candidate_||!channel_setter_||index>=candidate_->graph.channels.size())
        throw std::runtime_error("Animation callback has no resolved scalar channel");
    const auto current=channel_setter_(index,value);
    candidate_->graph.channels[index]=current;
}
bool MotionDeliveryView::can_set_channel_value(std::size_t index) const {
    return candidate_&&channel_setter_&&index<candidate_->graph.channels.size();
}
void MotionDeliveryView::set_anchor_placement(const RootAnchorKey& key,const RootVector& orientation,const RootVector& position) const {
    if(!candidate_)throw std::runtime_error("Animation root placement requires a frame delivery candidate");
    const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
    if(at==candidate_->anchors.end())throw std::runtime_error("Animation delivery root anchor is unavailable");
    auto next=at->state;
    set_root_placement(next,orientation,position);
    (void)root_world_placement(next,at->parent?&*at->parent:nullptr);
    at->state=std::move(next);
}
void MotionDeliveryView::set_anchor_orientation(const RootAnchorKey& key,const RootVector& orientation) const {
    if(!candidate_)throw std::runtime_error("Animation root orientation requires a frame delivery candidate");
    const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
    if(at==candidate_->anchors.end())throw std::runtime_error("Animation delivery root anchor is unavailable");
    set_anchor_placement(key,orientation,at->state.position);
}
void MotionDeliveryView::copy_unparented_anchor_orientation(const RootAnchorKey& destination,const RootAnchorKey& source) const {
    if(!candidate_)throw std::runtime_error("Animation orientation transfer requires a frame delivery candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation orientation transfer root unavailable");
        if(at->parent)throw std::runtime_error("Parented actor orientation transfer is unsupported");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    auto next=to->state;
    set_root_placement(next,from->state.orientation,next.position);
    next.normalization_counter=0;
    to->state=std::move(next);
}
void MotionDeliveryView::copy_unparented_anchor_placement(const RootAnchorKey& destination,const RootAnchorKey& source) const {
    if(!candidate_)throw std::runtime_error("Animation placement transfer requires a frame delivery candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation placement transfer root unavailable");
        if(at->parent)throw std::runtime_error("Parented actor placement transfer is unsupported");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    auto next=to->state;set_root_placement(next,from->state.orientation,from->state.position);
    next.normalization_counter=0;to->state=std::move(next);
}
void MotionDeliveryView::copy_anchor_world_orientation(const RootAnchorKey& destination,const RootAnchorKey& source) const {
    if(!candidate_)throw std::runtime_error("Animation world orientation transfer requires a frame candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation world transfer root unavailable");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    const auto world=root_world_placement(from->state,from->parent?&*from->parent:nullptr);
    auto next=to->state;set_root_world_orientation(next,world.orientation,to->parent?&*to->parent:nullptr);
    next.normalization_counter=0;to->state=std::move(next);
}
void MotionDeliveryView::copy_anchor_world_placement(const RootAnchorKey& destination,const RootAnchorKey& source) const {
    if(!candidate_)throw std::runtime_error("Animation world placement transfer requires a frame candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation world transfer root unavailable");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    const auto world=root_world_placement(from->state,from->parent?&*from->parent:nullptr);
    auto next=to->state;set_root_world_placement(next,world,to->parent?&*to->parent:nullptr);
    next.normalization_counter=0;to->state=std::move(next);
}
void MotionDeliveryView::copy_anchor_world_orientation(const RootAnchorKey& destination,const RootAnchorKey& source,const RootVector& pre_rotation) const {
    if(!candidate_)throw std::runtime_error("Animation orientation composition requires a model candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation orientation composition root unavailable");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    const auto source_world=root_world_placement(from->state,from->parent?&*from->parent:nullptr);
    auto rotation=make_initial_root_animation();rotation.orientation=source_world.orientation;
    const RootParentTransform offset{pre_rotation,{0,0,0,1}};
    // Same ordered quaternion product as original001a7df0: offset*source.
    const auto combined=root_world_placement(rotation,&offset).orientation;
    auto next=to->state;set_root_world_orientation(next,combined,to->parent?&*to->parent:nullptr);
    next.normalization_counter=0;to->state=std::move(next);
}

MotionRequest make_motion_request(std::size_t node,float duration) {
    if(finite(duration)<0) throw std::runtime_error("Negative crossfade duration");
    MotionRequest r;r.node=node;r.width=r.bias=duration;
    r.rate=r.secondary_rate=duration==0?std::numeric_limits<float>::max():finite(1.0F/duration);
    return r;
}
void MotionDeliveryView::copy_anchor_world_heading(const RootAnchorKey& destination,const RootAnchorKey& source,const RootVector& post_rotation) const {
    if(!candidate_)throw std::runtime_error("Animation world heading requires a model candidate");
    const auto find=[&](const RootAnchorKey& key){
        const auto at=std::find_if(candidate_->anchors.begin(),candidate_->anchors.end(),[&](const auto& anchor){return anchor.key==key;});
        if(at==candidate_->anchors.end())throw std::runtime_error("Animation world heading root unavailable");
        return at;
    };
    const auto from=find(source);const auto to=find(destination);
    const auto source_world=root_world_placement(from->state,from->parent?&*from->parent:nullptr);
    auto rotation=make_initial_root_animation();rotation.orientation=post_rotation;
    const RootParentTransform offset{source_world.orientation,{0,0,0,1}};
    const auto combined=root_world_placement(rotation,&offset).orientation;
    const auto basis=root_placement_matrix({combined,{0,0,0,1}});
    const float heading=basis[2]<basis[6]?std::atan2(basis[1],basis[0]):std::atan2(-basis[4],basis[5]);
    const float half=heading*.5F;
    const RootVector yaw{0,0,std::sin(half),std::cos(half)};
    auto next=to->state;set_root_world_orientation(next,yaw,to->parent?&*to->parent:nullptr);
    next.normalization_counter=0;to->state=std::move(next);
}
void install_motion(AnimationController& controller,const MotionRequest& request,MotionInstallMode mode,const MotionDeliveryHooks& hooks) {
    if(controller.installation_locked)return;
    OperationGuard guard(controller);
    const DeliveryContext delivery{hooks};
    validate(request);
    auto next=controller;next.effects.clear();
    if(mode==MotionInstallMode::replace_if_different) {
        const auto* current=!next.queued.empty()?&next.queued.back():(!next.active.empty()?&next.active.back():nullptr);
        if(current&&current->request.node==request.node) {controller=std::move(next);return;}
    }
    if(mode==MotionInstallMode::reset) {
        cancel(next,next.active,delivery);next.active.clear();cancel(next,next.queued,delivery);next.queued.clear();
        append(next,record(next,request),1,true,delivery);
    } else if(mode==MotionInstallMode::replace||mode==MotionInstallMode::replace_if_different) {
        cancel(next,next.active,delivery);cancel(next,next.queued,delivery);next.queued.clear();
        append(next,record(next,request),0,true,delivery);
    } else if(mode==MotionInstallMode::queue) {
        if(next.active.empty()) throw std::runtime_error("Queued motion requires an initialized active record");
        next.queued.push_back(record(next,request));
    } else throw std::runtime_error("Invalid motion installation mode");
    controller=std::move(next);
}
void clear_motions(AnimationController& controller,const MotionDeliveryHooks& hooks) {
    OperationGuard guard(controller);
    const DeliveryContext delivery{hooks};
    auto next=controller;next.effects.clear();cancel(next,next.active,delivery);cancel(next,next.queued,delivery);
    next.active.clear();next.queued.clear();controller=std::move(next);
}
void update_animation_controller(AnimationController& controller,AnimationPlayerState& player,
    const AnimationGraph& graph,const SkeletonDefinition& skeleton,const AnimationClipBank& clips,
    float dt,const RootMotionConstraint& constraint,const MotionDeliveryHooks& hooks) {
    OperationGuard guard(controller,true);
    DeliveryContext delivery{hooks};
    if(finite(dt)<0||controller.active.size()+controller.queued.size()>256||
        (controller.active.empty()&&(!controller.queued.empty()||controller.terminal_threshold_enabled)))
        throw std::runtime_error("Motion controller requires bounded state");
    auto next=controller;auto next_player=player;next.effects.clear();
    finite(next.terminal_width);finite(next.terminal_bias);
    std::vector<Visited> visited;
    std::vector<AnimationRootRequest> requests;
    float weight=1;bool paused=false;
    for(auto at=next.active.rbegin();at!=next.active.rend();++at) {
        validate(at->request);
        AnimationRootRequest root{at->host,at->request.node,0,at->request.initial_phase,{0,0,0,1},false,false};
        if(weight>0) {
            const auto result=step_transition_blend(at->blend,weight,dt,paused);
            visited.push_back({at->host,at->starting,at->leaving,result});
            at->starting=at->leaving=false;
            root.weight=result.sampled_weight;root.paused=result.paused;root.submit_node=result.submit_node;
            weight=result.remaining_weight;paused=result.boundary;
        }
        requests.push_back(root);
    }
    const auto after_graph=[&](std::vector<AnimationPlayerGroup>& groups,AnimationPlayerState& candidate) {
        delivery.player=&candidate;
        for(auto at=visited.rbegin();at!=visited.rend();++at) {
            auto& r=find_record(next,at->host);
            if(r.request.notifications_active&&selector_valid(r.request.selector)&&!r.request.phase_hooks.empty()) {
                const auto* group=selected_group(r,groups,graph);
                if(!group) throw std::runtime_error("Phase hooks require a resolved group");
                for(std::size_t i=0;i<r.request.phase_hooks.size();++i) {
                    const auto& hook=r.request.phase_hooks[i];
                    if(crossing(group->phase.previous,group->phase.current,hook.start,hook.end))
                        emit(next,{MotionEffect::Kind::token,r.host,hook.token.receiver,hook.token.id,
                            static_cast<int>(7+i),0,5,0},delivery);
                }
            }
            if(at->starting) {embedded(next,r,0,1,dt,delivery);embedded(next,r,1,1,dt,delivery);token(next,r,0,delivery);}
            if(at->blend.crossed) {embedded(next,r,0,2,dt,delivery);embedded(next,r,1,4,dt,delivery);token(next,r,1,delivery);}
            if(at->leaving) {embedded(next,r,0,3,dt,delivery);token(next,r,3,delivery);}
            if(complete(r,groups,graph)) {embedded(next,r,0,4,dt,delivery);token(next,r,4,delivery,true);}
            for(auto& group:groups) if(group.host==r.host) group.paused=at->blend.paused;
        }
        if(next.active.size()>=2) {
            std::size_t retain=0;
            for(std::size_t i=next.active.size()-1;i>0;--i) if(next.active[i].blend.progress==1) {retain=i;break;}
            if(retain) {
                if(next.active.size()==2) {embedded(next,next.active.front(),0,4,dt,delivery);token(next,next.active.front(),5,delivery,false,true);}
                next.active.erase(next.active.begin(),next.active.begin()+static_cast<std::ptrdiff_t>(retain));
            }
        } else if(!next.active.empty()) {
            auto& r=next.active.back();
            if(!next.queued.empty()) {
                const auto& queued=next.queued.front().request;
                if(remaining_time(r,groups,graph)<=times(plus(queued.width,queued.bias),.5F)) {
                    r.leaving=true;
                    auto incoming=std::move(next.queued.front());next.queued.erase(next.queued.begin());
                    append(next,std::move(incoming),0,false,delivery);
                }
            } else if(next.terminal_threshold_enabled&&remaining_time(r,groups,graph)<=times(plus(next.terminal_width,next.terminal_bias),.5F)) {
                token(next,r,2,delivery,true);
            }
        }
        std::vector<std::uint64_t> hosts;
        for(const auto& r:next.active) hosts.push_back(r.host);
        return hosts;
    };
    update_animation_player(next_player,graph,skeleton,clips,requests,dt,constraint,after_graph);
    controller=std::move(next);player=std::move(next_player);
}
}
