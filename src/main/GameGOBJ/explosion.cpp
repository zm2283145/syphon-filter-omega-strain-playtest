#include "explosion.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Explosion: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float div(float a,float b){
    if(b==0.0F)throw std::runtime_error("Explosion: division by zero");
    return check(vu::div(a,b));
}
// cvt.w.s (truncation); values outside the integer range are not native.
std::int32_t trunc_word(float v){
    check(v);
    if(!(std::fabs(v)<2147483648.0F))throw std::runtime_error("Explosion: value outside the integer range");
    return static_cast<std::int32_t>(v);
}
constexpr float flash_inner_scale=1.0F;     // 0048a9a0
constexpr float flash_outer_scale=2.0F;     // 0048a9a8
constexpr float shock_speed=150.0F;         // 0048a9b0
constexpr float milliseconds=1000.0F;       // 0021a600 (lui 0x447a)
}

BlastResult prepare_blast(const BlastRequest& r,std::span<const BlastObject> registry){
    check(r.strength);check(r.inner);check(r.outer);
    BlastResult result;
    RootVector P{0.0F,0.0F,0.0F,1.0F};                                       // 0021a300..0021a30c
    if(r.point)P=*r.point;                                                   // 0021a318: the whole quadword
    else if(r.source)P=r.source->position_e4;                                // 0021a330: source +e4
    for(const float v:P)check(v);
    result.flash={P,mul(r.inner,flash_inner_scale),mul(r.outer,flash_outer_scale)};   // 0021a380..0021a384
    const bool source_owned=!r.source||r.source->byte_14;                    // 0021a3b8..0021a3dc (fp)
    const ObjectWeakHandle source=r.source?r.source->handle:ObjectWeakHandle{};
    for(const auto& o:registry){
        if(r.skip_humans){                                                   // 0021a3e4: reads +4c first
            if(o.object==0)throw std::runtime_error("Explosion: skip-humans reads +4c of a null registry entry");
            if(o.human_4c)continue;
        }
        if(o.object==0)continue;                                             // 0021a408
        if(!(o.byte_2d&&o.byte_2c&&!o.byte_2e))continue;                     // 0021a410..0021a450
        const bool owned=o.controller_player_54.value_or(false)?o.byte_14:source_owned;   // 0021a458..0021a4a0
        if(!owned)continue;
        if(trunc_word(o.health_40)<=0)continue;                              // 0021a4a8 (solo: 005721c8 clear)
        RootVector v{};
        for(std::size_t i=0;i<4;++i)v[i]=sub(o.position_e4[i],P[i]);          // vsub.xyzw
        // vmul.xyz, then ACC = x (vf0.w * x), + y, + z; VU SQRT.
        const float squared=add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));
        const float d=check(vu::sqrt(squared));
        if(!(d<r.outer))continue;                                            // 0021a564 c.olt.s
        float scale=0.0F,amount=0.0F;
        const float damage=game_int_to_float(r.damage);                      // cvt.s.w
        if(d<r.inner){scale=r.strength;amount=damage;}                       // 0021a570
        else{
            const float t=sub(1.0F,div(sub(d,r.inner),sub(r.outer,r.inner)));
            scale=mul(r.strength,t);amount=mul(damage,t);
        }
        std::int8_t type=0;                                                  // 0021a5c8 c.ole.s: <= 0 gives 0
        if(!(scale<=0.0F))type=r.kind?2:13;
        ExplosionEvent e;
        e.delay_ms_0=trunc_word(mul(milliseconds,div(d,shock_speed)));       // 0021a60c..0021a624, cvt.w.s
        e.game_clock_4=true;e.flush_5=false;                                 // 0021a820(event, delay, 1, 0)
        e.victim_c={o.object,o.id_c};                                        // 00176ef0
        e.damage_14=trunc_word(amount);                                      // 0021a6d8
        e.source_18=source;
        e.type_20=type;
        e.point_30=P;                                                        // 001325e0
        result.events.push_back(e);
    }
    return result;
}

std::optional<ExplosionDamageCall> explosion_damage_call(const ExplosionEvent& e,
    const std::function<bool(const ObjectWeakHandle&)>& valid){
    if(!valid)throw std::runtime_error("Explosion event: no weak-handle check");
    if(e.victim_c.object==0||!valid(e.victim_c))return std::nullopt;         // 0021b89c..0021b8dc
    ExplosionDamageCall call;
    call.victim=e.victim_c.object;
    call.source=e.source_18.object!=0&&valid(e.source_18)?e.source_18.object:0;   // 0021b8e4..0021b910
    call.category=1;call.type=e.type_20;call.point=e.point_30;               // a2 = 1, lb +20, t2 = +30
    call.amount=game_int_to_float(e.damage_14);                              // cvt.s.w
    return call;
}

ActorDamageInput blast_actor_damage_input(const ExplosionDamageCall& call,std::uint64_t actor,
    std::optional<ActorDamageController> controller,bool byte_14,std::optional<ActorDamageController> source_controller,
    std::optional<ActorDamageParty> fire_setter_3480){
    if(call.victim!=actor)throw std::runtime_error("Explosion damage: call for another victim");
    if(call.type<0)throw std::runtime_error("Explosion damage: negative type byte");
    ActorDamageInput in;
    in.actor=actor;in.controller=controller;in.byte_14=byte_14;
    in.source={call.source,call.source?source_controller:std::nullopt};
    in.fire_setter_3480=fire_setter_3480;
    in.category=call.category;in.type=static_cast<std::uint8_t>(call.type);
    in.penetration=std::nullopt;in.quiet=false;                              // t0 = null, t1 = 0
    in.hit_point=call.point;in.amount=call.amount;
    return in;
}

ExplosionWorld::ExplosionWorld(BlastObjects& objects,ScriptScheduler& events):objects_(objects),events_(events){}

BlastResult ExplosionWorld::blast(const BlastRequest& request){
    const auto registry=objects_.registry();
    auto result=prepare_blast(request,registry);
    flashes_.push_back(result.flash);
    for(const auto& e:result.events)                                         // 003ea1d0 (0055a2f8 is 0: queued)
        events_.enqueue(e.delay_ms_0,[this,e]{
            ++fired_;
            const auto call=explosion_damage_call(e,[this](const ObjectWeakHandle& h){return objects_.valid(h);});
            if(call)objects_.damage(*call);
        });
    return result;
}

BlastResult ExplosionWorld::create_blast(std::uint64_t object,std::int32_t damage,float strength,float inner,float outer){
    BlastRequest r;
    if(object!=0)r.source=objects_.source(object);
    r.damage=damage;r.kind=true;r.point=std::nullopt;r.skip_humans=false;
    r.strength=strength;r.inner=inner;r.outer=outer;
    return blast(r);
}

}
