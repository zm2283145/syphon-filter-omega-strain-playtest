#include "actor_burning.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Actor burning: "+what);}
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float div(float a,float b){
    if(b==0.0F)fail("division by zero");
    return check(vu::div(a,b));
}
// max.s fd, fs, ft / min.s as the interpreter models them (fs unless ft is strictly beyond it).
float max_s(float fs,float ft){return ft>fs?ft:fs;}
float min_s(float fs,float ft){return ft<fs?ft:fs;}
std::int32_t trunc_word(float v){
    check(v);
    if(!(std::fabs(v)<2147483648.0F))fail("cvt.w.s outside the integer range");
    return static_cast<std::int32_t>(v);
}
float word_float(std::int32_t v){
    if(v>(1<<24)||v<-(1<<24))fail("cvt.s.w of an integer that is not exactly representable");
    return static_cast<float>(v);
}
template<class F>
const F& need(const F& f,const char* what){
    if(!f)fail(std::string(what)+" is not hosted");
    return f;
}

constexpr float rate_fire=std::bit_cast<float>(0x3cfdf3b6U);
constexpr float rate_shock=std::bit_cast<float>(0x3f666666U);
constexpr float w_13=std::bit_cast<float>(0x3f99999aU);
constexpr float w_08=std::bit_cast<float>(0x3f4ccccdU);
constexpr float w_07=std::bit_cast<float>(0x3f333333U);
constexpr std::string_view fire="FireHuman";             // 0049d5e8
constexpr std::string_view smoke="FireHumanSmoke";       // 0049d718
constexpr std::string_view shock="Tazer_Shock1";         // 0049d750
constexpr std::uint32_t one=fire_record_single_node;
constexpr std::array<FireEmitterRecord,10> fire_records{{
    {fire,0x0,one,4.0F,0.0F,rate_fire,1,1},{fire,0x4,one,3.0F,0.0F,rate_fire,1,1},
    {fire,0x7,0x6,2.0F,0.0F,rate_fire,1,1},{fire,0x8,0x7,2.0F,0.0F,rate_fire,1,1},
    {fire,0x6,one,2.0F,0.0F,rate_fire,1,1},{fire,0xb,0xa,2.0F,0.0F,rate_fire,1,1},
    {fire,0xc,0xb,2.0F,0.0F,rate_fire,1,1},{fire,0xa,one,2.0F,0.0F,rate_fire,1,1},
    {fire,0x12,0x11,3.0F,0.0F,rate_fire,1,1},{fire,0xe,0xd,3.0F,0.0F,rate_fire,1,1}}};
constexpr std::array<FireEmitterRecord,1> smoke_records{{{smoke,0x0,one,4.0F,0.0F,rate_fire,0,1}}};
constexpr std::array<FireEmitterRecord,13> shock_records{{
    {shock,0x4,one,1.0F,0.0F,rate_shock,1,0},{shock,0x8,one,0.5F,0.0F,rate_shock,1,0},
    {shock,0xc,one,0.5F,0.0F,rate_shock,1,0},{shock,0xd,one,w_13,0.0F,rate_shock,1,0},
    {shock,0x11,one,w_13,0.0F,rate_shock,1,0},{shock,0xe,one,w_08,0.0F,rate_shock,1,0},
    {shock,0x12,one,w_08,0.0F,rate_shock,1,0},{shock,0xf,one,w_07,0.0F,rate_shock,1,0},
    {shock,0x13,one,w_07,0.0F,rate_shock,1,0},{shock,0x9,0xa,1.0F,0.0F,rate_shock,1,0},
    {shock,0x5,0x6,1.0F,0.0F,rate_shock,1,0},{shock,0x7,one,w_07,0.0F,rate_shock,1,0},
    {shock,0xb,one,w_07,0.0F,rate_shock,1,0}}};

std::int8_t node_of(std::uint32_t index){
    if(index>=fire_node_table.size())fail("node index "+std::to_string(index)+" outside the node table 0049d020");
    return fire_node_table[index];
}
}

std::span<const FireEmitterRecord> fire_effect_descriptor(std::uint32_t address){
    if(address==0x0049d600U)return fire_records;
    if(address==0x0049d730U)return smoke_records;
    if(address==0x0049d760U)return shock_records;
    fail("descriptor "+std::to_string(address)+" is not one of 0049d600 / 0049d730 / 0049d760");
}

FireEffectObject make_fire_effect_object(std::uint64_t actor,std::uint32_t descriptor,std::uint32_t count,float f12){
    if(count>fire_effect_descriptor(descriptor).size())fail("more states than the descriptor's records");
    FireEffectObject o;
    o.actor_74=actor;o.descriptor_78=descriptor;o.intensity_80=fire_effect_initial_80;
    // Each state (0036b180(0, 0x20)): +0 = 0, +4 = f12; +10 is first written
    // by the update that creates the emitter.
    o.states_7c.assign(count,FireEmitterState{0,f12,{}});
    return o;
}

