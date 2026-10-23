#include "area_effect.hpp"
#include "game_libm.hpp"
#include "projectile_candidates.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Area effect: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float div(float a,float b){
    if(b==0.0F)throw std::runtime_error("Area effect: division by zero");
    return check(vu::div(a,b));
}
RootVector vsub(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=sub(a[i],b[i]);return r;}
RootVector vadd(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=add(a[i],b[i]);return r;}
RootVector vscale(const RootVector& a,float s){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=mul(a[i],s);return r;}
constexpr float growth_time=5.0F;            // 0048afe8
constexpr float band_height=8.0F;            // 0048afe0
constexpr float flash_scale=60.0F;           // 0048b000
constexpr float view_limit=std::bit_cast<float>(0x3f666666U);   // 0048b008 (0.9)
constexpr float blind_duration=2.0F;         // 0048aff0 BlindDuration
constexpr float blind_nv_factor=3.0F;        // 0048aff8 BlindNVFactor
constexpr float stealth_scale=200.0F;        // 00259d78 (lui 0x4348)
constexpr float extent_fraction=std::bit_cast<float>(0x3f666666U);   // 0025a744 (0.9)
constexpr float retire_delay=std::bit_cast<float>(0x3dcccccdU);      // 0025a0d0 (0.1)
constexpr std::int32_t exposure_node=14;     // *0049d024
// 004f8060 (built once by 0025a5c8..0025a658).
constexpr std::array<RootVector,4> directions{{{0.0F,1.0F,0.0F,0.0F},{0.0F,-1.0F,0.0F,0.0F},{1.0F,0.0F,0.0F,0.0F},{-1.0F,0.0F,0.0F,0.0F}}};
AreaEvent event(AreaEvent::Kind k){AreaEvent e;e.kind=k;return e;}
std::optional<RootVector> traced(AreaWorld& world,const RootVector& start,const RootVector& d,std::vector<AreaEvent>& out){
    auto e=event(AreaEvent::Kind::los);e.position=start;e.vector=d;out.push_back(e);
    return world.los(start,d);
}
}

AreaEffect construct_area_effect(std::uint64_t object,const ObjectWeakHandle& owner,const RootVector& position,
    const WeaponRecord& record,std::int32_t record_id,bool flag,AreaWorld& world,std::vector<AreaEvent>& out){
    AreaEffect a;
    a.object=object;
    a.owner_84=owner.object?owner:ObjectWeakHandle{0,-1};                  // 0025a384..0025a3b0
    a.position_70=position;a.record_80=record_id;a.done_d4=false;a.age_60=0.0F;a.flag_d5=flag;
    a.particles_d6=record.word(0xdc)!=0;                                    // 0025a484
    const auto type=record.damage_type_bc();
    if(type==3)out.push_back(event(AreaEvent::Kind::light));                // 0025a4a4..0025a534
    if(type==5||type==6||type==7){                                          // 0025a570..0025a828
        const float radius=check(record.real(0xc8));
        for(std::size_t k=0;k<4;++k){
            const auto d=vscale(directions[k],radius);                      // vmulx.xyzw
            const auto hit=traced(world,position,d,out);
            a.extents_90[k]=hit?vadd(position,vscale(vsub(*hit,position),extent_fraction)):vadd(position,d);
        }
    }
    return a;
}

bool area_gas_exposure(const AreaEffect& a,std::uint64_t object,bool not_gasable,AreaWorld& world,std::vector<AreaEvent>& out){
    if(not_gasable)return false;                                            // 002596fc
    for(std::size_t k=0;k<5;++k){
        RootVector source=k==0?a.position_70:a.extents_90[k-1];
        auto n=event(AreaEvent::Kind::node);n.object=object;n.value=exposure_node;out.push_back(n);
        const auto node=world.node_position(object,exposure_node);
        source[2]=node[2];                                                  // 002597c8
        if(!traced(world,source,vsub(node,source),out))return true;
    }
    return false;
}

