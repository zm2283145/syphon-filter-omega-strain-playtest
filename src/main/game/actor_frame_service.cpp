#include "actor_frame_service.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
constexpr std::size_t capacity=65536,world_visit_limit=1000000;
void elapsed_ok(float elapsed){
    if(!std::isfinite(elapsed)||elapsed<0||(elapsed!=0&&!std::isnormal(elapsed)))
        throw std::runtime_error("Actor service elapsed time must be finite nonnegative normal or zero");
}
bool eligible(const ActorFrameState& state){return state.flag2d!=0&&state.flag2c!=0&&state.flag2e==0;}
}
void ActorFrameService::reserve_logic_capacity(std::size_t requested){
    if(running_||requested>capacity)
        throw std::runtime_error("Actor service capacity change during traversal or excessive capacity is unsupported");
    if(requested>logic_capacity_){logic_order_.reserve(requested);logic_capacity_=requested;}
}
void ActorFrameService::register_actor(ActorFrameSlot slot,const ActorFrameState& state,
    ActorFrameCallbacks callbacks,bool in_logic_order){
    if(running_&&(!first_pass_||!in_logic_order||logic_order_.size()>=logic_capacity_))
        throw std::runtime_error("Actor service live registration requires first-pass append within reserved capacity");
    if(!slot||actors_.contains(slot)||actors_.size()>=capacity)
        throw std::runtime_error("Invalid, duplicate or excessive actor service slot");
    if(!callbacks.pre_logic||!callbacks.logic||!callbacks.pre_world||!callbacks.world)
        throw std::runtime_error("Actor service requires all four actual virtual-slot bindings");
    if(in_logic_order&&logic_order_.size()>=logic_capacity_)reserve_logic_capacity(logic_order_.size()+1);
    actors_.emplace(slot,Entry{state,std::move(callbacks)});
    if(in_logic_order)logic_order_.push_back(slot);
}
void ActorFrameService::remove_actor(ActorFrameSlot slot){
    if(running_)throw std::runtime_error("Actor service registration storage mutation during traversal is unsupported");
    if(!actors_.erase(slot))throw std::runtime_error("Unknown actor service slot");
    std::erase(logic_order_,slot);
    // Pending entries intentionally survive, just as stored pointer/identity
    // snapshots do. The second pass rejects a missing slot or changed identity.
}
ActorFrameState& ActorFrameService::actor(ActorFrameSlot slot){
    const auto found=actors_.find(slot);
    if(found==actors_.end())throw std::runtime_error("Unknown actor service slot");
    return found->second.state;
}
const ActorFrameState& ActorFrameService::actor(ActorFrameSlot slot) const{
    const auto found=actors_.find(slot);
    if(found==actors_.end())throw std::runtime_error("Unknown actor service slot");
    return found->second.state;
}
void ActorFrameService::queue_world(ActorFrameSlot slot){
    const auto& state=actor(slot);
    if(state.world_key<0)return;
    if(pending_.size()>=capacity)throw std::runtime_error("Actor service pending queue exceeds bounded capacity");
    pending_.emplace(state.world_key,Saved{slot,state.identity});
}
void ActorFrameService::logic_tail(ActorFrameSlot slot,float elapsed,const ActorFrameCallback& component){
    elapsed_ok(elapsed);
    actor(slot);
    if(component)component(*this,slot,elapsed);
    queue_world(slot);
}
void ActorFrameService::run(float elapsed){
    elapsed_ok(elapsed);
    if(running_)throw std::runtime_error("Recursive actor service traversal is unsupported");
    running_=true;
    first_pass_=true;
    struct Release{bool& flag;bool& first;~Release(){flag=false;first=false;}} release{running_,first_pass_};
    // Copy the current slot and reread the live end after callbacks. The
    // explicit capacity bound prevents relocation of the original iterator.
    for(std::size_t i=0;i<logic_order_.size();++i){
        const auto slot=logic_order_[i];
        auto& entry=actors_.at(slot);
        entry.callbacks.pre_logic(*this,slot,elapsed);
        if(eligible(entry.state))entry.callbacks.logic(*this,slot,elapsed);
    }
    first_pass_=false;
    std::size_t visits=0;
    // Increment AFTER callbacks: a newly inserted greater/equal key can be
    // reached this pass, while an insertion before this iterator is not.
    for(auto it=pending_.begin();it!=pending_.end();++it){
        if(++visits>world_visit_limit)throw std::runtime_error("Actor service world traversal exceeds bounded visit limit");
        const auto found=actors_.find(it->second.slot);
        if(found==actors_.end()||found->second.state.identity!=it->second.identity)continue;
        auto& entry=found->second;
        const auto slot=it->second.slot;
        entry.callbacks.pre_world(*this,slot,elapsed);
        // The original does not revalidate identity after this hook, but it
        // does freshly inspect eligibility before invoking the world body.
        if(eligible(entry.state))entry.callbacks.world(*this,slot,elapsed);
    }
    pending_.clear();
}
}
