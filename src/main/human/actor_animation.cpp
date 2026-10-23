#include "float_bits.hpp"
#include "actor_animation.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace {
float number(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Actor animation value outside finite normal-or-zero domain");
    return value;
}
float product(float a,float b) {return number(a*b);}
float plus(float a,float b) {return number(a+b);}
float difference(float a,float b) {return number(a-b);}
bool bit(std::uint32_t value,unsigned index) {return (value&(std::uint32_t(1)<<index))!=0;}
}
ActorAnimationTuning read_actor_animation_tuning(std::istream& input) {
    std::string source;char byte;
    while(input.get(byte)) {
        if(source.size()==4096)throw std::runtime_error("Actor animation tuning input exceeds bounded extent");
        source.push_back(byte);
    }
    if(!input.eof())throw std::runtime_error("Actor animation tuning input failed");
    std::istringstream stream(source);std::string magic,version;
    ActorAnimationTuning result;
    if(!(stream>>magic>>version)||magic!="OMEGA_ACTOR_TUNING"||version!="1"||!(stream>>result.accumulation_duration))
        throw std::runtime_error("Actor animation tuning header is invalid");
    if(number(result.accumulation_duration)<=0)throw std::runtime_error("Actor animation tuning duration is invalid");
    for(auto& value:result.channel_timing)
        if(!(stream>>value)||number(value)<0)throw std::runtime_error("Actor animation tuning table is invalid");
    stream>>std::ws;
    if(!stream.eof())throw std::runtime_error("Actor animation tuning has trailing data");
    return result;
}
ActorDirectionPlan prepare_actor_direction_channels(const ActorDirectionInput& in) {
    if(number(in.elapsed)<=0) throw std::runtime_error("Actor animation elapsed time must be positive");
    number(in.desired_x);number(in.desired_y);number(in.speed);
    for(float value:in.current_channels)number(value);
    ActorDirectionPlan out;
    out.approach_step=product(in.elapsed,1.875F);
    const auto equipment_step=product(in.elapsed,5.625F);
    const auto direction_step=product(in.elapsed,0.28125F);
    const auto approach=[&](std::size_t index,float target,float step) {
        // Validate the immediate arithmetic before returning a complete plan.
        const auto delta=difference(target,in.current_channels[index]);
        number(in.current_channels[index]+std::clamp(delta,-step,step));
        out.commands.push_back({ActorChannelOperation::approach,index,target,step});
    };
    const auto set=[&](std::size_t index,float value) {
        out.commands.push_back({ActorChannelOperation::set,index,value});
    };
    approach(12,in.has_equipment&&in.weapon_class!=6?1.0F:0.0F,equipment_step);
    approach(13,in.weapon_class==6?1.0F:0.0F,equipment_step);
    const bool gated=(in.physical_present&&in.contact_dc&&!bit(in.flags3034,0))||
        bit(in.flags3210,4)||bit(in.flags3210,5)||bit(in.flags3210,10)||
        bit(in.flags3210,11)||bit(in.flags3248,15);
    if(gated) {
        set(15,in.current_channels[15]);set(16,in.current_channels[16]);
        return out;
    }
    const auto angle=number(std::atan2(in.desired_x,in.desired_y));
    float direction=std::clamp(product(plus(angle,3.1415927F),0.15915494F),0.0F,1.0F);
    float opposite=direction>0.5F?difference(direction,0.5F):plus(direction,0.5F);
    const auto forward=in.current_channels[5], sideways=in.current_channels[6];
    if(direction<0.1F&&forward<0.5F&&in.facing_positive)direction=1;
    else if(direction>0.9F&&forward<0.5F&&!in.facing_positive)direction=0;
    if(opposite<0.1F&&forward>0.5F&&sideways<0.5F)opposite=1;
    else if(opposite>0.9F&&forward>0.5F&&sideways>0.5F)opposite=0;
    if(!bit(in.flags3210,2)) {
        if(in.speed==0) {direction=0.5F;opposite=0;}
        set(15,direction);set(16,opposite);
    } else if(in.speed>0) {
        approach(15,direction,direction_step);approach(16,opposite,direction_step);
    } else {
        set(15,in.current_channels[15]);set(16,in.current_channels[16]);
    }
    return out;
}

