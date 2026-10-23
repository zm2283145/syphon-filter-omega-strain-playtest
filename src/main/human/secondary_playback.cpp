#include "secondary_playback.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Secondary playback: "+what);}
// EE FPU results as the interpreter models them: finite only, denormals flushed.
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float word_float(std::int32_t v){return static_cast<float>(v);}           // cvt.s.w (exact below 2^24 here)
// The unsigned conversion of 001d7890 / 00190b00 (bltz: halve with the low
// bit kept, convert, double).
float unsigned_float(std::int32_t v){
    if(v>=0)return static_cast<float>(v);
    const auto u=static_cast<std::uint32_t>(v);
    const float half=static_cast<float>(static_cast<std::int32_t>((u>>1)|(u&1U)));
    return check(half+half);
}
// 0049f790 +0x10 per row (boot image).
constexpr std::array<std::string_view,25> table_names{
    "","","DATA_ID_PistolOneHandAim_Right","DATA_ID_PistolTwoHandAim_Right","DATA_ID_PistolOneHandAim_Right",
    "dataDualPistolAim","taser_pose","taser_pose","stand_rifle_held_at_shoulder_combat","stand_rifle_held_at_shoulder_combat",
    "stand_rifle_held_at_shoulder_combat","stand_rifle_held_at_shoulder_combat","ryusaki_ikpose","DATA_ID_PistolLowered",
    "DATA_ID_RifleLowered","stand_hold_shotgun","ryusaki_ikpose","knife_stab_pose","baton_pose","taser_pose",
    "throwing_star_pose","","","DATA_ID_PistolOneHandAim_Right","dataDualPistolAim"};
void validate(const SecondaryClipTiming& c){
    if(c.frames_4<=0)fail("a clip with no frames (the original divides by the count and loops on it)");
    if(c.stride_8<0)fail("a negative key stride");
    if(!std::isfinite(c.frame_period_c)||!(c.frame_period_c>0.0F))fail("a clip frame period outside the finite positive domain");
}
// Remaining time of a playing record: FLT_MAX for a looping clip, else
// (frames - elapsed) / rate.
float remaining(const SecondaryMotionSlot& s){
    const auto& c=*s.clip;
    if(c.looping_10)return std::numeric_limits<float>::max();
    if(s.rate_8==0.0F)fail("a playing record with a zero rate (the original divides by it)");
    return check(check(word_float(c.frames_4)-s.elapsed_c)/s.rate_8);
}
// One record's step in 001d6120's tail; true when it crossed into the last
// +320 seconds this frame.
bool advance(SecondaryMotionSlot& s,float dt,float duration){
    const bool before=remaining(s)<duration;
    set_secondary_motion_time(s,check(s.elapsed_c+check(dt*s.rate_8)));
    const bool after=remaining(s)<duration;
    return after&&!before;
}
}

SecondaryMotionName secondary_motion_name(std::string_view text){
    SecondaryMotionName n{};
    for(std::size_t i=0;i<47&&i<text.size()&&text[i]!='\0';++i)n[i]=static_cast<std::uint8_t>(text[i]);
    return n;
}
std::string_view secondary_motion_text(const SecondaryMotionName& name){
    std::size_t n=0;
    while(n<name.size()&&name[n]!=0)++n;
    return std::string_view(reinterpret_cast<const char*>(name.data()),n);
}
SecondaryMotionName secondary_state_table_name(std::uint8_t state){
    if(state>=table_names.size())fail("state "+std::to_string(state)+" outside the 0049f790 table");
    return secondary_motion_name(table_names[state]);
}
SecondaryMotionName secondary_state_motion_name(const SecondaryComponent& c,std::uint8_t state){
    if(state==0x15&&c.one_shots[0].name)return *c.one_shots[0].name;                  // +c0
    if(state==0x16&&c.one_shots[1].name)return *c.one_shots[1].name;                  // +c8
    return secondary_state_table_name(state);
}
bool secondary_motion_names_match(const SecondaryMotionName& q,const SecondaryMotionName& n){
    if(q[0]==0&&n[0]==0)return true;
    return q==n;
}

SecondaryMotionSlot make_secondary_motion_slot(const SecondaryClipTiming& clip){
    validate(clip);
    SecondaryMotionSlot s;
    s.clip=clip;
    const float frames=word_float(clip.frames_4);
    const float span=check(unsigned_float(clip.frames_4)*clip.frame_period_c);
    if(span==0.0F)fail("a zero clip duration");
    s.rate_8=check(frames/span);
    set_secondary_motion_time(s,0.0F);
    return s;
}

