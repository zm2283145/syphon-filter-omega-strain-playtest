#include "float_bits.hpp"
#include "actor_ladder.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <vector>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float scalar(float v) {
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Ladder value outside finite normal-or-zero domain");
    return v;
}
float mul(float a,float b){return scalar(a*b);}
float add(float a,float b){return scalar(a+b);}
float sub(float a,float b){return scalar(a-b);}
float div(float a,float b){if(b==0)throw std::runtime_error("Ladder division by zero");return scalar(a/b);}
void check(const ActorLadderTuning& tuning) {
    if(scalar(tuning.rung_spacing)<=0)throw std::runtime_error("Ladder rung spacing must be positive");
    scalar(tuning.lower_offset);
}
bool bounded(const std::optional<ActorLadderContacts>& physical) {
    if(!physical)return false;
    scalar(physical->upper_990);scalar(physical->lower_998);scalar(physical->reference_988);
    return physical->upper_990>-std::numeric_limits<float>::max()&&
        physical->lower_998<std::numeric_limits<float>::max();
}
}
ActorLadderIntent detect_actor_ladder_intent(const ActorLadderIntentInput& in) {
    scalar(in.vertical);scalar(in.slide_amount);
    const bool c= in.physical&&in.physical->contact_99c;
    const bool d= in.physical&&in.physical->contact_99d;
    const bool inhibit=in.physical&&in.physical->contact_99f&&
        (in.active_actions&(1u<<13))&&std::abs(in.vertical)<0.5F;
    ActorLadderIntent out;
    out.requested_slide=(in.slide_enabled||in.slide_amount>0.75F)&&!in.dismount_requested;
    out.requested_up=!out.requested_slide&&in.vertical>0&&!inhibit&&!in.dismount_requested;
    out.requested_down=!out.requested_slide&&in.vertical<0&&!inhibit&&!in.dismount_requested;
    out.slide=out.requested_slide&&!c;out.up=out.requested_up&&!d;out.down=out.requested_down&&!c;
    return out;
}
bool actor_ladder_channel_arrived(float previous,float current,float target,float threshold) {
    scalar(previous);scalar(current);scalar(target);
    if(scalar(threshold)<0)throw std::runtime_error("Ladder arrival threshold must be nonnegative");
    return std::abs(sub(target,current))<=threshold&&threshold<std::abs(sub(target,previous));
}
ActorLadderProximity actor_ladder_proximity(const std::optional<ActorLadderContacts>& physical,
    float world_z,const ActorLadderTuning& tuning) {
    scalar(world_z);check(tuning);ActorLadderProximity out;
    if(!bounded(physical))return out;
    const auto window=mul(tuning.rung_spacing,1.5F),lower=add(physical->lower_998,tuning.lower_offset);
    const auto odd=[&](float coordinate) {
        const auto inverse=scalar(1.0F/tuning.rung_spacing);
        const auto distance=mul(std::abs(sub(coordinate,physical->reference_988)),inverse);
        const auto remainder=scalar(std::fmod(distance,2.0F));
        return remainder>0.5F&&remainder<1.5F;
    };
    if(sub(physical->upper_990,world_z)<window)out.upper=odd(physical->upper_990);
    if(sub(world_z,lower)<window)out.lower=odd(lower);
    return out;
}
float actor_ladder_slide_amount(float value,float elapsed,bool sliding,bool enabled) {
    if(scalar(value)<0||value>1||scalar(elapsed)<0)throw std::runtime_error("Ladder slide timing outside bounded domain");
    if(!sliding)return 0;
    return enabled?std::min(1.0F,add(value,elapsed)):std::max(0.0F,sub(value,mul(elapsed,2.0F)));
}
RootVector blend_actor_ladder_position(const RootVector& current,const RootVector& target,
    float elapsed,const std::optional<ActorLadderContacts>& physical,const ActorLadderTuning& tuning) {
    for(float v:current)scalar(v);for(float v:target)scalar(v);
    if(scalar(elapsed)<0)throw std::runtime_error("Ladder elapsed time must be nonnegative");
    check(tuning);const bool clamp=bounded(physical);
    const auto amount=mul(elapsed,9.0F);RootVector result;
    for(std::size_t i=0;i<4;++i)result[i]=sub(add(current[i],mul(target[i],amount)),mul(current[i],amount));
    result[3]=1;
    if(clamp)result[2]=std::min(std::max(result[2],add(physical->lower_998,tuning.lower_offset)),physical->upper_990);
    return result;
}
bool actor_ladder_left_hand(const ActorLadderSnapshot& in,const ActorLadderTuning& tuning,bool account_exit) {
    check(tuning);if(!in.physical)return true;
    const auto distance=sub(scalar(in.world_position[2]),scalar(in.physical->reference_988));
    const auto rounded=scalar(std::floor(add(0.5F,scalar(std::abs(distance)/mul(2,tuning.rung_spacing)))));
    bool left=scalar(std::fmod(rounded,2.0F))<0.999F;
    if(account_exit&&(in.actor.ordinary.direction.flags3248&0x6000000u))left=!left;
    return left;
}
RootVector actor_ladder_target_position(const ActorLadderSnapshot& in,const ActorLadderTuning& tuning,bool exact) {
    check(tuning);for(float v:in.world_position)scalar(v);
    if(!bounded(in.physical))return in.world_position;
    for(float v:in.reference_position)scalar(v);
    if(in.reference_position[2]!=in.physical->reference_988)
        throw std::runtime_error("Ladder reference position disagrees with physical reference height");
    const auto distance=sub(in.world_position[2],in.physical->reference_988);
    float snapped=mul(scalar(std::floor(add(0.5F,scalar(distance/2.0F)))),2);
    float lower=sub(add(in.physical->lower_998,tuning.lower_offset),in.physical->reference_988);
    float upper=sub(in.physical->upper_990,in.physical->reference_988);
    if(!exact) {
        lower=mul(scalar(std::ceil(scalar(sub(lower,mul(0.5F,tuning.rung_spacing))/2.0F))),2);
        upper=mul(scalar(std::floor(scalar(add(upper,mul(0.5F,tuning.rung_spacing))/2.0F))),2);
    }
    if(lower<=upper)snapped=std::clamp(snapped,lower,upper);
    auto result=in.reference_position;result[2]=add(in.physical->reference_988,snapped);return result;
}
namespace {
float bits_of(std::uint32_t v){return std::bit_cast<float>(v);}
// EE max.s / min.s: max(fs, ft) keeps fs unless fs < ft; min keeps fs unless ft < fs.
float max_s(float fs,float ft){return fs<ft?ft:fs;}
float min_s(float fs,float ft){return ft<fs?ft:fs;}
bool actor_bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
// 00195ce0-style rescan of physical bits 12..0 into +28 after a bit change.
void physical_bit(ActorPhysicalMode& mode,unsigned bit,bool on){
    if(on)mode.bits|=1U<<bit;else mode.bits&=~(1U<<bit);
    std::int32_t top=12;
    while(top>=0&&!actor_bit(mode.bits,static_cast<unsigned>(top)))--top;
    mode.value=top;
}
}
void run_actor_ladder_body(ActorAnimationReceiverState& actions,ActorLadderBodyFields& fields,
    const ActorLadderPhysicalView* physical,ActorMotionTransitionState& transition,std::uint64_t receiver,
    float elapsed,const ActorLadderBodyHost& host) {
    if(!host.neutral_channels||!host.channel||!host.set_channel||!host.set_channel_target||!host.root_world_position||
       !host.set_root_base||!host.sound||!host.dismount||!host.resolve||!host.resolve_root||!host.install)
        throw std::runtime_error("Ladder body requires complete host services");
    if(scalar(elapsed)<0||!receiver)throw std::runtime_error("Ladder body configuration outside bounded domain");
    if(physical&&(!physical->contacts||!physical->pending))throw std::runtime_error("Ladder body physical view is incomplete");
    const float rung=bits_of(0x3f800000U),lower_offset=bits_of(0x40e00000U);   // 004a0418, 004a0410
    const ActorLadderTuning tuning{rung,lower_offset};
    // Live views of the actor and physical fields, read at each use.
    const auto contacts=[&]()->std::optional<ActorLadderContacts>{
        if(!physical)return std::nullopt;
        const auto& c=*physical->contacts;
        return ActorLadderContacts{c.top_990,c.bottom_998,physical->reference_980[2],
            c.flags_99c[0]!=0,c.flags_99c[1]!=0,c.flags_99c[3]!=0};
    };
    const auto ladder_snapshot=[&]{
        ActorLadderSnapshot s;s.physical=contacts();
        s.reference_position=physical?physical->reference_980:RootVector{0,0,0,1};
        s.world_position=host.root_world_position(ActorRoot::ladder);
        s.actor.ordinary.direction.flags3248=actions.flags_3248;
        return s;
    };
    const auto left_hand=[&]{return actor_ladder_left_hand(ladder_snapshot(),tuning,false);};   // 00476120(actor, 0)
    const auto bounded=[&]{
        return physical&&!(physical->contacts->top_990<=-std::numeric_limits<float>::max())&&
            physical->contacts->bottom_998<std::numeric_limits<float>::max();
    };
    const auto target21=[&](float value){host.set_channel_target(21,value,bits_of(0x3f000000U),0.0F,false);};   // 0019d5a0
    const auto release=[&]{                                                   // 00475bb0
        actions.flags_3248&=~0x2000000U;actions.flags_3248&=~0x4000000U;target21(1.0F);
    };
    const auto align=[&](bool lower){                                         // 00475c00(actor, lower)
        if(!bounded())return;
        auto p=host.root_world_position(ActorRoot::edge);
        if(lower){p[2]=sub(add(physical->contacts->bottom_998,lower_offset),1.0F);host.set_root_base(ActorRoot::edge,p);actions.flags_3248|=0x2000000U;}
        else{p[2]=physical->contacts->top_990;host.set_root_base(ActorRoot::edge,p);actions.flags_3248|=0x4000000U;}
        target21(0.0F);
    };
    const auto position=[&]{                                                  // 00475d80(actor, dt)
        const auto target=actor_ladder_target_position(ladder_snapshot(),tuning,false);   // 00475ee0(.., 0)
        const auto current=host.root_world_position(ActorRoot::ladder);
        const float amount=mul(bits_of(0x41100000U),elapsed);                  // 9 * dt
        RootVector p;
        for(std::size_t i=0;i<4;++i)p[i]=sub(add(add(current[i],0.0F),mul(target[i],amount)),mul(current[i],amount));
        p[3]=1.0F;
        if(bounded())p[2]=min_s(max_s(p[2],add(physical->contacts->bottom_998,lower_offset)),physical->contacts->top_990);
        host.set_root_base(ActorRoot::ladder,p);
    };
    const auto climbing_off=[&](bool physical_on){                            // action 14 off, physical bit 8
        set_actor_action(actions,14,false);
        if(physical)physical_bit(*physical->pending,8,physical_on);
    };
    // Override {0.2, -0.2, 5, -5}, lookup, consume on success, replace install.
    const auto submit=[&](const std::function<ActorMotionRegistration()>& lookup,std::int32_t first,std::vector<MotionPhaseHook> hooks){
        const float width=bits_of(0x3e4ccccdU),bias=min_s(bits_of(0xbe4ccccdU),width);
        set_actor_motion_transition(transition,{width,bias,width==0?std::numeric_limits<float>::max():div(1.0F,width),
            bias==0?std::numeric_limits<float>::max():div(1.0F,bias)});
        const auto r=lookup();
        if(!r.node)return;                                                    // unresolved: the override stays pending
        const auto used=transition.value;transition.pending=false;
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=used.width;q.bias=used.bias;q.rate=used.rate;q.secondary_rate=used.secondary_rate;
        q.limit=-1;q.initial_phase=0;q.notifications_active=true;
        const std::array<std::size_t,5> slots{0,1,3,5,6};
        for(std::size_t i=0;i<slots.size();++i)q.notifications[slots[i]]={first+static_cast<std::int32_t>(i),receiver};
        q.phase_hooks=std::move(hooks);
        host.install(q,MotionInstallMode::replace);
    };
    host.neutral_channels();                                                  // 004740a4
    if(actor_bit(actions.flags_3248,27)&&!(scalar(host.channel(21).current)<bits_of(0x3f19999aU))){   // 0.6
        actions.flags_3248|=left_hand()?0x800000U:0x1000000U;
        actions.flags_3248&=~0x8000000U;
    }
    if(actor_bit(actions.active_actions,12)||actor_bit(actions.active_actions,15)){   // 00475ad0
        if(!actor_bit(actions.active_actions,12)&&!actor_bit(actions.flags_3248,21))position();
        return;
    }
    const auto intent=detect_actor_ladder_intent({contacts(),actions.active_actions,fields.vertical_31c8,
        fields.slide_31d8,fields.slide_enabled_31b5,fields.exit_requested_31b6});   // 004762a0
    const bool slide=intent.slide,up=intent.up,down=intent.down;
    const bool none=!slide&&!up&&!down;
    const auto f0=actions.flags_3248;
    const bool changed=(up&&!actor_bit(f0,18))||(down&&!actor_bit(f0,19))||(slide&&!actor_bit(f0,20));
    // Proximity (004742a4..474434): 1.5 rungs from +990 or +998+7 on an odd rung from +988.
    bool near_upper=false,near_lower=false;
    if(bounded()){
        const float z=host.root_world_position(ActorRoot::ladder)[2];
        const auto& c=*physical->contacts;
        const float lower=add(c.bottom_998,lower_offset);
        const float window=mul(bits_of(0x3fc00000U),rung);
        const float inverse=div(1.0F,rung);
        const auto odd=[&](float coordinate){
            const float r=scalar(std::fmod(mul(std::fabs(sub(coordinate,physical->reference_980[2])),inverse),2.0F));   // 00121ae8
            return !(r<=bits_of(0x3f000000U))&&r<bits_of(0x3fc00000U);
        };
        if(sub(c.top_990,z)<window&&odd(c.top_990))near_upper=true;
        if(sub(z,lower)<window&&odd(lower))near_lower=true;
    }
    const auto ch=host.channel(21);
    if(actor_ladder_channel_arrived(ch.previous,ch.current,ch.target,bits_of(0x3e4ccccdU)))host.sound();   // 00475b40, 00188f00
    // Slide amount (00474460..4744d8) -> channel 18.
    if(actor_bit(actions.flags_3248,20)){
        if(fields.slide_enabled_31b5)fields.slide_31d8=min_s(1.0F,add(fields.slide_31d8,elapsed));
        else fields.slide_31d8=max_s(0.0F,sub(fields.slide_31d8,mul(2.0F,elapsed)));
    }else fields.slide_31d8=0.0F;
    host.set_channel(18,fields.slide_31d8);
    // Keep the current climb (004744e0..474590).
    if(!actor_bit(actions.active_actions,13)){
        const auto f=actions.flags_3248;
        if((up&&!near_upper&&actor_bit(f,18))||(down&&!near_lower&&actor_bit(f,19))||(slide&&actor_bit(f,20))){
            actions.flags_3248&=~0x800000U;actions.flags_3248&=~0x1000000U;
            return;
        }
    }
    // Start a climb or a slide (00474598..474658).
    bool start=slide;
    if(!start&&((up&&!near_upper)||(down&&!near_lower))&&actor_bit(actions.active_actions,13)){
        const auto f=actions.flags_3248;
        if((!actor_bit(f,22)||actor_bit(f,21))&&!actor_bit(f,25)&&!actor_bit(f,26)&&
           !(scalar(host.channel(21).current)<=bits_of(0x3ecccccdU)))start=true;   // 0.4
    }
    if(start){                                                                // 00474658
        climbing_off(true);
        if(slide){
            submit([&]{return host.resolve("sliderLadderSlide");},0x55,{});
            actions.flags_3248|=0x100000U;actions.flags_3248&=~0x80000U;actions.flags_3248&=~0x40000U;
            release();
        }else{
            const bool left=left_hand();
            const char* name=up?(left?"blendLadderClimbUp_LeftHand":"blendLadderClimbUp_RightHand"):
                                 (left?"blendLadderClimbDown_LeftHand":"blendLadderClimbDown_RightHand");
            const std::int32_t first=left?0x5b:0x5a,second=left?0x5a:0x5b;
            const float high=min_s(max_s(bits_of(0x3f666666U),0.0F),1.0F),low=min_s(max_s(bits_of(0x3ecccccdU),0.0F),1.0F);
            submit([&]{return host.resolve_root(name);},0x50,
                {{high,high,{first,receiver}},{low,low,{second,receiver}},{high,high,{0x5c,receiver}},{low,low,{0x5c,receiver}}});
            if(up){actions.flags_3248|=0x40000U;actions.flags_3248&=~0x80000U;}
            else if(down){actions.flags_3248|=0x80000U;actions.flags_3248&=~0x40000U;}
        }
        actions.flags_3248&=~0x800000U;actions.flags_3248&=~0x1000000U;      // 00475278
        position();
        return;
    }
    // Stop (004752b8..475934).
    {
        const auto f=actions.flags_3248;
        if(actor_bit(actions.active_actions,14)&&(actor_bit(f,23)||actor_bit(f,24)||actor_bit(f,20))&&
           (none||changed||(up&&near_upper)||(down&&near_lower))){
            fields.pending_3208|=1U;
            climbing_off(false);
            const bool left=left_hand();
            if(left){actions.flags_3248|=0x10000U;actions.flags_3248&=~0x20000U;}
            else{actions.flags_3248|=0x20000U;actions.flags_3248&=~0x10000U;}
            submit([&]{return host.resolve(left?"sliderLadderClimbStop_LeftHand":"sliderLadderClimbStop_RightHand");},0x4b,{});
            if((up&&near_upper)||(down&&near_lower))align(near_lower);
            position();
            return;
        }
    }
    // Dismount, reversal, alignment or idle (00475940..475ac4).
    if(fields.exit_requested_31b6){
        fields.pending_3208|=1U;
        host.dismount();                                                      // 004728d0
        fields.exit_requested_31b6=false;
        position();
        return;
    }
    {
        const auto f=actions.flags_3248;
        if((up&&actor_bit(f,25))||(down&&actor_bit(f,26))){
            fields.pending_3208|=1U;actions.flags_3248|=0x8000000U;release();position();
            return;
        }
        if(((down&&near_lower&&!actor_bit(f,25))||(up&&near_upper&&!actor_bit(f,26)))&&!actor_bit(f,21)&&
           actor_bit(actions.active_actions,13)){
            fields.pending_3208|=1U;align(near_lower);position();
            return;
        }
    }
    if(actor_bit(actions.active_actions,14))return;
    fields.pending_3208|=1U;position();
}