namespace {
enum class OrdinaryPart { whole, prefix, moving_tail };
ActorOrdinaryPlan ordinary_part(const ActorOrdinaryInput& in,
    const ActorAnimationTuning& tuning,OrdinaryPart part,std::optional<bool> retained_gate={},
    std::optional<float> stored_step={}) {
    const auto& d=in.direction;
    if(part!=OrdinaryPart::moving_tail&&((in.mode!=0&&in.mode!=2&&!bit(d.flags3210,27)&&!bit(d.flags3210,5))||bit(in.flags324c,3)||bit(in.flags324c,6)))
        throw std::runtime_error("Actor animation special mode is outside ordinary producer");
    if(number(tuning.accumulation_duration)<=0)throw std::runtime_error("Actor animation tuning is not initialized");
    for(float value:tuning.channel_timing)if(number(value)<0)throw std::runtime_error("Actor animation timing is outside nonnegative domain");
    for(float value:in.target_channels)number(value);
    for(float value:in.prior_direction)number(value);
    for(float value:{in.desired_z,in.desired_w,in.turn,in.accumulation,in.physical_2c8})number(value);
    if(in.accumulation<0||in.accumulation>1)throw std::runtime_error("Actor animation accumulation outside unit interval");
    if(in.physical_height)number(*in.physical_height);
    if(number(d.elapsed)<=0)throw std::runtime_error("Actor animation elapsed time must be positive");
    number(d.desired_x);number(d.desired_y);number(d.speed);
    for(float value:d.current_channels)number(value);
    ActorOrdinaryPlan out;
    if(part==OrdinaryPart::whole) {
        auto early=prepare_actor_direction_channels(d);
        out.commands=std::move(early.commands);out.approach_step=early.approach_step;
    } else {
        out.approach_step=number(stored_step.value());
        if(out.approach_step<0)throw std::runtime_error("Actor approach step must be nonnegative");
    }
    out.facing_positive=d.facing_positive;out.prior_direction=in.prior_direction;
    out.desired_direction={d.desired_x,d.desired_y,in.desired_z,in.desired_w};
    auto current=d.current_channels;auto targets=in.target_channels;
    const auto clamp=[](float v){return std::clamp(number(v),0.0F,1.0F);};
    const auto timing=[&](unsigned suffix){return tuning.channel_timing.at((suffix-0x48)/8);};
    const auto target=[&](std::size_t index,float value,float duration,float damping=3.0F) {
        number(value);number(duration);number(damping);targets[index]=value;
        out.commands.push_back({ActorChannelOperation::target,index,value,duration,damping});
    };
    const auto approach=[&](std::size_t index,float value,float step) {
        current[index]=plus(current[index],std::clamp(difference(value,current[index]),-step,step));
        targets[index]=current[index];
        out.commands.push_back({ActorChannelOperation::approach,index,value,step});
    };
    for(const auto& command:out.commands) {
        auto& value=current[command.index];
        value=command.operation==ActorChannelOperation::set?command.value:
            plus(value,std::clamp(difference(command.value,value),-command.parameter,command.parameter));
        targets[command.index]=value;
    }
    const bool gated=retained_gate.value_or((d.physical_present&&d.contact_dc&&!bit(d.flags3034,0))||
        bit(d.flags3210,4)||bit(d.flags3210,5)||bit(d.flags3210,10)||
        bit(d.flags3210,11)||bit(d.flags3248,15));
    const bool stance3=bit(in.flags3204,3),stance4=bit(in.flags3204,4);
    const auto crossing=[](float old,float value){return (old<0.5F&&value>0.5F)||(old>0.5F&&value<0.5F);};
    const auto retarget_axes=[&]() {
        for(unsigned axis=0;axis<2;++axis) {
            const auto desired=out.desired_direction[1-axis],previous=out.prior_direction[1-axis];
            float value;
            if((desired<=0&&desired<previous)||(desired<0&&desired==previous))value=0;
            else if((desired>=0&&desired>previous)||(desired>0&&desired==previous))value=1;
            else if(desired<previous)value=plus(product(std::min(1.0F,difference(previous,desired)),-1.0F),1.0F);
            else if(desired>previous)value=plus(product(std::min(1.0F,difference(desired,previous)),1.0F),0.0F);
            else value=0.5F;
            target(axis,value,timing(crossing(current[5+axis],value)?0xe8:0xe0));
        }
    };
    const auto smooth_direction=[&](float amount) {
        for(unsigned c=0;c<4;++c) {
            // Original VU: old + desired*t, then subtract old*t.
            const auto old=out.prior_direction[c];
            out.prior_direction[c]=difference(plus(old,product(out.desired_direction[c],amount)),product(old,amount));
        }
        for(unsigned c=0;c<2;++c)
            if(std::abs(difference(out.prior_direction[c],out.desired_direction[c]))<0.001F)
                out.prior_direction[c]=out.desired_direction[c];
        if(out.desired_direction[0]!=0&&out.desired_direction[1]==0&&out.prior_direction[1]<0)
            out.prior_direction[1]=0;
    };
    const auto moving_tail=[&]() {
        retarget_axes();
        const auto factor=product(difference(1.0F,product(0.95F,gated?1.0F:current[4])),30.0F);
        smooth_direction(std::min(1.0F,product(d.elapsed,factor)));
        target(14,0.5F,timing(0xb8),6.5F);approach(17,0.5F,out.approach_step);
        if(!stance3&&!stance4) {
            if(current[2]>0.8F)out.pending_set_bits|=2;
            else if(current[2]>=0.5F)out.pending_set_bits|=4;
        }
        target(4,1,timing(0xa8));
    };
    if(part==OrdinaryPart::moving_tail) {
        moving_tail();
    } else {
    out.commands.push_back({ActorChannelOperation::select,3,0});
    if(d.speed==0) {
        if(!(in.flags3204&((1u<<5)|(1u<<6)|(1u<<7))))out.pending_set_bits|=1;
        const auto c4=current[4];
        if(!gated||bit(d.flags3248,15)) {
            auto duration=product(product(product(c4,c4),c4),c4);
            duration=product(plus(product(duration,1.5F),1.0F),0.16F);
            target(4,0,duration);
        }
        target(10,stance3?1.0F:0.0F,timing(0x90),5.5F);
        approach(11,stance4?1.0F:0.0F,out.approach_step);
        if(gated||current[4]!=0) {
            if(gated)for(unsigned index:{3u,5u,6u,7u,8u,9u})target(index,current[index],-1,0);
            else for(unsigned index:{7u,8u})target(index,current[index],-1,0);
        } else {
            target(5,0.5F,timing(0x68));target(6,0.5F,timing(0x58));
            const auto duration=product(current[4],timing(0xc0));
            target(7,0,duration);target(8,0,duration);target(9,targets[6],timing(0x58));
        }
        float turn_target=0.5F;
        if(in.turn_pose_enabled&&d.physical_present&&in.contact_dd&&!bit(d.flags3248,4))
            turn_target=clamp(difference(0.5F,product(in.turn,0.22207667F)));
        target(14,turn_target,timing(0xb8),6.5F);
        approach(17,bit(d.flags3248,30)?0.0F:(bit(d.flags3248,31)?1.0F:0.5F),out.approach_step);
        if(current[4]==0)target(2,0,timing(0x50));else target(2,current[2],-1,0);
        retarget_axes();smooth_direction(std::min(1.0F,product(d.elapsed,1.5F)));
    } else {
        if(in.turn_correction_enabled&&out.desired_direction[1]>0&&current[4]==1) {
            const auto angle=number(std::atan2(std::abs(out.desired_direction[0]),std::abs(out.desired_direction[1])));
            const auto attenuation=clamp(product(difference(1.2217305F,angle),0.8185111F));
            const auto rotation=product(product(in.turn,0.31666666F),attenuation);
            const auto sine=number(std::sin(rotation)),cosine=number(std::cos(rotation));
            const auto old=out.desired_direction;
            //00140e20 stores supplied rows into columns; the call supplies an
            // all-zero third row, and the VU evaluates XYZ without translation.
            out.desired_direction[0]=plus(plus(product(cosine,old[0]),product(-sine,old[1])),product(0,old[2]));
            out.desired_direction[1]=plus(plus(product(sine,old[0]),product(cosine,old[1])),product(0,old[2]));
            out.desired_direction[2]=plus(plus(product(0,old[0]),product(0,old[1])),product(0,old[2]));
            out.desired_direction[3]=out.desired_direction[2];
        }
        target(10,stance3?1.0F:0.0F,timing(stance3?0x98:0xa0),5.5F);
        approach(11,stance4?1.0F:0.0F,out.approach_step);
        const auto x=out.desired_direction[0],y=out.desired_direction[1];
        float value=y<0?plus(product(product(current[7],current[3]),0.5F),0.0F):(y>0?1.0F:(x==0?0.5F:1.0F));
        float duration=0;
        if(gated||current[4]!=0) {
            duration=timing(0x68);
            if(!gated&&crossing(current[5],value)&&!bit(d.flags3248,4)&&current[4]<1)
                duration=plus(timing(0x70),product(current[4],difference(timing(0x68),timing(0x70))));
        }
        target(5,value,duration);
        if(x<0) {value=0;out.facing_positive=false;}
        else if(x>0) {value=1;out.facing_positive=true;}
        else value=current[7]>0?(out.facing_positive?1.0F:0.0F):0.5F;
        duration=0;
        if(gated||current[4]!=0) {
            duration=timing(0x58);
            if(!gated&&crossing(current[6],value)&&!bit(d.flags3248,4)&&current[4]<1)
                duration=plus(timing(0x60),product(current[4],difference(timing(0x58),timing(0x60))));
        }
        target(6,value,duration);
        const auto angle=number(std::atan2(std::abs(x),std::abs(y)));
        const auto numerator=number(std::sin(angle));
        const auto denominator=number(std::sin(difference(2.3561945F,angle)));
        value=clamp(number(number(numerator/denominator)/1.4142135F));
        const auto forward=product(std::abs(difference(current[5],0.5F)),2.0F);
        const auto sideways=product(std::abs(difference(current[6],0.5F)),2.0F);
        duration=timing(current[3]<=value?0x78:0x80);
        duration=plus(timing(0x88),product(plus(forward,product(value,difference(sideways,forward))),difference(duration,timing(0x88))));
        if(!gated&&current[4]==0)duration=0;
        else if(!gated&&crossing(current[3],value)&&!bit(d.flags3248,4)&&current[4]<1)
            duration=plus(timing(0x88),product(current[4],difference(duration,timing(0x88))));
        target(3,value,duration);
        if(!(d.physical_present&&d.contact_dc)) {
            if(d.physical_present&&in.contact_dd) {
                if(in.accumulation>=1&&in.secondary_311&&*in.secondary_311==std::uint8_t{0x16}) {
                    target(7,0,timing(0xd0));target(8,0,timing(0xd0));
                } else if(in.accumulation<1||stance3||stance4) {
                    duration=product(gated?1.0F:current[4],timing(stance3?0xd8:0xc0));
                    target(7,0,duration);target(8,0,duration);
                } else if(y<0) {target(7,1,timing(0xc8));target(8,1,timing(0xc8));}
                else {target(7,0,timing(0xc0));target(8,current[3],timing(0xc0));}
            } else {target(7,0,timing(0xc0));target(8,0,timing(0xc0));}
        }
        if(current[7]<=0||y<=0)target(9,targets[6],timing(0x58));
        value=clamp(d.speed);
        const bool restricted=d.physical_present&&in.physical_2c8>=0.025F;
        const bool between=current[11]>0&&current[11]<1;
        const bool slow=restricted||between||d.speed<0.5F;
        if(between)value=0;
        else {
            if(restricted)value=product(difference(1.0F,in.physical_2c8),0.5F);
            if(d.physical_present&&in.physical_height) {
                auto limit=plus(product(number(0.5F/2.25F),difference(2.25F,*in.physical_height)),0.5F);
                value=std::min(value,clamp(limit));
            }
            if(bit(d.flags3248,28)||bit(d.flags3248,4))value=std::min(0.5F,value);
            value=product(value,std::min(1.0F,product(current[4],1.5F)));
        }
        target(2,value,timing(slow?0x50:0x48));
        if(part==OrdinaryPart::whole&&value>0.5F&&bit(in.flags3204,1))
            throw std::runtime_error("Actor running-world message producer requires explicit dispatch hosting");
        out.world_message_candidate=value>0.5F&&bit(in.flags3204,1);
        if(part==OrdinaryPart::prefix)return out;
        moving_tail();
    }
    }
    auto squared=product(out.desired_direction[0],out.desired_direction[0]);
    squared=plus(squared,product(out.desired_direction[1],out.desired_direction[1]));
    squared=plus(squared,product(out.desired_direction[2],out.desired_direction[2]));
    const auto step=product(d.elapsed,number(1.0F/tuning.accumulation_duration));
    out.accumulation=squared>0.95F&&in.accumulate_enabled?
        std::min(1.0F,plus(in.accumulation,step)):std::max(0.0F,difference(in.accumulation,step));
    return out;
}
} // namespace
ActorOrdinaryPlan prepare_actor_ordinary_channels(const ActorOrdinaryInput& input,
    const ActorAnimationTuning& tuning) {
    return ordinary_part(input,tuning,OrdinaryPart::whole);
}