FireMatrix fire_effect_record_matrix(const FireEffectObject& object,std::size_t record,const FireEffectHost& host){
    const auto records=fire_effect_descriptor(object.descriptor_78);
    if(record>=records.size())fail("record outside the descriptor");
    const auto& r=records[record];
    const auto& position=need(host.node_position,"the node position (00132a30 + 00132910)");
    RootVector p=position(node_of(r.node_a));
    if(r.node_b!=fire_record_single_node){
        const RootVector b=position(node_of(r.node_b&0xffU));
        for(std::size_t i=0;i<4;++i)p[i]=mul(add(p[i],b[i]),0.5F);           // vadd.xyzw, vmulx.xyzw by 0.5
    }
    return FireMatrix{RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},p};
}

void update_fire_effect_object(FireEffectObject& object,float dt,const FireEffectHost& host){
    const auto records=fire_effect_descriptor(object.descriptor_78);
    if(object.states_7c.size()>records.size())fail("more states than the descriptor's records");
    float f20=0.0F,f21=0.0F;
    for(std::size_t k=0;k<object.states_7c.size();++k){
        const auto& r=records[k];
        auto& st=object.states_7c[k];
        const FireMatrix m=fire_effect_record_matrix(object,k,host);
        const RootVector& p=m[3];
        if(!st.emitter_0){
            st.emitter_0=need(host.create_emitter,"the particle emitter (0036b180 + 003a35c0 + 003a2dd0)")(r.name,m,r.weight);
            st.previous_10=p;
        }
        if(st.emitter_0){
            float rate;
            if(need(host.fast_decay,"the actor's +3204 bit 5 / +346d")())rate=bits(0x3e4ccccdU);
            else if(need(host.life_108,"the actor's virtual +108")()==3)rate=r.rate;
            else rate=mul(3.0F,-r.rate);
            st.intensity_4=sub(st.intensity_4,mul(rate,dt));
            if(st.intensity_4<0.0F){
                need(host.destroy_emitter,"the emitter release (003a2ba0 + 0036b130)")(st.emitter_0);
                st.emitter_0=0;
            }else if(st.intensity_4<bits(0x3e4ccccdU))need(host.fade_emitter,"the emitter +6a")(st.emitter_0);
            else if(!(st.intensity_4<=1.0F))st.intensity_4=1.0F;
        }
        RootVector v{};
        if(r.velocity)for(std::size_t i=0;i<4;++i)v[i]=sub(st.previous_10[i],p[i]);
        if(st.emitter_0){
            need(host.move_emitter,"the emitter move (003a2400)")(st.emitter_0,dt,m,v);
            need(host.emitter_strength,"the emitter strength (003a2680)")(st.emitter_0,r.strength?mul(st.intensity_4,r.weight):r.weight);
            need(host.emitter_floor,"the emitter +5c")(st.emitter_0,max_s(r.minimum,st.intensity_4));
            f20=add(f20,mul(st.intensity_4,r.weight));
        }
        f21=add(f21,r.weight);
        st.previous_10=p;
    }
    object.intensity_80=f21==0.0F?0.0F:div(f20,f21);
}

std::int32_t fire_zone_strength(float f12){
    return trunc_word(div(mul(word_float(fire_zone_scale),f12),100.0F));
}

FireZone make_fire_zone(std::uint64_t owner,std::uint32_t owner_key,float f12,float f13){
    FireZone z;
    z.owner_90=owner;z.owner_key_94=owner_key;
    z.extent_80={f12,f12,f12,0.0F};
    z.strength_60=fire_zone_strength(f13);
    z.byte_2d=1;                                                          // virtual +5c(1)
    return z;
}

FireZone make_level_fire_zone(std::uint32_t key,const RootVector& first,const RootVector& second,float value){
    FireZone z;
    z.key_0c=key;
    z.owner_90=0;z.owner_key_94=0xffffffffU;
    for(std::size_t k=0;k<3;++k){
        z.center_70[k]=mul(add(second[k],first[k]),0.5F);                 // vadd, vmulx by 0.5
        z.extent_80[k]=mul(sub(second[k],first[k]),0.5F);                 // vsub, vmulx by 0.5
    }
    z.strength_60=fire_zone_strength(value);
    return z;
}

void set_fire_zone_enabled(FireZone& zone,bool enabled){zone.byte_2c=enabled?1:0;}

void set_fire_zone_present(FireZone& zone,bool present){
    if(present)zone.byte_2e=0;
    zone.byte_2d=present?1:0;
}

void link_level_fire_zone(std::span<const std::array<std::uint32_t,2>> links,
                          const std::function<std::uint64_t(std::uint32_t,std::uint32_t)>& find,
                          const std::function<void(std::uint64_t)>& associate){
    for(const auto& l:links){
        if(l[0]==0xffffffffU||l[1]==0xffffffffU)continue;
        const auto found=need(find,"0040c480 (the FIR: link lookup)")(l[0],l[1]);
        if(found)need(associate,"00173d60 (the FIR: link association)")(found);
    }
}