namespace {
ActorLadderSnapshot exit_snapshot(const ActorAnimationReceiverState& actions,const ActorLadderPhysicalView* physical,const RootVector& ladd){
    ActorLadderSnapshot s;
    if(physical){
        const auto& c=*physical->contacts;
        s.physical=ActorLadderContacts{c.top_990,c.bottom_998,physical->reference_980[2],c.flags_99c[0]!=0,c.flags_99c[1]!=0,c.flags_99c[3]!=0};
        s.reference_position=physical->reference_980;
    }
    s.world_position=ladd;
    s.actor.ordinary.direction.flags3248=actions.flags_3248;
    return s;
}
bool contacts_bounded(const ActorLadderPhysicalView* physical){
    return physical&&!(physical->contacts->top_990<=-std::numeric_limits<float>::max())&&
        physical->contacts->bottom_998<std::numeric_limits<float>::max();
}
const ActorLadderTuning exit_tuning{std::bit_cast<float>(0x3f800000U),std::bit_cast<float>(0x40e00000U)};   // 004a0418, 004a0410
}
void request_actor_ladder_exit(const ActorAnimationReceiverState& actions,ActorLadderExitFields& fields,
    const ActorLadderPhysicalView* physical,std::int32_t action,const ActorLadderExitHost& host) {
    if(action<30||action>35)throw std::runtime_error("Ladder exit action outside 30..35");
    if(!host.channel||!host.root_world_position||!host.find_node||!host.send_self)throw std::runtime_error("Ladder exit host incomplete");
    if(physical&&(!physical->contacts||!physical->pending))throw std::runtime_error("Ladder exit physical view is incomplete");
    if(fields.requested_31b6||actions.highest_action!=16)return;                          // 004735c0
    if(actor_bit(actions.active_actions,12)||actor_bit(actions.active_actions,15))return;
    const auto contact=[&](std::size_t k){return physical&&physical->contacts->flags_99c[k]!=0;};
    const float settle=bits_of(0x3e19999aU);                                               // 0.15
    std::int32_t selected=-1;
    if(action==35)selected=35;
    else{
        const bool top=contact(4)||(actor_bit(actions.flags_3248,26)&&scalar(host.channel(21).current)<settle);
        if(top&&(action==32||action==30))selected=32;
        else{
            const bool bottom=contact(2)||(actor_bit(actions.flags_3248,25)&&scalar(host.channel(21).current)<settle);
            if(bottom&&(action==31||action==30))selected=31;
            else if(contact(3)){
                if(contact(5)&&(action==33||action==30))selected=33;
                else if(contact(6)&&(action==34||action==30))selected=34;
            }
        }
    }
    if(selected==35){                                                                      // 00473794
        if(contact(8))host.send_self(0x24);
        return;
    }
    if(selected<0)return;
    fields.requested_31b6=true;                                                            // 00473824
    const bool left=actor_ladder_left_hand(exit_snapshot(actions,physical,host.root_world_position(ActorRoot::ladder)),exit_tuning,true);
    const char* name{};std::int8_t kind{};
    switch(selected){
    case 32:name=left?"animLadderGetOffTop_LeftHand":"animLadderGetOffTop_RightHand";kind=4;break;
    case 31:name=left?"sliderLadderGetOff_LeftHand":"sliderLadderGetOff_RightHand";kind=2;break;
    case 33:name=left?"animLadderGetOffLeft_LeftHand":"animLadderGetOffLeft_RightHand";kind=3;break;
    default:name=left?"animLadderGetOffRight_LeftHand":"animLadderGetOffRight_RightHand";kind=3;break;
    }
    const auto node=host.find_node(name);                                                  // 00473fd0
    fields.node_3514=node?*node:0U;
    fields.kind_3518=kind;
}
std::optional<ActorLadderExitMessage> dismount_actor_ladder(const ActorAnimationReceiverState& actions,
    const ActorLadderExitFields& fields,const ActorLadderPhysicalView* physical,const ActorLadderDismountInput& input,
    const ActorLadderDismountHost& host) {
    if(!host.channel||!host.root_world_placement)throw std::runtime_error("Ladder dismount host incomplete");
    if(!contacts_bounded(physical))return std::nullopt;                                     // 004728f4
    const auto kind=fields.kind_3518;
    if(kind<0||kind>9)throw std::runtime_error("Ladder dismount: exit kind +3518 outside the contact bytes");
    const auto& c=*physical->contacts;
    if(c.flags_99c[static_cast<std::size_t>(kind)]==0)return std::nullopt;                  // 0047295c
    const float z=scalar(host.root_world_placement(ActorRoot::ladder).position[2]);
    const float lower=add(c.bottom_998,exit_tuning.lower_offset);
    const float reach=mul(bits_of(0x3f400000U),exit_tuning.rung_spacing);                  // 004a03c0 * 004a0418
    const bool near_bottom=std::fabs(sub(z,lower))<=reach;
    const bool near_top=std::fabs(sub(z,c.top_990))<=reach;
    const float settle=bits_of(0x3e19999aU);
    const bool bottom=kind==2&&(near_bottom||(actor_bit(actions.flags_3248,25)&&scalar(host.channel(21).current)<settle));
    const bool top=kind==4&&(near_top||(actor_bit(actions.flags_3248,26)&&scalar(host.channel(21).current)<settle));
    if((kind==2||kind==4)&&!bottom&&!top)return std::nullopt;                               // 00472b48
    if(input.player_component&&!input.actor_byte_14)return std::nullopt;                    // 00472b54
    ActorLadderExitMessage m;m.node=fields.node_3514;
    const auto ladd=host.root_world_placement(ActorRoot::ladder);
    if(bottom){
        float level=lower;
        if(actor_bit(actions.flags_3248,25))level=sub(level,1.0F);
        m.placement.position=ladd.position;m.placement.position[2]=level;m.action=0x1f;
    }else if(top){
        m.placement.position=ladd.position;m.placement.position[2]=c.top_990;m.action=0x20;
    }else{
        m.placement.position=actor_ladder_target_position(exit_snapshot(actions,physical,ladd.position),exit_tuning,true);
        m.action=0x1e;
    }
    m.placement.orientation=ladd.orientation;                                              // 001323f0('ladd')
    return m;
}
void install_actor_ladder_exit(std::uint8_t life_state,bool physical_present,const ActorLadderExitMessage& message,
    ActorMotionTransitionState& transition,std::uint64_t receiver,const ActorLadderInstallHost& host) {
    if(life_state==2||life_state==3)return;                                                // 00472e58
    if(!host.place_edge_root||!host.bind_support||!host.resolve_id||!host.retained||!host.install||!receiver)
        throw std::runtime_error("Ladder exit installation host incomplete");
    if(message.object)throw std::runtime_error("Ladder exit: an object reference (195ee0/195de0 owner conversion) is not native");
    if(message.node==0)throw std::runtime_error("Ladder exit: node 0 (missing exit motion name, index-0 fallback) is not native");
    host.place_edge_root(message.placement);                                               // 00472f7c
    if(physical_present)host.bind_support(1,message.placement,0);                          // 00472f88
    const float d=bits_of(0x3f000000U);                                                    // 0049d038
    const auto transition_value=ActorMotionTransition{d,d,div(1.0F,d),div(1.0F,d)};
    const auto submit=[&](const ActorMotionRegistration& r,MotionInstallMode mode,bool tokens){
        if(!r.node)return;
        const auto used=transition.value;transition.pending=false;
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=used.width;q.bias=used.bias;q.rate=used.rate;q.secondary_rate=used.secondary_rate;
        q.limit=-1;q.initial_phase=0;
        if(tokens){
            q.notifications_active=true;
            q.notifications[0]={message.action==0x20?0x62:0x5d,receiver};
            q.notifications[1]={0x5e,receiver};q.notifications[3]={0x5f,receiver};
            q.notifications[5]={0x60,receiver};q.notifications[6]={0x61,receiver};
        }
        host.install(q,mode);
    };
    set_actor_motion_transition(transition,transition_value);
    submit(host.resolve_id(message.node),MotionInstallMode::replace,true);                  // 004731fc
    set_actor_motion_transition(transition,transition_value);
    submit(host.retained(),MotionInstallMode::queue,false);                                // 00473474
}
void cancel_actor_ladder(ActorAnimationReceiverState& actions,ActorPhysicalMode* physical,const ActorLadderCancelHost& host) {
    if(!actor_bit(actions.active_actions,16))return;                                       // 001ab9a0
    if(actions.highest_action==16){
        if(!host.retained||!host.install)throw std::runtime_error("Ladder cancel host incomplete");
        const auto r=host.retained();                                                      // 001ae270/001ae260 (unchecked)
        if(!r.node)throw std::runtime_error("Ladder cancel: retained motion +326c is unresolved");
        const float d=bits_of(0x3f000000U);
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=q.bias=d;q.rate=q.secondary_rate=div(1.0F,d);q.limit=-1;q.initial_phase=0;
        host.install(q,MotionInstallMode::replace);                                        // 001aba48
    }
    for(unsigned a=12;a<=16;++a)set_actor_action(actions,a,false);                         // 001aba80..1abb98
    if(physical)for(unsigned b=6;b<=9;++b)physical_bit(*physical,b,false);                 // 001a7e60 x4
}
}