ActorOrdinaryPlan prepare_actor_neutral_channels(const ActorOrdinaryInput& in,
    const ActorAnimationTuning& tuning,float step,ActorNeutralPolicy policy) {
    const auto& d=in.direction;
    if(number(step)<0)throw std::runtime_error("Actor approach step must be nonnegative");
    for(float v:d.current_channels)number(v);
    for(float v:in.target_channels)number(v);
    for(float v:tuning.channel_timing)if(number(v)<0)throw std::runtime_error("Invalid neutral timing");
    ActorOrdinaryPlan out;out.approach_step=step;
    auto targets=in.target_channels;
    const auto timing=[&](unsigned suffix){return tuning.channel_timing.at((suffix-0x48)/8);};
    const auto target=[&](std::size_t index,float value,float duration,float damping=3.0F) {
        targets[index]=value;out.commands.push_back({ActorChannelOperation::target,index,value,duration,damping});
    };
    const auto approach=[&](std::size_t index,float value) {
        number(d.current_channels[index]+std::clamp(difference(value,d.current_channels[index]),-step,step));
        out.commands.push_back({ActorChannelOperation::approach,index,value,step});
    };
    // This helper recomputes its own gate on entry; unlike ordinary continuation,
    // it does not use the caller's retained pre-message gate.
    const bool gated=(d.physical_present&&d.contact_dc&&!bit(d.flags3034,0))||
        bit(d.flags3210,4)||bit(d.flags3210,5)||bit(d.flags3210,10)||
        bit(d.flags3210,11)||bit(d.flags3248,15);
    if(!gated||bit(d.flags3248,15))target(4,0,timing(0xb0));
    target(10,bit(in.flags3204,3)?1.0F:0.0F,timing(0xa0),5.5F);
    approach(11,bit(in.flags3204,4)?1.0F:0.0F);
    if(gated)target(3,d.current_channels[3],-1,0);else target(3,0,timing(0x80));
    target(2,0,timing(0x50));
    if(gated)for(unsigned index:{5u,6u,7u,8u,9u})target(index,d.current_channels[index],-1,0);
    else {
        target(5,0.5F,timing(0x68));
        if(!policy.preserve_channel6)target(6,0.5F,timing(0x58));
        target(7,0,timing(0xc0));target(8,0,timing(0xc0));target(9,targets[6],timing(0x58));
    }
    target(0,0.5F,timing(0xe0));target(1,0.5F,timing(0xe0));
    if(!policy.preserve_turn)target(14,0.5F,timing(0xb8),6.5F);
    if(!policy.preserve_channel17)approach(17,0.5F);
    if(!policy.preserve_selector)out.commands.push_back({ActorChannelOperation::select,3,0});
    return out;
}

