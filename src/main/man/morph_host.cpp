#include "morph_host.hpp"
#include "vu_float.hpp"
#include "ie_screen.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
float scalar(float value){check(std::isfinite(value)&&std::fpclassify(value)!=FP_SUBNORMAL,"Invalid morph host arithmetic");return value;}
void validate_curve(const MorphCurve& curve){
    check(!curve.empty()&&curve.size()<=256,"Invalid morph curve count");
    for(std::size_t i=0;i<curve.size();++i){
        scalar(curve[i].value);
        check(curve[i].frame>=0,"Negative morph curve frame");
        if(i)check(curve[i-1].frame<curve[i].frame,"Unordered morph curve keys");
    }
}
void append_expression(MorphHostState& state,std::uint8_t shape,float weight){
    if(state.active_count==8)return;
    state.face_slots[state.active_count]={shape,weight};
    if(weight!=0)++state.active_count;
}
void speech_step(MorphHostState& state,std::int32_t elapsed){
    bool continuing=false;
    for(std::size_t i=0;i<state.speech.size();++i){
        auto& channel=state.speech[i];
        check(channel.keys.size()<=32767&&
              (channel.keys.empty()?channel.index<=1:channel.index<channel.keys.size()),"Invalid speech channel extent");
        scalar(channel.previous_weight);scalar(channel.next_weight);
        while(channel.next_time<elapsed&&std::size_t(channel.index)+1<channel.keys.size()){
            ++channel.index;
            const auto key=channel.keys[channel.index];
            channel.previous_time=channel.next_time;channel.next_time=key.time;
            channel.previous_weight=channel.next_weight;
            channel.next_weight=scalar(float(key.weight)*(1.0F/32768.0F));
            if(channel.previous_weight==0&&channel.next_weight==0)channel.previous_time=channel.next_time;
        }
        if(elapsed<=channel.next_time){
            continuing=true;
            if(channel.previous_time<elapsed&&state.active_count<8){
                const float numerator=static_cast<float>(std::int64_t(elapsed)-channel.previous_time);
                const float denominator=static_cast<float>(std::int32_t(channel.next_time)-channel.previous_time);
                const float fraction=scalar(numerator/denominator);
                const float delta=scalar(channel.next_weight-channel.previous_weight);
                const float weight=scalar(channel.previous_weight+scalar(fraction*delta));
                state.face_slots[state.active_count++]={i<19?i:i+4,weight};
            }
        }
    }
    if(!continuing)state.speech_active=false;
}
void gaze_step(MorphHostState& state,std::array<float,3> direction){
    for(float value:direction)scalar(value);
    const auto normalize=[&]{
        const auto x=scalar(direction[0]*direction[0]);
        const auto y=scalar(direction[1]*direction[1]);
        const auto z=scalar(direction[2]*direction[2]);
        const auto norm=scalar(scalar(x+y)+z);check(norm>0,"Zero morph gaze length");
        // 00391f44 / 00391fc4: VU RSQRT Q = vf0.w/sqrt(norm), then vmulq.xyz.
        const auto scale=scalar(vu::rsqrt(norm));
        for(float& value:direction)value=scalar(value*scale);
    };
    normalize();
    if(direction[1]<0){
        direction[1]=0;
        if(direction[0]!=0||direction[2]!=0)normalize();
    }
    for(float& value:direction)value=scalar(value*1.5F);
    for(unsigned axis: {0U,2U}){
        const float value=direction[axis];if(value==0)continue;
        const std::size_t positive=axis==0?22:19,negative=axis==0?21:20;
        std::size_t slot=0;
        while(slot<state.active_count&&state.face_slots[slot].shape_index!=positive&&state.face_slots[slot].shape_index!=negative)++slot;
        if(slot==state.active_count){if(state.active_count==8)return;++state.active_count;}
        state.face_slots[slot]={value>0?positive:negative,std::min(std::abs(value),1.0F)};
    }
}
}
float sample_morph_curve(const MorphCurve& curve,std::int32_t frame){
    validate_curve(curve);
    check(frame>=curve.front().frame&&frame<=curve.back().frame,"Morph curve frame outside bounds");
    std::size_t next=0;while(curve[next].frame<frame)++next;
    if(curve[next].frame==frame)return curve[next].value;
    const auto& a=curve[next-1];const auto& b=curve[next];
    if(a.value==b.value)return b.value;
    const float fraction=scalar(float(frame-a.frame)/float(b.frame-a.frame));
    return scalar(a.value-scalar(fraction*scalar(a.value-b.value)));
}
MorphHostConfig read_morph_host_config(std::istream& input){
    const auto token=[&](const char* expected){std::string value;check(bool(input>>value)&&value==expected,"Invalid morph host config token");};
    const auto integer=[&](std::uint32_t maximum){
        long long value{};check(bool(input>>value)&&value>=0&&value<=maximum,"Invalid morph host config integer");return static_cast<std::uint32_t>(value);
    };
    token("OMEGA_MORPH_HOST_CONFIG");check(integer(1)==1,"Unsupported morph host config version");
    MorphHostConfig result;
    const auto curve=[&](const char* label,MorphCurve& output){
        token(label);const auto count=integer(256);check(count>0,"Empty morph host config curve");
        for(std::uint32_t i=0;i<count;++i){MorphCurveKey key;key.frame=static_cast<std::int32_t>(integer(2147483647));check(bool(input>>key.value),"Invalid morph curve value");output.push_back(key);}
        validate_curve(output);
    };
    curve("GRIP",result.grip_curve);curve("ACTIVE",result.active_expression_curve);curve("PERIODIC",result.periodic_expression_curve);
    token("SHAPES");result.active_expression_shape=static_cast<std::uint8_t>(integer(255));result.periodic_expression_shape=static_cast<std::uint8_t>(integer(255));
    token("END");std::string trailing;check(!(input>>trailing),"Trailing morph host config input");return result;
}
MorphSpeechTracks decode_morph_speech(std::span<const std::uint8_t> bytes){
    check(bytes.size()>=88&&bytes.size()<=4*1024*1024,"Invalid raw LPD extent");
    const auto word=[&](std::size_t at){return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|(std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);};
    check(word(0)==22,"Unsupported raw LPD table header");MorphSpeechTracks tracks;std::size_t cursor=88;
    for(std::size_t channel=0;channel<21;++channel){
        const auto count=word(4+channel*4);check(count<=32767&&count!=1,"Unsupported LPD channel key count");
        check(cursor<=bytes.size()&&count<=(bytes.size()-cursor)/4,"Truncated LPD key stream");
        for(std::uint32_t index=0;index<count;++index){
            const auto bits=word(cursor);cursor+=4;
            const auto time=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits));
            if(index)check(tracks[channel].back().time<time,"Unordered LPD key times");
            tracks[channel].push_back({time,static_cast<std::uint16_t>(bits>>16)});
        }
    }
    if(cursor!=bytes.size()){
        check(bytes.size()==(cursor+2047)/2048*2048,"Invalid LPD trailing extent");
        check(std::all_of(bytes.begin()+cursor,bytes.end(),[](auto value){return value==0;}),"Nonzero LPD padding");
    }
    return tracks;
}
std::int32_t morph_speech_clock_ms(std::uint32_t t1_count,std::uint32_t t1_base,std::int64_t ticks){
    return ie_text_clock_ms(t1_count,t1_base,ticks);
}
void start_morph_speech(MorphHostState& state,const MorphSpeechTracks& tracks,std::int32_t start_ms){
    auto next=state;
    for(std::size_t i=0;i<21;++i){
        const auto& keys=tracks[i];check(keys.size()<=32767&&keys.size()!=1,"Unsupported initial speech key count");
        auto& channel=next.speech[i];channel.keys=keys;channel.index=1;
        if(keys.empty()){channel.previous_time=channel.next_time=0;channel.previous_weight=channel.next_weight=0;}
        else{
            channel.previous_time=keys[0].time;channel.next_time=keys[1].time;
            channel.previous_weight=scalar(float(keys[0].weight)*(1.0F/32768.0F));
            channel.next_weight=scalar(float(keys[1].weight)*(1.0F/32768.0F));
            if(channel.previous_weight==0&&channel.next_weight==0)channel.previous_time=channel.next_time;
        }
    }
    next.speech_active=true;next.speech_start=start_ms;state=std::move(next);
}
void set_morph_base_shape(MorphHostState& state,std::uint8_t shape,bool enabled){
    state.face_slots[0].shape_index=shape;state.base_enabled=enabled;
    if(enabled)state.face_slots[0].weight=1;
}
void update_morph_host(MorphHostState& state,const MorphHostConfig& config,const MorphHostInput& input){
    auto next=state;
    check(next.active_count<=8,"Invalid morph active count");
    for(const auto& slot:next.face_slots){check(slot.shape_index<=255,"Morph shape index outside byte domain");scalar(slot.weight);}
    for(const auto& grip:next.grips){scalar(grip.primary_weight);scalar(grip.secondary_weight);}
    next.active_count=next.base_enabled?1:0;
    if(next.speech_active)speech_step(next,input.speech_clock?
        std::bit_cast<std::int32_t>(std::uint32_t(*input.speech_clock)-std::uint32_t(next.speech_start)):input.speech_elapsed_tick);
    if(input.expressions_enabled){
        for(std::size_t i=0;i<2;++i){
            auto& grip=next.grips[i];
            if(grip.counter&&input.secondary_grip_shapes[i]){
                if(grip.counter==1){grip.secondary_shape=*input.secondary_grip_shapes[i];grip.primary_weight=0;grip.secondary_weight=1;}
                grip.secondary_weight=sample_morph_curve(config.grip_curve,grip.counter);
                grip.primary_weight=scalar(1-grip.secondary_weight);
            }
        }
        if(next.active_expression_counter){
            if(next.active_expression_counter==1){next.periodic_counter=300;next.periodic_reset=3000;}
            if(next.active_count<8)append_expression(next,config.active_expression_shape,
                sample_morph_curve(config.active_expression_curve,next.active_expression_counter));
            next.active_expression_counter=static_cast<std::uint8_t>(next.active_expression_counter+1);
            if(next.active_expression_counter>72)next.active_expression_counter=0;
        }else if(next.periodic_counter>=0){
            if(next.periodic_counter>=next.periodic_reset){
                check(input.periodic_random.has_value(),"Morph periodic reset requires original RNG input");
                next.periodic_counter=0;next.periodic_reset=static_cast<std::uint16_t>((*input.periodic_random&1023)+300);
            }
            if(next.active_count<8)append_expression(next,config.periodic_expression_shape,
                sample_morph_curve(config.periodic_expression_curve,next.periodic_counter));
            const auto bits=static_cast<std::uint16_t>(std::uint16_t(next.periodic_counter)+1);
            next.periodic_counter=std::bit_cast<std::int16_t>(bits);
        }
        if(input.local_gaze)gaze_step(next,*input.local_gaze);
    }
    for(auto& grip:next.grips)if(grip.counter){grip.counter=static_cast<std::uint8_t>(grip.counter+1);if(grip.counter>8)grip.counter=0;}
    state=std::move(next);
}
bool morph_periodic_reset_due(const MorphHostState& state){
    return state.active_expression_counter==0&&state.periodic_counter>=0&&state.periodic_counter>=state.periodic_reset;
}
std::vector<SkinMorphTarget> morph_face_targets(const MorphHostState& state){
    check(state.active_count<=8,"Invalid morph active count");
    return {state.face_slots.begin(),state.face_slots.begin()+state.active_count};
}
std::array<SkinMorphTarget,2> morph_hand_targets(const MorphHostState& state,std::size_t hand){
    check(hand<2,"Invalid morph hand index");const auto& grip=state.grips[hand];
    return {{{grip.primary_shape,grip.primary_weight},{grip.secondary_shape,grip.secondary_weight}}};
}
}
