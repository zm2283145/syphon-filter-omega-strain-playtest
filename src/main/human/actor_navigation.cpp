#include "float_bits.hpp"
#include "actor_navigation.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace omega {
namespace {
float checked(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Actor navigation value outside finite normal-or-zero domain");
    return value;
}
float add(float a,float b){return checked(a+b);}
float sub(float a,float b){return checked(a-b);}
float mul(float a,float b){return checked(a*b);}
float divide(float a,float b){return checked(a/b);}
bool bit(std::uint32_t value,unsigned index){return (value&(std::uint32_t{1}<<index))!=0;}
void vector(const RootVector& value){for(float v:value)checked(v);}
float dot(const RootVector& a,const RootVector& b){return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}
RootVector normalized(RootVector value) {
    const auto squared=dot(value,value);
    if(squared==0)throw std::runtime_error("Actor navigation cannot normalize a zero direction");
    // VU RSQRT Q = vf0.w/sqrt(|v|^2), then vmulq.xyz (001a82b4, 001a849c,
    // 001a8cec). The planar distance path below is VU SQRT then VU DIV.
    const auto inverse=checked(vu::rsqrt(squared));
    for(unsigned i=0;i<3;++i)value[i]=mul(value[i],inverse);
    return value;
}
RootVector rotated(const RootVector& q,const RootVector& value,bool inverse=false) {
    vector(q);vector(value);
    auto rotation=q;
    if(inverse)for(unsigned i=0;i<3;++i)rotation[i]=sub(0,rotation[i]);
    const RootVector conjugate{sub(0,rotation[0]),sub(0,rotation[1]),sub(0,rotation[2]),rotation[3]};
    RootVector first{},result{};
    for(unsigned i=0;i<3;++i) {
        const auto j=(i+1)%3,k=(i+2)%3;
        first[i]=sub(add(mul(rotation[j],value[k]),mul(value[i],rotation[3])),mul(value[j],rotation[k]));
    }
    first[3]=sub(sub(sub(mul(rotation[3],0),mul(rotation[2],value[2])),mul(rotation[1],value[1])),mul(rotation[0],value[0]));
    for(unsigned i=0;i<3;++i) {
        const auto j=(i+1)%3,k=(i+2)%3;
        result[i]=sub(add(add(mul(first[j],conjugate[k]),mul(first[i],conjugate[3])),mul(conjugate[i],first[3])),mul(conjugate[j],first[k]));
    }
    result[3]=value[3];return result;
}
void position(const RootVector& value) {
    vector(value);
    if(value[3]!=1)throw std::runtime_error("Actor navigation requires affine world positions");
}
}
ActorNavigationTuning read_actor_navigation_tuning(std::istream& input) {
    std::string text;char byte;
    while(input.get(byte)) {
        if(text.size()>=512)throw std::runtime_error("Navigation tuning exceeds bounded extent");
        text.push_back(byte);
    }
    if(!input.eof())throw std::runtime_error("Navigation tuning read failed");
    std::istringstream stream(text);std::string magic,version;ActorNavigationTuning result;
    if(!(stream>>magic>>version>>result.ladder_offset>>result.ladder_stop_distance>>result.distance_scale)||
        magic!="OMEGA_NAVIGATION_TUNING"||version!="1")throw std::runtime_error("Invalid navigation tuning header");
    checked(result.ladder_offset);
    if(checked(result.ladder_stop_distance)<0||checked(result.distance_scale)<=0)
        throw std::runtime_error("Invalid navigation distance tuning");
    stream>>std::ws;
    if(!stream.eof())throw std::runtime_error("Trailing navigation tuning data");
    return result;
}
float actor_navigation_playback_rate(float metric,float channel2_current) {
    checked(metric);checked(channel2_current);
    auto power=mul(metric,metric);power=mul(power,power);power=mul(power,power);
    return add(1,mul(channel2_current,mul(1.25F,power)));
}
ActorNavigationPlan prepare_actor_navigation(const ActorNavigationInput& in,const ActorNavigationTuning& tuning) {
    ActorNavigationPlan out;
    if(!in.physical_present||!in.target_enabled)return out;
    if(in.mode!=0&&in.mode!=2&&in.mode!=7&&in.mode!=8&&in.mode!=16&&in.mode!=21&&in.mode!=28)return out;
    // The target lane W is not checked: 001a80e0 reads the vector with lq and
    // uses its x/y/z; the AI producer 0014bff0 sends its facing vector (W 0)
    // as the target while stopped (movement state 0).
    vector(in.target_position);out.updated=true;
    if(in.mode==16) {
        checked(tuning.ladder_offset);
        if(checked(tuning.ladder_stop_distance)<0)throw std::runtime_error("Negative ladder stop distance");
        const auto& a=in.first_ladder_anchor;const auto& b=in.second_ladder_anchor;
        if(checked(a.weight)<0||checked(b.weight)<0)throw std::runtime_error("Negative ladder anchor weight");
        const auto weight=add(a.weight,b.weight);float height;
        if(weight>0) {
            position(a.position);position(b.position);
            const auto ratio=divide(b.weight,weight);
            height=sub(sub(add(a.position[2],mul(b.position[2],ratio)),mul(a.position[2],ratio)),tuning.ladder_offset);
        } else {position(in.fallback_ladder_position);height=in.fallback_ladder_position[2];}
        const auto delta=sub(in.target_position[2],height);
        if(std::abs(delta)>tuning.ladder_stop_distance) {
            out.direction=normalized({0,0,delta,0});out.speed=1;
        } else out.arrived=bit(in.flags3210,13);
        if(delta!=0) {
            const auto direction=normalized({0,0,delta,0});
            out.ladder_action_candidate=static_cast<std::uint8_t>(direction[2]<0?31:32);
            if(!in.component_predicate)out.immediate_ladder_action=out.ladder_action_candidate;
        }
        return out;
    }
    position(in.world.position);vector(in.world.orientation);
    RootVector delta;
    for(unsigned i=0;i<4;++i)delta[i]=sub(in.target_position[i],in.world.position[i]);
    auto planar=delta;planar[2]=0;
    const auto squared=dot(planar,planar);
    if(in.mode==7||in.mode==8) {
        if(squared>=1) {out.direction=rotated(in.world.orientation,planar,true);out.speed=1;}
        else out.arrived=true;
        return out;
    }
    checked(in.target_yaw);
    if(checked(in.speed_limit)<0||checked(tuning.distance_scale)<=0)throw std::runtime_error("Invalid actor navigation speed tuning");
    const RootVector forward{-checked(std::sin(in.target_yaw)),checked(std::cos(in.target_yaw)),0,0};
    float distance=0;
    if(squared!=0) {
        // 001a8864 VU SQRT, 001a8870 VU DIV 1/distance, then vmulq.xyz.
        distance=checked(vu::sqrt(squared));
        const auto inverse=checked(vu::div(1,distance));
        for(unsigned i=0;i<3;++i)planar[i]=mul(planar[i],inverse);
    }
    const bool near=distance<1,restricted=bit(in.flags3204,4)||in.mode==21;
    out.arrived=near&&std::abs(delta[2])<3;
    RootVector facing;bool equipment_facing=false;
    if(restricted&&!near&&in.speed_limit>0) {
        facing=rotated(in.world.orientation,planar,true);
        if(dot(planar,forward)<0)for(auto& v:facing)v=sub(0,v);
    } else {
        const bool use_equipment=(in.equipment_present&&in.primary_classified&&in.secondary_classified&&in.equipment_state!=16)||
            (in.body_class!=6&&in.body_enabled);
        if(use_equipment) {
            if(!in.equipment_present)throw std::runtime_error("Actor navigation equipment branch has no component");
            facing=rotated(in.equipment_rotation,{0,1,0,0});equipment_facing=true;
        } else facing=rotated(in.world.orientation,forward,true);
    }
    if(restricted) {
        const auto local=rotated(in.world.orientation,planar,true);
        out.direction={0,local[1]>0?1.0F:-1.0F,0,0};
    } else if(near||in.speed_limit==0)out.direction=facing;
    else {
        const auto along=mul(0.1F,dot(planar,forward));
        auto adjusted=planar;
        for(unsigned i=0;i<4;++i)adjusted[i]=sub(adjusted[i],mul(forward[i],along));
        out.direction=rotated(in.world.orientation,normalized(adjusted),true);
    }
    auto angle=checked(std::atan2(-facing[0],facing[1]));
    if(angle>3.1415927F)angle=sub(angle,6.2831855F);
    else if(angle<-3.1415927F)angle=add(angle,6.2831855F);
    out.speed=std::min(mul(distance,divide(1,tuning.distance_scale)),in.speed_limit);
    if(std::abs(angle)>1.0471976F)out.speed=std::min(0.001F,out.speed);
    else if(std::abs(angle)>0.43633232F)out.speed=std::min(0.5F,out.speed);
    if(near)out.speed=0;
    out.turn=std::clamp(mul(angle,in.mode==28?8.0F:(equipment_facing?3.0F:2.0F)),-4.1887903F,4.1887903F);
    if(in.mode!=28&&out.speed==0) {
        const auto magnitude=std::abs(angle);
        if(magnitude<0.08726646F)out.turn=0;
        else if(magnitude<1.5707964F)out.turn=mul(out.turn,mul(magnitude,divide(1,1.5707964F)));
    }
    out.turn_pose=in.stationary_latch&&out.speed==0&&std::abs(out.turn)>0.0001F;
    if(in.retime_target)out.retime_target=in.target_position;
    return out;
}
float actor_navigation_group_metric(const AnimationController& controller,
    const AnimationPlayerState& player,const AnimationGraph& graph,const MotionGroupSelector& selector) {
    if(controller.active.size()>256||player.groups.size()>4096)
        throw std::runtime_error("Actor navigation metric exceeds bounded container extent");
    float result=0;
    for(const auto& record:controller.active) {
        for(const auto& group:player.groups) {
            if(group.host!=record.host)continue;
            bool match;
            if(group.members.size()==1) {
                const auto node=group.members.front().node;
                if(node>=graph.nodes().size())throw std::runtime_error("Invalid navigation group member");
                const auto& leaf=graph.nodes()[node];
                if(leaf.kind!=AnimationGraphKind::Leaf&&leaf.kind!=AnimationGraphKind::ControlledLeaf)
                    throw std::runtime_error("Navigation metric group member is not a leaf");
                match=leaf.leaf.group==-1?selector.node==node&&selector.group==-1:
                    !selector.node&&selector.group==leaf.leaf.group;
            } else match=!selector.node&&selector.group==group.number;
            if(match) {
                if(checked(group.totals.total_weight)<0)throw std::runtime_error("Negative navigation group weight");
                result=add(result,group.totals.total_weight);break;
            }
        }
    }
    return result;
}
void update_actor_navigation(const ActorNavigationHost& host,const ActorNavigationTuning& tuning) {
    if(!host.snapshot||!host.apply)throw std::runtime_error("Actor navigation host is incomplete");
    const auto plan=prepare_actor_navigation(host.snapshot(),tuning);
    if(plan.updated) {
        auto patch=plan;patch.immediate_ladder_action.reset();patch.ladder_action_candidate.reset();patch.retime_target.reset();
        host.apply(patch);
        if(plan.ladder_action_candidate&&!host.snapshot().component_predicate) {
            if(!host.deliver_ladder_immediate||!host.set_arrived)throw std::runtime_error("Ladder navigation delivery is unresolved");
            host.deliver_ladder_immediate(*plan.ladder_action_candidate);
            if(!bit(host.snapshot().flags3210,14))host.set_arrived(true);
        }
        if(plan.retime_target) {
            if(!host.retime_target)throw std::runtime_error("Navigation vector-curve host is unresolved");
            host.retime_target(plan.target_duration,*plan.retime_target);
        }
    }
    const auto tail=host.snapshot();
    if(tail.component_predicate&&!tail.actor_flag14) {
        if(!host.group_metric||!host.channel2_current||!host.set_playback_rate)
            throw std::runtime_error("Navigation model-rate host is unresolved");
        const auto metric=host.group_metric();
        const auto current=host.channel2_current();
        host.set_playback_rate(actor_navigation_playback_rate(metric,current));
    }
}
}