void update_actor_animation_channels(const ActorAnimationHost& host,const ActorAnimationTuning& tuning) {
    if(!host.snapshot||!host.apply||!host.deliver||!host.resolve_running_receiver)
        throw std::runtime_error("Actor animation requires explicit synchronous host callbacks");
    auto state=host.snapshot();
    const auto elapsed=state.ordinary.direction.elapsed;
    ActorAnimationPatch patch;
    patch.pending_clear_bits=0xe7;
    if(state.ordinary.mode==5)patch.pending_set_bits|=0x40;
    else if(state.ordinary.direction.physical_present&&state.ordinary.direction.contact_dc)patch.pending_set_bits|=0x80;
    if(state.ordinary.mode==2)patch.pending_set_bits|=0x20;
    host.apply(patch);
    state=host.snapshot();
    if((state.multiplayer_active&&!state.actor_flag14)||!state.component_predicate) {
        if(!host.navigation_body)throw std::runtime_error("Actor navigation continuation is unresolved");
        host.navigation_body(elapsed);
    }
    state=host.snapshot();state.ordinary.direction.elapsed=elapsed;
    const auto& initial=state.ordinary.direction;
    const bool gated=(initial.physical_present&&initial.contact_dc&&!bit(initial.flags3034,0))||
        bit(initial.flags3210,4)||bit(initial.flags3210,5)||bit(initial.flags3210,10)||
        bit(initial.flags3210,11)||bit(initial.flags3248,15);
    const auto early=prepare_actor_direction_channels(initial);
    patch={};
    patch.commands=early.commands;patch.approach_step=early.approach_step;
    host.apply(patch);
    state=host.snapshot();
    patch={};patch.pending_clear_bits=0x10;
    // 0019a5e0..0019a630: physical, actor +14 and the +58 component's virtual
    // +54 (player) request the low-ceiling probe +3d1.
    patch.physical_request3d1=state.ordinary.direction.physical_present&&state.actor_flag14&&state.component_predicate;
    const bool pending_stance=patch.physical_request3d1&&state.physical_3d2?
        true:bit(state.ordinary.direction.flags3248,1);
    if(pending_stance)patch.pending_set_bits=0x10;
    host.apply(patch);
    // The pair decision is fixed before either receiver executes; recipients
    // are reread separately, exactly as the two actor+58 loads in the original.
    const bool was=bit(state.previous_flags,4),now=bit(state.ordinary.flags3204,4);
    if(was!=now) {
        const std::uint32_t first=now?2u:4u;
        host.deliver({ActorAnimationEvent::Kind::stance,host.snapshot().component_receiver,first,0});
        host.deliver({ActorAnimationEvent::Kind::stance,host.snapshot().component_receiver,first+1,0});
    }
    state=host.snapshot();patch={};patch.pending_clear_bits=8;
    if(bit(state.ordinary.direction.flags3248,0))patch.pending_set_bits=8;
    patch.physical_clear38_bit0=state.ordinary.direction.physical_present;
    host.apply(patch);
    state=host.snapshot();state.ordinary.direction.elapsed=elapsed;
    const bool paired=state.ordinary.mode!=7&&state.ordinary.mode!=8&&state.ordinary.mode!=16&&
        (bit(state.ordinary.flags324c,3)||bit(state.ordinary.flags324c,6));
    const bool neutral_body=state.ordinary.mode!=0&&state.ordinary.mode!=2&&state.ordinary.mode!=7&&state.ordinary.mode!=8&&
        state.ordinary.mode!=16&&!bit(state.ordinary.direction.flags3210,27)&&!bit(state.ordinary.direction.flags3210,5);
    if(paired||state.ordinary.mode==16||neutral_body) {
        if(paired) {
            if(!host.paired_body)throw std::runtime_error("Actor paired continuation is unresolved");
            host.paired_body();
        } else if(state.ordinary.mode==16) {
            if(!host.ladder_body)throw std::runtime_error("Actor ladder continuation is unresolved");
            host.ladder_body(elapsed);
        } else {
            patch={};
            if(state.ordinary.mode!=1)patch.pending_set_bits=1;
            const auto plan=prepare_actor_neutral_channels(state.ordinary,tuning,state.approach_step);
            patch.commands=plan.commands;patch.prior_direction=plan.prior_direction;
            host.apply(patch);
        }
        state=host.snapshot();const auto& d=state.ordinary.direction;
        auto squared=product(d.desired_x,d.desired_x);
        squared=plus(squared,product(d.desired_y,d.desired_y));
        squared=plus(squared,product(state.ordinary.desired_z,state.ordinary.desired_z));
        if(number(tuning.accumulation_duration)<=0)throw std::runtime_error("Actor tuning is not initialized");
        const auto delta=product(elapsed,number(1.0F/tuning.accumulation_duration));
        number(state.ordinary.accumulation);
        if(state.ordinary.accumulation<0||state.ordinary.accumulation>1)throw std::runtime_error("Actor accumulation outside unit interval");
        patch={};patch.accumulation=squared>0.95F&&state.ordinary.accumulate_enabled?
            std::min(1.0F,plus(state.ordinary.accumulation,delta)):
            std::max(0.0F,difference(state.ordinary.accumulation,delta));
        host.apply(patch);return;
    }
    if(state.ordinary.mode==7||state.ordinary.mode==8) {
        const auto neutral=prepare_actor_neutral_channels(state.ordinary,tuning,state.approach_step,{false,false,false,true});
        patch={};patch.commands=neutral.commands;patch.prior_direction=std::array<float,4>{};
        if(state.ordinary.direction.speed==0)patch.pending_set_bits=1;
        float side=0.5F;
        if(state.ordinary.direction.desired_x<-0.75F&&state.ordinary.direction.physical_present&&bit(state.physical_flags810,10)) {
            side=0;patch.flags3248_set_bits=0x100;
        } else if(state.ordinary.direction.desired_x>0.75F&&state.ordinary.direction.physical_present&&bit(state.physical_flags810,11)) {
            side=1;patch.flags3248_set_bits=0x200;
        } else patch.flags3248_clear_bits=0x300;
        patch.commands.push_back({ActorChannelOperation::target,6,side,tuning.channel_timing[2],3});
        const auto& d=state.ordinary.direction;
        auto squared=product(d.desired_x,d.desired_x);
        squared=plus(squared,product(d.desired_y,d.desired_y));
        squared=plus(squared,product(state.ordinary.desired_z,state.ordinary.desired_z));
        if(number(tuning.accumulation_duration)<=0)throw std::runtime_error("Actor tuning is not initialized");
        const auto delta=product(elapsed,number(1.0F/tuning.accumulation_duration));
        number(state.ordinary.accumulation);
        if(state.ordinary.accumulation<0||state.ordinary.accumulation>1)throw std::runtime_error("Actor accumulation outside unit interval");
        patch.accumulation=squared>0.95F&&state.ordinary.accumulate_enabled?
            std::min(1.0F,plus(state.ordinary.accumulation,delta)):
            std::max(0.0F,difference(state.ordinary.accumulation,delta));
        host.apply(patch);return;
    }
    const bool moving=state.ordinary.direction.speed!=0;
    auto plan=ordinary_part(state.ordinary,tuning,OrdinaryPart::prefix,gated,state.approach_step);
    patch={};patch.commands=plan.commands;patch.pending_set_bits=plan.pending_set_bits;
    if(moving) {
        patch.desired_direction=plan.desired_direction;patch.facing_positive=plan.facing_positive;
    } else {
        patch.prior_direction=plan.prior_direction;patch.accumulation=plan.accumulation;
        patch.physical_set38_bit0=state.ordinary.direction.physical_present&&bit(state.ordinary.flags3204,3);
    }
    host.apply(patch);
    if(moving) {
        if(plan.world_message_candidate&&(host.snapshot().frame_counter&15u)==0) {
            const auto receiver=host.resolve_running_receiver();
            const auto event_state=host.snapshot();
            host.deliver({ActorAnimationEvent::Kind::running_world,receiver,3,number(event_state.running_value),event_state.source_id});
        }
        state=host.snapshot();state.ordinary.direction.elapsed=elapsed;
        plan=ordinary_part(state.ordinary,tuning,OrdinaryPart::moving_tail,gated,state.approach_step);
        patch={};patch.commands=plan.commands;patch.pending_set_bits=plan.pending_set_bits;
        patch.prior_direction=plan.prior_direction;patch.accumulation=plan.accumulation;
        host.apply(patch);
    }
}
std::vector<ActorHeadingCommand> prepare_actor_heading(const ActorHeadingInput& in) {
    if(in.mode==16)
        throw std::runtime_error("Actor heading mode is outside proved producer");
    if(in.history.empty()||in.history.size()>4096||in.selected>=in.history.size())
        throw std::runtime_error("Actor heading history is outside bounded domain");
    for(float v:in.history)number(v);
    number(in.saved_yaw);number(in.turn);
    if(number(in.elapsed)<=0)throw std::runtime_error("Actor heading elapsed time must be positive");
    float norm=0;for(float v:in.world_rotation)norm=plus(norm,product(number(v),v));
    if(std::abs(difference(norm,1))>0.0001F)throw std::runtime_error("Actor heading requires unit world rotation");
    // The original chooses by signed Z, not magnitude or projected length.
    const auto [x,y,z,w]=in.world_rotation;
    const auto xx=product(x,plus(x,x)),yy=product(y,plus(y,y)),zz=product(z,plus(z,z));
    const auto xy=product(x,plus(y,y)),xz=product(x,plus(z,z)),yz=product(y,plus(z,z));
    const auto wx=product(w,plus(x,x)),wy=product(w,plus(y,y)),wz=product(w,plus(z,z));
    const std::array<float,3> first{difference(difference(1,yy),zz),plus(xy,wz),difference(xz,wy)};
    const std::array<float,3> second{difference(xy,wz),difference(difference(1,xx),zz),plus(yz,wx)};
    const auto heading=number(first[2]<second[2]?std::atan2(first[1],first[0]):std::atan2(-second[0],second[1]));
    using Kind=ActorHeadingCommand::Kind;
    std::vector<ActorHeadingCommand> result;
    const bool last=in.selected+1==in.history.size();
    const auto write=[&](float value) {
        result.push_back({Kind::history,in.selected,value});
        result.push_back({Kind::target,0,last?value:in.history.back()});
    };
    const auto place=[&](float value) {
        const auto half=product(0.5F,value);
        const auto cosine=number(std::cos(half)),sine=number(std::sin(half));
        result.push_back({Kind::world_orientation,0,value,{product(sine,0),product(sine,0),product(sine,1),cosine}});
        result.push_back({Kind::clear_root_160,0,0});
    };
    const bool ordinary=in.mode==0||in.mode==2||in.mode==21||in.mode==28||bit(in.flags3210,22)||
        (in.mode==10&&(in.flags3248&0x1c00)==0);
    const bool hold=(in.mode==0||in.mode==21||bit(in.flags3210,22))&&in.mode!=28&&
        bit(in.flags3204,0)&&in.physical_present&&in.contact_dd;
    if(in.mode==1||in.mode==7||in.mode==8)write(heading);
    else if((in.mode==4||in.mode==5)&&bit(in.flags3248,10)) {
        if(in.transition_previous!=4&&in.transition_current==4) {
            const auto [tx,ty,tz,tw]=in.transition_rotation;
            float length=0;for(float v:in.transition_rotation)length=plus(length,product(number(v),v));
            if(std::abs(difference(length,1))>0.0001F)throw std::runtime_error("Actor transition requires unit world rotation");
            // Only returned Euler Z is consumed.001bd590 computes it before
            // the subsequent pitch/roll work, directly from matrix column1.
            const auto column_x=difference(product(tx,plus(ty,ty)),product(tw,plus(tz,tz)));
            const auto column_y=difference(difference(1,product(tx,plus(tx,tx))),product(tz,plus(tz,tz)));
            auto value=number(std::atan2(-column_x,column_y));
            if(in.euler_negate)value=-value;
            result.push_back({Kind::history,in.selected,value});
            result.push_back({Kind::target,0,last?value:in.history.back(),{},0.5F});
        }
        place(in.saved_yaw);
    }
    else if(!ordinary) {
        if(in.mode==27||!last)place(in.saved_yaw);else write(heading);
    }
    else if(!last) {place(in.saved_yaw);write(in.history.back());}
    else if(hold)write(heading);
    else {
        float value=plus(heading,product(in.turn,in.elapsed));
        unsigned iterations=0;
        while(value>3.1415927F||value<-3.1415927F) {
            if(++iterations>4096)throw std::runtime_error("Actor heading wrap exceeds bounded iteration count");
            value=value>3.1415927F?plus(value,-6.2831855F):plus(value,6.2831855F);
        }
        write(value);place(value);
    }
    result.push_back({Kind::stationary_latch,0,bit(in.flags3204,0)?1.0F:0.0F});
    return result;
}

