#include "float_bits.hpp"
#include "actor_heading_state.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
void value(float v) {
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Heading stack requires finite normal-or-zero values");
}
}
bool apply_actor_heading_state_command(ActorHeadingState& state,const ActorHeadingCommand& command) {
    using Kind=ActorHeadingCommand::Kind;
    if(command.kind!=Kind::history&&command.kind!=Kind::target)return false;
    if(state.history.empty()||state.history.size()>4096)
        throw std::runtime_error("Heading stack outside bounded extent");
    for(float angle:state.history)value(angle);
    if(command.kind==Kind::history) {
        if(command.index>=state.history.size())throw std::runtime_error("Heading stack index outside array");
        value(command.value);state.history[command.index]=command.value;
    } else {
        value(command.duration);
        target_animation_rotation_curve(state.curve,state.history.back(),command.duration);
    }
    return true;
}
void update_actor_aim_heading(ActorHeadingState& state,std::int32_t& entry_20,std::int32_t previous_30,
    std::int32_t current_38,bool byte,float yaw){
    if(state.active.size()!=state.history.size()||state.history.size()>4096)
        throw std::runtime_error("Heading stack (00247410): entry bytes and angles disagree or exceed the bounded extent");
    value(yaw);
    const auto in58=[](std::int32_t x){return x==5||x==8;};
    auto next=state;
    std::int32_t entry=entry_20;
    const auto target=[&](float duration){   // 00186b50
        if(next.history.empty())throw std::runtime_error("Heading stack (00186b50): the curve target reads an empty stack");
        target_animation_rotation_curve(next.curve,next.history.back(),duration);
    };
    const auto index=[&]{
        if(entry<0||static_cast<std::size_t>(entry)>=next.history.size())
            throw std::runtime_error("Heading stack (00247410): input +20 outside the stack");
        return static_cast<std::size_t>(entry);
    };
    if(!in58(previous_30)&&in58(current_38)){
        if(!byte||entry!=-1)return;
        const float duration=current_38==8?std::bit_cast<float>(0x3ea8f5c3U):1.0F;
        next.history.push_back(yaw);next.active.push_back(1);              // 002477e0 / 002477c0
        target(duration);
        entry=static_cast<std::int32_t>(next.history.size())-1;            // 002477b0
    }else if(in58(previous_30)&&!in58(current_38)){
        if(entry==-1)return;
        next.active[index()]=0;                                            // 00247720
        std::size_t size=next.active.size();
        while(size>0&&next.active[size-1]==0)--size;
        next.history.resize(size);next.active.resize(size);                // 00250e70
        target(-1.0F);
        entry=-1;
    }else{
        if(!in58(current_38)||!byte||entry==-1)return;
        next.history[index()]=yaw;                                         // 00186be0
        target(-1.0F);
    }
    state=std::move(next);
    entry_20=entry;
}
}
