#include "actor_world.hpp"
#include "actor_movement_scheduler.hpp"
#include "morph_resources.hpp"
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace omega {
// The services 001e7f40 / 001e8760 read, over the world's actors. Actor and
// list-element ids are actor handles (an element stands for the actor's AI
// component; its +58 +30 owner is the actor itself).
class ActorWorld::MovementHost final:public ActorMovementWorldHost {
public:
    explicit MovementHost(ActorWorld& world):world_(world){}
    bool class_flag(std::uint64_t a) override{return (actor(a).state_words().current_3204&1U)!=0;}
    std::uint8_t mode_108(std::uint64_t a) override{return actor(a).life_state();}
    // The player component's +54 answers true, an AI component's +58 (the
    // NPC test the list pass's message checks); neither answers +5c. An actor
    // in a port-only agent slot 1..3 answers as that agent's player component.
    std::optional<bool> controller_54(std::uint64_t a) override{return in_agent_slot(a)||actor(a).player_controller();}
    std::optional<bool> controller_58(std::uint64_t a) override{return !in_agent_slot(a)&&!actor(a).player_controller();}
    std::optional<bool> controller_5c(std::uint64_t) override{return false;}
    std::int32_t physical_mode(std::uint64_t a) override{return actor(a).physical().response.physical.mode_20;}
    bool physical_enabled(std::uint64_t a) override{return actor(a).physical_3d0()!=0;}
    Bounds local_bounds(std::uint64_t a) override{return actor(a).context_bounds();}
    CollisionMatrix refreshed_world_transform(std::uint64_t a) override{
        return root_placement_matrix(actor(a).component_placement());}
    bool byte_2c(std::uint64_t a) override{return actor(a).root_bytes()[0]!=0;}
    bool byte_2d(std::uint64_t a) override{return actor(a).root_bytes()[1]!=0;}
    bool byte_2e(std::uint64_t a) override{return actor(a).root_bytes()[2]!=0;}
    std::uint8_t byte_14(std::uint64_t a) override{return actor(a).byte_14();}
    // Actor +49: 2 for every human actor (the constructor's store).
    std::uint8_t byte_49(std::uint64_t a) override{actor(a);return 2;}
    RootVector position_e0(std::uint64_t a) override{return actor(a).position_e0();}
    // 001e8ca0: the target is in the self's relation tree (+2fc0).
    bool related(std::uint64_t self,std::uint64_t target) override{return actor(self).relation_root()==actor(target).relation_root();}
    // end() is read first (001e7f40 captures it): the list is taken then.
    std::uint64_t list_begin() override{return 0;}
    std::uint64_t list_end() override{
        list_=world_.movement_list_?world_.movement_list_():std::vector<std::size_t>{};
        return list_.size();
    }
    std::uint64_t list_next(std::uint64_t node) override{return node+1;}
    std::optional<std::uint64_t> list_element(std::uint64_t node) override{
        const auto index=list_.at(static_cast<std::size_t>(node));
        if(index>=world_.actors_.size())throw std::runtime_error("Actor world: a movement list entry names an unknown actor");
        if(world_.destroyed_[index])return std::nullopt;
        return handle(index);
    }
    std::optional<std::uint64_t> element_owner(std::uint64_t element) override{actor(element);return element;}
    std::optional<std::uint64_t> player_slot(int slot) override{
        if(slot>=1&&slot<=3){
            const auto& s=world_.agent_slots_[static_cast<std::size_t>(slot-1)];
            if(!s||world_.destroyed_[*s])return std::nullopt;
            return handle(*s);
        }
        if(slot!=0||!world_.first_player_)return std::nullopt;
        return handle(*world_.first_player_);
    }
    float secondary_root_weight(std::uint64_t a) override{return actor(a).root_weight(ActorRoot::edge);}
    float tertiary_root_weight(std::uint64_t a) override{return actor(a).root_weight(ActorRoot::ladder);}
    void send_interaction(std::uint64_t self,std::uint64_t target,std::int32_t action) override{
        if(action!=7)throw std::runtime_error("Actor world: 001e7f40 sends only action 7");
        const auto from=index(self),to=index(target);
        if(world_.movement_contact_)world_.movement_contact_(from,to);
    }
private:
    static std::uint64_t handle(std::size_t index){return SoloCollisionWorld::actor_handle+index;}
    bool in_agent_slot(std::uint64_t h) const{return world_.in_agent_slot(index(h));}
    std::size_t index(std::uint64_t h) const{
        if(h<SoloCollisionWorld::actor_handle||h-SoloCollisionWorld::actor_handle>=world_.actors_.size())
            throw std::runtime_error("Actor world: the movement pass named an unknown actor");
        const auto i=static_cast<std::size_t>(h-SoloCollisionWorld::actor_handle);
        if(world_.destroyed_[i])throw std::runtime_error("Actor world: the movement pass read a deleted actor");
        return i;
    }
    SoloActorSimulation& actor(std::uint64_t h){return *world_.actors_[index(h)];}
    ActorWorld& world_;
    std::vector<std::size_t> list_;
};

class ActorWorld::WaterActors final:public WaterEffectActorHost {
public:
    explicit WaterActors(ActorWorld& world):world_(world){}
    // The record's weak owner (+0 / +4): an actor handle 0x100000 + index; a deleted actor no longer resolves.
    std::optional<std::int32_t> generation(std::uint32_t actor) override{
        const auto i=index(actor);
        if(!i||world_.destroyed_[*i])return std::nullopt;
        return world_.serials_[*i];
    }
    bool has_transform_component(std::uint32_t) override{return true;}
    // The same authored bounds as the standalone view (SoloWaterActors).
    std::optional<WaterActorBounds> bounds(std::uint32_t) override{return WaterActorBounds{{-1,-1,0,1},{1,1,6,1}};}
    std::uint8_t byte_49(std::uint32_t) override{return 2;}
    RootVector refreshed_translation(std::uint32_t actor) override{return world_.actors_.at(require(actor))->component();}
    RootVector current_translation(std::uint32_t actor) override{return world_.actors_.at(require(actor))->component();}
private:
    std::optional<std::size_t> index(std::uint32_t actor) const{
        if(actor<SoloCollisionWorld::actor_handle||actor-SoloCollisionWorld::actor_handle>=world_.actors_.size())return std::nullopt;
        return static_cast<std::size_t>(actor-SoloCollisionWorld::actor_handle);
    }
    std::size_t require(std::uint32_t actor) const{
        const auto i=index(actor);
        if(!i||world_.destroyed_[*i])throw std::runtime_error("Actor world: a water-effect read of an unknown or deleted actor");
        return *i;
    }
    ActorWorld& world_;
};
// The sink the actors hold: it forwards to the host's sink (set_water_sink), or answers the standalone stand-in.
class ActorWorld::DefaultWaterSink final:public WaterEffectSink {
public:
    WaterEffectSink* target{};
    WaterRecordResources record_resources() override{return target?target->record_resources():WaterRecordResources{0x28,0x27,1};}
    void request(const WaterEffectRequest& r) override{if(target)target->request(r);}
    void splash(std::uint8_t id,const RootVector& p) override{if(target)target->splash(id,p);}
};

ActorWorld::ActorWorld(SoloCollisionWorld world):world_(std::make_shared<SoloCollisionWorld>(std::move(world))),
    movement_(std::make_unique<MovementHost>(*this)){
    water_=std::make_unique<WaterEffectManager>(random_);   // 00137ab0 -> 002b3070: 48 game-RNG draws
    water_actors_=std::make_unique<WaterActors>(*this);
    default_water_sink_=std::make_unique<DefaultWaterSink>();
}
ActorWorld::~ActorWorld()=default;
WaterEffectManager& ActorWorld::water(){return *water_;}
WaterEffectActorHost& ActorWorld::water_actors(){return *water_actors_;}
void ActorWorld::set_water_sink(WaterEffectSink* sink){default_water_sink_->target=sink;}
WaterEffectSink& ActorWorld::water_sink(){return *default_water_sink_;}
std::unique_ptr<SoloActorSimulation> ActorWorld::make(std::size_t index,const SoloActorConfig& config,SoloActorAnimation* animation,
    std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial,const SoloActorController& controller,bool present){
    SoloActorWorldLink link{world_,SoloCollisionWorld::actor_handle+index,0x200000+index,&random_,&frame_counter_,
        water_.get(),water_actors_.get(),default_water_sink_.get()};
    std::unique_ptr<SoloActorSimulation> actor(new SoloActorSimulation(link,config,animation,std::move(tuning),initial,controller,present));
    actor->set_movement_world(movement_.get());
    if(step_dt_)actor->set_step_dt(*step_dt_);
    actor->set_actor_peers([this](std::uint64_t h)->SoloActorSimulation*{
        if(h<SoloCollisionWorld::actor_handle||h-SoloCollisionWorld::actor_handle>=actors_.size())return nullptr;
        const auto i=static_cast<std::size_t>(h-SoloCollisionWorld::actor_handle);
        return destroyed_[i]?nullptr:actors_[i].get();
    },&carry_hooks_);
    return actor;
}
std::size_t ActorWorld::add(const SoloActorConfig& config,SoloActorAnimation* animation,std::optional<ActorAnimationTuning> tuning,
    SoloActorControlFields initial,const SoloActorController& controller,bool present){
    if(!owner_dt_)owner_dt_=config.dt;
    const auto index=actors_.size();
    if(index>=0x100000)throw std::runtime_error("Actor world: too many actors for the handle ranges");
    actors_.push_back(make(index,config,animation,std::move(tuning),initial,controller,present));
    removal_requested_.push_back(false);removed_.push_back(false);destroyed_.push_back(false);serials_.push_back(1);
    if(!controller.ai&&!first_player_)first_player_=index;   // 003fb8d0 slot 0
    return index;
}
void ActorWorld::rebuild_ai(std::size_t index,const SoloActorConfig& config,SoloActorAnimation* animation,ActorAnimationTuning tuning,
    const SoloActorController& controller,bool present,SoloActorControlFields initial){
    if(index>=actors_.size())throw std::runtime_error("Actor world: rebuild of an unknown actor");
    if(!controller.ai||!animation)throw std::runtime_error("Actor world: an AI actor needs its AI controller and animation model");
    if(actors_[index]->player_controller())throw std::runtime_error("Actor world: the player's rebuild is not hosted");
    if(removal_requested_[index]||destroyed_[index])throw std::runtime_error("Actor world: rebuild of a removed or deleted actor");
    auto rebuilt=make(index,config,animation,std::move(tuning),initial,controller,present);
    if(stepping_){
        // 132ws9: 001b8130 from a script native the pump runs (the revive's NotifyCheckpointMsg). The original
        // rebuilds the record at the same address and the traversal goes on to it; here the new actor takes the
        // old one's traversal slot and the old object is kept until the step ends (its phase may be on the stack).
        if(!step_view_.list)throw std::runtime_error("Actor world: a rebuild during a step outside the traversal");
        const auto& live=*step_view_.live;
        const auto at=std::find(live.begin(),live.end(),index);
        if(at==live.end())throw std::runtime_error("Actor world: an actor appended in this step rebuilt in it (not hosted)");
        const auto k=static_cast<std::size_t>(at-live.begin());
        SoloActorSimulation::replace_in_step(*actors_[index],*rebuilt,(*step_view_.frames)[k],(*step_view_.inputs)[k]);
        (*step_view_.list)[k]=rebuilt.get();
        retired_.push_back(std::move(actors_[index]));
    }
    actors_[index]=std::move(rebuilt);++serials_[index];
}
std::size_t ActorWorld::add_player(const SoloActorConfig& config,SoloActorAnimation* animation,
    std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial,bool present){
    return add(config,animation,std::move(tuning),initial,SoloActorController{},present);
}
std::size_t ActorWorld::add_ai(const SoloActorConfig& config,SoloActorAnimation* animation,ActorAnimationTuning tuning,
    const SoloActorController& controller,bool present,SoloActorControlFields initial){
    if(!controller.ai)throw std::runtime_error("Actor world: an AI actor needs its AI controller");
    if(!animation)throw std::runtime_error("Actor world: an AI actor needs an animation model");
    return add(config,animation,std::move(tuning),initial,controller,present);
}
void ActorWorld::set_carry_hooks(SoloActorSimulation::CarryHooks hooks){carry_hooks_=std::move(hooks);}
std::size_t ActorWorld::size() const{return actors_.size();}
SoloActorSimulation& ActorWorld::actor(std::size_t index){return *actors_.at(index);}
const SoloActorSimulation& ActorWorld::actor(std::size_t index) const{return *actors_.at(index);}
const SoloCollisionWorld& ActorWorld::collision() const{return *world_;}
void ActorWorld::set_collision_owner(std::uint64_t handle,const CollisionMatrix& matrix){
    const auto prior=world_->owner(handle);
    world_->set_owner(handle,matrix);
    if(!owner_dt_||!prior){world_->set_owner_velocity(handle,std::nullopt);return;}
    RootVector velocity{};
    for(std::size_t i=0;i<3;++i)velocity[i]=(matrix[12+i]-(*prior)[12+i])/ *owner_dt_;
    velocity[3]=0.0F;
    if(velocity[0]==0.0F&&velocity[1]==0.0F&&velocity[2]==0.0F)world_->set_owner_velocity(handle,std::nullopt);
    else world_->set_owner_velocity(handle,velocity);
}
std::vector<SoloActorFrame> ActorWorld::step(std::span<const SoloActorFrameInput> inputs){
    if(inputs.size()!=actors_.size())throw std::runtime_error("Actor world: one input per actor");
    stepping_=true;
    struct StepEnd{bool& b;~StepEnd(){b=false;}} step_end{stepping_};
    const auto initial=actors_.size();
    // The flush of the removals requested since the last step (0040c5c0 -> 0040b4b0).
    for(std::size_t i=0;i<actors_.size();++i)if(removal_requested_[i])removed_[i]=true;
    // A deleted actor (destroy) is not visited at all: its frame stays empty.
    std::vector<SoloActorSimulation*> list;list.reserve(actors_.size());
    std::vector<SoloActorFrameInput> live_inputs;live_inputs.reserve(actors_.size());
    std::vector<std::size_t> live;live.reserve(actors_.size());
    for(std::size_t i=0;i<actors_.size();++i){
        if(destroyed_[i])continue;
        list.push_back(actors_[i].get());live_inputs.push_back(inputs[i]);live.push_back(i);
    }
    const std::unique_ptr<bool[]> traverse(new bool[live.size()+1]);
    for(std::size_t k=0;k<live.size();++k)traverse[k]=!removed_[live[k]];
    std::vector<SoloActorFrame> live_frames(live.size());
    step_view_={&list,&live,&live_frames,&live_inputs};
    struct ViewEnd{ActorWorld& w;~ViewEnd(){w.step_view_={};w.retired_.clear();}} view_end{*this};
    std::function<void()> services;
    if(services_)services=[this]{services_(frame_counter_);};
    std::function<void()> later;
    if(after_first_traversal_)later=[this]{after_first_traversal_(frame_counter_);};
    // Actors added during the step (a generator's spawn): stable frames, the
    // default input (an AI actor reads no pad), visited in registration order.
    std::deque<SoloActorFrame> late_frames;const SoloActorFrameInput late_input{};
    std::size_t next=actors_.size();
    const std::function<std::optional<SoloActorSimulation::AppendedActor>()> appended=
        [&]()->std::optional<SoloActorSimulation::AppendedActor>{
            if(next>=actors_.size())return std::nullopt;
            late_frames.emplace_back();
            return SoloActorSimulation::AppendedActor{actors_[next++].get(),&late_frames.back(),&late_input};
        };
    SoloActorSimulation::run_frame(list,live_inputs,live_frames,services,later,appended,std::span<const bool>(traverse.get(),live.size()));
    world_->clear_owner_velocities();
    ++frame_counter_;
    std::vector<SoloActorFrame> frames(initial);
    for(std::size_t k=0;k<live.size();++k)frames[live[k]]=std::move(live_frames[k]);
    for(auto& f:late_frames)frames.push_back(std::move(f));
    return frames;
}
void ActorWorld::remove(std::size_t index){
    if(index>=actors_.size())throw std::runtime_error("Actor world: removal of an unknown actor");
    removal_requested_[index]=true;
}
bool ActorWorld::removed(std::size_t index) const{
    if(index>=actors_.size())throw std::runtime_error("Actor world: removal query of an unknown actor");
    return removal_requested_[index];
}
bool ActorWorld::removal_pending(std::size_t index) const{return removed(index)&&!removed_[index];}
void ActorWorld::destroy(std::size_t index){
    if(index>=actors_.size())throw std::runtime_error("Actor world: deletion of an unknown actor");
    if(destroyed_[index])throw std::runtime_error("Actor world: an actor deleted twice");
    if(!removal_requested_[index])throw std::runtime_error("Actor world: 0040bf50 deletes only an actor whose removal was requested");
    if(actors_[index]->player_controller())throw std::runtime_error("Actor world: the player's deletion is not hosted");
    destroyed_[index]=true;
}
bool ActorWorld::destroyed(std::size_t index) const{
    if(index>=actors_.size())throw std::runtime_error("Actor world: deletion query of an unknown actor");
    return destroyed_[index];
}
bool ActorWorld::traversed(std::size_t index) const{
    if(index>=actors_.size())throw std::runtime_error("Actor world: traversal query of an unknown actor");
    return !removed_[index];
}
void ActorWorld::set_frame_services(std::function<void(std::uint32_t frame)> services){services_=std::move(services);}
void ActorWorld::set_after_first_traversal(std::function<void(std::uint32_t frame)> entries){after_first_traversal_=std::move(entries);}
void ActorWorld::set_step_dt(float dt){for(auto& a:actors_)a->set_step_dt(dt);step_dt_=dt;owner_dt_=dt;}
void ActorWorld::set_movement_list(std::function<std::vector<std::size_t>()> list){movement_list_=std::move(list);}
void ActorWorld::set_agent_slot(int slot,std::optional<std::size_t> actor){
    if(slot<1||slot>3)throw std::runtime_error("Actor world: agent slots are 1..3 (slot 0 is the first player)");
    if(actor&&*actor>=actors_.size())throw std::runtime_error("Actor world: an agent slot names an unknown actor");
    if(actor&&first_player_==actor)throw std::runtime_error("Actor world: the first player is slot 0");
    agent_slots_[static_cast<std::size_t>(slot-1)]=actor;
}
bool ActorWorld::in_agent_slot(std::size_t actor) const{
    for(const auto& s:agent_slots_)if(s==actor)return true;
    return false;
}
void ActorWorld::set_movement_contact(std::function<void(std::size_t,std::size_t)> contact){movement_contact_=std::move(contact);}
void ActorWorld::activate(std::size_t index){actor(index).activate();}
void ActorWorld::deactivate(std::size_t index,bool effect_slots){actor(index).deactivate(effect_slots);}
std::uint32_t ActorWorld::frame_counter() const{return frame_counter_;}
std::uint64_t ActorWorld::random_state() const{return random_;}
std::uint64_t& ActorWorld::random(){return random_;}
}