void set_secondary_motion_time(SecondaryMotionSlot& s,float t){
    if(!s.clip)fail("00190b00 on a record without a clip");
    const auto& c=*s.clip;
    validate(c);
    if(!std::isfinite(t)||t<0.0F)fail("a playback time outside the nonnegative finite domain");
    s.elapsed_c=t;s.ended_20=0;
    const float frames=word_float(c.frames_4);
    if(c.looping_10){
        if(!(s.elapsed_c<frames)){
            int wraps=0;
            do{
                if(++wraps>4096)fail("more than 4096 loop wraps in one time step");
                s.elapsed_c=check(s.elapsed_c-frames);
            }while(!(s.elapsed_c<frames));
        }
    }else if(!(s.elapsed_c<frames)){s.elapsed_c=frames;s.ended_20=1;}
    s.cursor_10=0;
    const auto whole=static_cast<std::int32_t>(s.elapsed_c);                     // cvt.w.s (truncation)
    const std::int32_t key=static_cast<std::int32_t>(static_cast<std::uint32_t>(whole)*static_cast<std::uint32_t>(c.stride_8));
    std::int32_t next{};
    if(c.looping_10)next=static_cast<std::uint32_t>(whole)<static_cast<std::uint32_t>(c.frames_4-1)?key+c.stride_8:0;
    else next=static_cast<std::uint32_t>(whole)<static_cast<std::uint32_t>(c.frames_4)?key+c.stride_8:key;
    s.fraction_14=check(s.elapsed_c-unsigned_float(whole));
    s.key_18=key;s.next_key_1c=next;
}

float secondary_motion_phase(const SecondaryComponent& c,const SecondaryPlayback& p,const SecondaryMotionName& name){
    constexpr float none=-1.0F;
    const auto phase=[](const SecondaryMotionSlot& s){
        if(!s.clip)return none;
        validate(*s.clip);
        return check(s.elapsed_c/word_float(s.clip->frames_4));
    };
    const auto& st=c.states;
    if(secondary_motion_names_match(name,secondary_state_motion_name(c,st.next_311))&&st.current_310!=st.next_311)
        return phase(p.next_110);                                                     // 001cac78
    if(secondary_motion_names_match(name,secondary_state_motion_name(c,st.current_310)))
        return phase(p.current_e0);                                                   // 001cadc0
    return none;
}

void install_secondary_motion_slots(SecondaryPlayback& p,const SecondaryComponent& c,bool snapped,bool changed,bool first,
    const SecondaryMotionRegistry& registry){
    if(snapped)p.current_e0=p.next_110;                                               // 001d74a4
    if(changed){
        const auto name=secondary_state_motion_name(c,c.states.next_311);
        SecondaryMotionSlot next;                                                     // the zeroed record (001d76f0 / 001d7760)
        if(name[0]!=0){
            if(!registry.find||!registry.entry)fail("the model's motion registry (0018a670 / 0018a660) is not hosted");
            const auto id=registry.find(secondary_motion_text(name)).value_or(0U);
            const auto entry=registry.entry(id);
            if(!entry)fail("motion registry entry "+std::to_string(id)+" is null (the original dereferences it)");
            if(entry->type_4==0&&entry->clip_24)next=make_secondary_motion_slot(*entry->clip_24);   // 001d7890
        }
        p.next_110=next;
    }
    if(first)p.current_e0=p.next_110;                                                 // 001d77d8
}

bool advance_secondary_motion_slots(SecondaryPlayback& p,SecondaryComponent& c,float dt){
    if(!std::isfinite(dt)||dt<0.0F)fail("a frame time outside the nonnegative finite domain");
    auto& st=c.states;
    if(!p.current_e0.clip&&!p.next_110.clip)return false;                             // 001d6174
    const bool both=st.blend_318>0.0F&&p.current_e0.clip&&p.next_110.clip;            // s7
    const float duration=st.duration_320;
    bool restored=false;
    const auto restore=[&]{st.request_312=st.request_313;restored=true;};
    if(both){
        if(advance(p.current_e0,dt,duration)&&
           ((st.current_310==0x15&&st.next_311!=0x16)||(st.current_310==0x16&&st.next_311!=0x15)))restore();
        if(advance(p.next_110,dt,duration)&&(st.next_311==0x15||st.next_311==0x16))restore();
    }else{
        const bool current=p.current_e0.clip.has_value();
        const auto state=current?st.current_310:st.next_311;
        if(advance(current?p.current_e0:p.next_110,dt,duration)&&(state==0x15||state==0x16))restore();
    }
    return restored;
}

bool run_secondary_playback(SecondaryPlayback& p,SecondaryComponent& c,const SecondaryStep& step,bool first,float dt,
    const SecondaryMotionRegistry& registry){
    install_secondary_motion_slots(p,c,step.snapped,step.changed,first,registry);
    if(first)return false;                                                            // 001cdd04: the rest of 001cb160 is skipped
    return advance_secondary_motion_slots(p,c,dt);
}

}