void update_fire_zone(FireZone& zone,float dt,const FireZoneHost& host){
    if(!zone.byte_2c)return;
    const auto owner=need(host.owner,"the zone owner's weak handle (+90 / +94)")();
    if(owner){
        if(!owner->first)return;
        zone.center_70=owner->second;                                     // owner virtual +e4
        zone.center_70[2]=add(zone.center_70[2],2.0F);
    }
    RootVector limit{};
    for(std::size_t i=0;i<3;++i)limit[i]=add(zone.extent_80[i],fire_zone_margin);
    const auto& valid=need(host.valid,"the list entries' weak handles");
    const auto& target=need(host.target,"the list objects' fields");
    for(const auto& [object,key]:need(host.entries,"the list 004f78dc")()){
        if(!valid(object,key))continue;
        const FireZoneTarget t=target(object);
        if(!t.active)continue;
        if(owner&&object==zone.owner_90)continue;
        if(!t.byte_14)continue;
        if(t.human_4c&&t.life_108==3&&!t.byte_33a5)continue;
        std::array<float,3> d{};
        for(std::size_t i=0;i<3;++i)d[i]=std::fabs(sub(t.position_e4[i],zone.center_70[i]));
        if(d[0]<zone.extent_80[0]&&d[1]<zone.extent_80[1]&&d[2]<zone.extent_80[2]){
            const float amount=word_float(trunc_word(mul(1.0F,mul(word_float(zone.strength_60),dt))));
            need(host.damage,"the object's virtual +100")(object,amount);
            if(t.controller_58)need(host.heat,"the controller's virtual +80")(object,1.0F);
            continue;
        }
        if(!t.controller_58||!t.controller_player_54)continue;
        if(!(d[0]<limit[0]&&d[1]<limit[1]&&d[2]<limit[2]))continue;
        for(std::size_t i=0;i<3;++i)d[i]=sub(d[i],zone.extent_80[i]);
        const float m=min_s(max_s(max_s(d[0],d[1]),d[2]),fire_zone_margin);
        need(host.heat,"the controller's virtual +80")(object,div(sub(fire_zone_margin,m),fire_zone_margin));
    }
}

float burning_controller_value(float intensity){
    // The soft-double add 0011c1a8 and the narrowing 0011c928 both truncate
    // (research/FRONTEND.md, loadout_state.cpp): the result is the exact sum
    // truncated to single (round toward zero twice equals once).
    const double h=static_cast<double>(div(intensity,2.0F));
    const double s=h+0.5;
    const double bb=s-h;
    const double err=(h-(s-bb))+(0.5-bb);                                  // s + err is the exact sum (TwoSum)
    float r=static_cast<float>(s);
    const double over=static_cast<double>(r)-s;                            // exact (Sterbenz)
    if((s>0.0&&over>err)||(s<0.0&&over<err))r=std::nextafter(r,0.0F);
    return check(r);
}

void update_burning_actor(float& timer_31f4,float dt,const BurningActorHost& host){
    const auto dying=[&host]{
        const auto l=need(host.life_108,"the actor's virtual +108")();
        return l==3||l==2;
    };
    for(std::uint32_t k=0;k<3;++k){
        const auto intensity=need(host.slot_intensity,"the effect slots' objects (+354c / +3550)")(k);
        if(!intensity)continue;
        const float f21=*intensity;
        if(f21==0.0F){
            if(k==0)need(host.extinguish,"00186360")();
            else need(host.effect_off,"00185d40")(k);
            continue;
        }
        if(k!=0)continue;
        if(!need(host.light_34e0,"+34e0")())continue;
        need(host.light_strength,"00262470")(mul(f21,burning_light_scale));
        if(need(host.byte_14,"+14")()&&need(host.burning_326a,"+326a")()&&!dying()){
            std::uint64_t source=need(host.fire_setter_3480,"the weak handle +3480 / +3484")();
            if(!source)source=host.actor;
            need(host.damage,"the actor's virtual +100")(source,1,4,mul(f21,mul(burning_damage_rate,dt)));
        }
        if(need(host.controller_player,"the controller's +54")()&&host.byte_14())
            need(host.controller_80,"the controller's virtual +80")(burning_controller_value(f21));
    }
    if(need(host.light_34e0,"+34e0")())need(host.glow_up,"+2df6")();
    else if(!dying())need(host.glow_down,"00383680")();
    if(!(timer_31f4<0.0F)){
        timer_31f4=sub(timer_31f4,dt);
        if(timer_31f4<0.0F)
            need(host.damage,"the actor's virtual +100")(host.actor,2,0,word_float(trunc_word(need(host.health_40,"+40")())));
    }
}

std::int32_t decay_fire_damage(std::int32_t v,float dt){
    if(v<=0)return v;
    v=trunc_word(sub(word_float(v),mul(dt,fire_damage_decay_rate)));
    return v<0?0:v;
}

}  // namespace omega