void update_actor_heading(const ActorHeadingHost& host) {
    if(!host.snapshot||!host.apply)throw std::runtime_error("Actor heading host is incomplete");
    auto state=host.snapshot();
    using Kind=ActorHeadingCommand::Kind;
    if(state.heading.mode==16) {
        if(!bit(state.heading.flags3210,12)) {
            // Validate the admitted source before placement, then derive from
            // the post-placement resolved quaternion rather than this copy.
            auto source=state.heading;source.mode=7;source.world_rotation=state.ladder_reference_rotation;
            (void)prepare_actor_heading(source);
            host.apply({Kind::world_orientation,0,0,state.ladder_reference_rotation});
            host.apply({Kind::clear_root_160,0,0});
            state=host.snapshot();state.heading.mode=7;
            const auto commands=prepare_actor_heading(state.heading);
            for(const auto& command:commands)if(command.kind!=Kind::stationary_latch)host.apply(command);
        }
    } else {
        const auto commands=prepare_actor_heading(state.heading);
        for(const auto& command:commands)if(command.kind!=Kind::stationary_latch)host.apply(command);
    }
    host.apply({Kind::stationary_latch,0,bit(host.snapshot().heading.flags3204,0)?1.0F:0.0F});
}

void update_actor_pair_channels(const ActorPairHost& host,std::uint64_t actor,
    const ActorAnimationTuning& tuning) {
    if(!actor||!host.snapshot||!host.apply)throw std::runtime_error("Actor pair host is incomplete");
    const auto read=[&](std::uint64_t id) {
        const auto state=host.snapshot(id);
        if(!state)throw std::runtime_error("Actor pair source disappeared");
        return *state;
    };
    const auto partner=[&]()->std::optional<std::uint64_t> {
        const auto reference=read(actor).partner;
        if(!reference||!reference->actor)return std::nullopt;
        const auto state=host.snapshot(reference->actor);
        if(!state||state->generation!=reference->generation)return std::nullopt;
        return reference->actor;
    };
    const auto required_partner=[&] {
        const auto id=partner();
        if(!id)throw std::runtime_error("Actor pair reference expired during update");
        return *id;
    };
    const auto neutral=[&](std::uint64_t id) {
        const auto state=read(id).animation;
        const auto plan=prepare_actor_neutral_channels(state.ordinary,tuning,state.approach_step,{true,false,false,false});
        ActorAnimationPatch patch;patch.commands=plan.commands;patch.prior_direction=plan.prior_direction;
        host.apply(id,patch);
    };
    const auto initial=read(actor).animation.ordinary;
    if(!bit(initial.flags324c,3))return;
    if(!partner()) {
        ActorAnimationPatch patch;patch.pending_clear_bits=1;host.apply(actor,patch);neutral(actor);return;
    }
    const auto speed=number(initial.direction.speed),vertical=number(initial.direction.desired_y),turn=number(initial.turn);
    unsigned selection=0;float target=0.5F;bool idle=false;
    if(speed>0) {
        if(vertical>0)selection=1;else if(vertical<0)selection=2;else idle=true;
    } else {
        idle=true;
        if(!bit(initial.flags324c,1)&&turn!=0)
            target=std::clamp(difference(0.5F,product(turn,0.22207667F)),0.0F,1.0F);
    }
    const auto duration=number(tuning.channel_timing.at((0xb8-0x48)/8));
    if(duration<0)throw std::runtime_error("Invalid paired channel timing");
    ActorAnimationPatch channels;
    channels.commands={{ActorChannelOperation::select,4,static_cast<float>(selection)},
        {ActorChannelOperation::target,14,target,duration,6.5F}};
    host.apply(actor,channels);
    host.apply(required_partner(),channels);
    ActorAnimationPatch flag;
    if(idle)flag.pending_set_bits=1;else flag.pending_clear_bits=1;
    host.apply(actor,flag);neutral(actor);
    host.apply(required_partner(),flag);
    neutral(required_partner());
}

}