void update_area_effect(AreaEffect& a,float dt,const WeaponRecord& record,AreaWorld& world,std::vector<AreaEvent>& out){
    check(dt);
    const auto type=record.damage_type_bc();
    float g=1.0F;
    if(!a.flag_d5&&a.age_60<growth_time&&type!=4)g=div(a.age_60,growth_time);   // 002598bc..002598fc
    const float half_height=div(mul(band_height,add(1.0F,g)),2.0F);        // f22
    const float r=mul(check(record.real(0xc8)),g);
    const float r2=mul(r,r);                                                // f21
    const std::uint64_t owner=a.owner_84.object&&world.valid(a.owner_84)?a.owner_84.object:0;   // s2
    for(const auto& h:world.objects()){
        if(h.object==0||!world.valid(h))continue;
        const auto t=world.target(h.object);
        if(t.life_108==3&&type!=4)continue;                                 // 00259a40
        const bool own=t.controller_player_54.value_or(false)||owner==0?t.byte_14:world.owner(owner).byte_14;
        if(!own)continue;
        if(!t.human_4c)continue;
        const auto d=vsub(t.position_e4,a.position_70);
        const float d2=add(mul(d[0],d[0]),mul(d[1],d[1]));
        if(!(d2<r2))continue;
        if(!(std::fabs(d[2])<half_height))continue;
        auto statistic=[&](std::int32_t id){
            if(owner==0)return;
            const auto o=world.owner(owner);
            if(o.controller_player_54.value_or(false)&&o.byte_14&&!t.controller_player_54.value_or(false)){
                auto s=event(AreaEvent::Kind::statistic);s.value=id;out.push_back(s);
            }
        };
        if(type==6){                                                        // 00259fe8: tear gas
            if(!area_gas_exposure(a,h.object,t.not_gasable_346b,world,out))continue;
            auto e=event(AreaEvent::Kind::tear_gas);e.object=h.object;e.value=6;out.push_back(e);
            world.tear_gas(h.object);
        }else if(type==5){                                                  // 00259eb8: lethal gas
            if(t.invulnerable_33a4||t.life_108==3||t.life_108==2)continue;
            if(!area_gas_exposure(a,h.object,t.not_gasable_346b,world,out))continue;
            statistic(4);
            const auto category=static_cast<std::uint8_t>(2);
            const auto damage_type=static_cast<std::uint8_t>(static_cast<std::int8_t>(record.damage_type_bc()));   // lb +bc
            auto e=event(AreaEvent::Kind::damage);e.object=h.object;e.owner=owner;e.value=category;e.type=damage_type;e.amount=1.0F;out.push_back(e);
            world.damage(h.object,owner,category,damage_type,1.0F);
        }else if(type==7){                                                  // 00259db8: knockout gas
            if(t.invulnerable_33a4||t.killed_33a5)continue;
            if(!area_gas_exposure(a,h.object,t.not_gasable_346b,world,out))continue;
            statistic(6);
            auto e=event(AreaEvent::Kind::damage);e.object=h.object;e.owner=owner;e.value=1;e.type=7;e.amount=0.0F;out.push_back(e);
            world.damage(h.object,owner,1,7,0.0F);
        }else if(type==3){                                                  // 00259c00: flash
            const float fraction=div(sub(r2,d2),r2);                        // f20
            auto f=event(AreaEvent::Kind::flash);f.object=h.object;f.amount=mul(flash_scale,fraction);out.push_back(f);
            world.flash(h.object,f.amount);
            // The blast point in the object's frame (rigid inverse of +f4: 0013ee20, 0013edb0, 0013ed90, 0013ed60, 00131e70).
            ProjectileQueryMatrix m{};
            for(std::size_t i=0;i<16;++i)m[i]=t.matrix_f4[i/4][i%4];
            const auto inv=prepare_projectile_query_frame(m);
            RootVector local{};
            for(std::size_t i=0;i<4;++i)
                local[i]=add(add(add(mul(inv[i],a.position_70[0]),mul(inv[4+i],a.position_70[1])),mul(inv[8+i],a.position_70[2])),mul(inv[12+i],a.position_70[3]));
            if(local[1]<=0.0F)continue;                                     // 00259cd4
            const float yaw=check(game_atanf(div(local[0],local[1])));      // 001212e0
            const float pitch=check(game_atanf(div(local[2],local[1])));
            if(!(std::fabs(yaw)<view_limit))continue;
            if(!(std::fabs(pitch)<view_limit))continue;
            float amount=mul(blind_duration,fraction);
            if(t.night_vision_3470)amount=mul(amount,blind_nv_factor);
            amount=sub(amount,mul(div(amount,stealth_scale),game_int_to_float(static_cast<std::int32_t>(100U-static_cast<std::uint32_t>(t.stealth_44)))));
            auto b=event(AreaEvent::Kind::blind);b.object=h.object;b.amount=amount;out.push_back(b);
            world.blind(h.object,amount);
        }else if(type==4){                                                  // 00259bb0: fire
            if(t.fire_immune_33b8||t.invulnerable_33a4||t.burning_326a)continue;
            auto e=event(AreaEvent::Kind::ignite);e.object=h.object;e.owner=owner;out.push_back(e);
            world.ignite(h.object,owner);
        }
    }
    const float duration=check(record.real(0xd0));
    if(type==3)a.age_60=duration;                                           // 0025a07c: a one-frame flash
    a.age_60=add(a.age_60,dt);
    if(!(a.age_60<=duration)&&!a.done_d4){                                  // 0025a0a8
        a.done_d4=true;
        out.push_back(event(AreaEvent::Kind::unregister));
        auto e=event(AreaEvent::Kind::retire);e.amount=retire_delay;out.push_back(e);
    }
}

bool finish_area_effect(AreaEffect& a,std::vector<AreaEvent>& out){
    if(a.done_d4)return false;                                              // 0025a160
    a.done_d4=true;                                                         // 0025a174
    out.push_back(event(AreaEvent::Kind::unregister));                      // 0025a170: 0040c9a0
    auto e=event(AreaEvent::Kind::retire);e.amount=retire_delay;out.push_back(e);   // 0025a184: 003cb230(area, 0.1)
    return true;
}

void apply_actor_flash(ActorSenses& s,float amount){s.flash_33ac=check(amount);}

bool apply_actor_blind(ActorSenses& s,float amount,bool byte_14,bool controller_player){
    s.blind_33a8=check(amount);
    return byte_14&&controller_player;
}

void decay_actor_senses(ActorSenses& s,float dt){
    check(dt);
    for(float* t:{&s.blind_33a8,&s.flash_33ac}){
        if(*t<=0.0F)continue;
        const float v=sub(*t,dt);
        *t=v<0.0F?0.0F:v;
    }
}

}
