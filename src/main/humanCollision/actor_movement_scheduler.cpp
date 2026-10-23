#include "actor_movement_scheduler.hpp"
#include <stdexcept>

namespace omega {
namespace {
bool truthy(const std::optional<bool>& value){return value.value_or(false);}
}

bool adjust_actor_movement_pair_live(ActorMovementWorldHost& host,std::uint64_t self,
    const Bounds& self_world_bounds,std::uint64_t target,float factor,RootVector& endpoint){
    ActorMovementPairInput input;
    input.self_controller_54=truthy(host.controller_54(self));
    if(!input.self_controller_54){
        input.other_controller_54=truthy(host.controller_54(target));
        if(!input.other_controller_54)return false;
    }
    input.other_flag_2d=host.byte_2d(target);
    if(input.other_flag_2d)input.other_flag_2c=host.byte_2c(target);
    if(!input.other_flag_2d||!input.other_flag_2c)return false;
    input.other_flag_2e=host.byte_2e(target);
    if(input.other_flag_2e)return false;
    input.other_mode_108=host.mode_108(target);
    if(input.other_mode_108==2||input.other_mode_108==3)return false;
    input.physical_enabled_3d0=host.physical_enabled(self);
    if(!input.physical_enabled_3d0)return false;
    input.same_actor=target==self;
    if(input.same_actor)return false;
    input.excluded_by_relation=host.related(self,target);
    if(input.excluded_by_relation)return false;
    input.other_position=host.position_e0(target);
    // Bounds and the dirty refresh run only for targets inside the radius.
    if(!actor_movement_pair_in_range(endpoint,input.other_position))return false;
    input.other_local_bounds=host.local_bounds(target);
    input.other_world_transform=host.refreshed_world_transform(target);
    input.self_world_bounds=self_world_bounds;
    input.factor=factor;
    const auto result=adjust_actor_movement_pair(endpoint,input);
    endpoint=result.endpoint;
    return result.adjusted;
}

ActorMovementScheduleResult schedule_actor_movement_adjustment(ActorMovementWorldHost& host,
    std::uint64_t self,float elapsed,const RootVector& original){
    RootVector endpoint=original;
    bool any=false;
    const bool self_flag=host.class_flag(self); // captured before virtual calls
    const auto factors=actor_movement_factors(elapsed);
    const auto finish=[&]{return ActorMovementScheduleResult{actor_movement_adjustment_delta(original,endpoint),any};};
    if(host.mode_108(self)==3)return finish();
    const auto second=host.mode_108(self);
    if((second==2||second==3)&&!truthy(host.controller_54(self)))return finish();
    if(host.physical_mode(self)==9||!host.physical_enabled(self))return finish();
    const Bounds local=host.local_bounds(self);
    const Bounds self_world=actor_movement_world_bounds(local,host.refreshed_world_transform(self));

    const std::uint64_t end=host.list_end();
    for(std::uint64_t node=host.list_begin();node!=end;node=host.list_next(node)){
        if(!host.list_element(node))continue;
        const auto element=host.list_element(node); // original revalidates
        if(!element)throw std::runtime_error(
            "Movement list element invalidated between checks (original null dereference)");
        const auto target=host.element_owner(*element);
        if(!target||host.byte_49(*target)!=2)continue;
        const bool target_flag=host.class_flag(*target);
        const bool controlled=truthy(host.controller_54(self));
        const float factor=controlled&&target_flag?factors.accelerated:factors.ordinary;
        const bool adjusted=adjust_actor_movement_pair_live(host,self,self_world,*target,factor,endpoint);
        any=any||adjusted;
        if(adjusted&&host.byte_14(self)!=0){
            if(truthy(host.controller_58(*target))||truthy(host.controller_5c(*target)))
                host.send_interaction(self,*target,7);
        }
    }
    for(int slot=0;slot<4;++slot){
        const auto target=host.player_slot(slot);
        if(!target||host.byte_49(*target)!=2)continue;
        bool weighted=true;
        if(host.secondary_root_weight(*target)<=0.0F&&host.tertiary_root_weight(*target)<=0.0F)weighted=false;
        const bool controlled=truthy(host.controller_54(self));
        const float factor=!controlled&&self_flag&&!weighted?0.0F:factors.ordinary;
        const bool adjusted=adjust_actor_movement_pair_live(host,self,self_world,*target,factor,endpoint);
        any=any||adjusted;
    }
    return finish();
}
}
