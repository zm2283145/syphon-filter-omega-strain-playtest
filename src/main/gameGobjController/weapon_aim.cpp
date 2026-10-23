#include "weapon_aim.hpp"
#include "water_effects.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
// VU vmul + vmulax/vmadday/vmaddz: ((x*x + y*y) + z*z), then vsqrt.
float length3(const RootVector& v){
    const float s=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];
    return std::sqrt(s);
}
float clamp01(float v){return std::min(std::max(v,0.0F),1.0F);}
void store_target(WeaponAimState& s,const WeaponAimInput& in){
    s.target_28=in.target?in.target->object:0;
    s.target_id_2c=in.target?in.target->id_c:-1;
    s.subtarget_3c=in.subtarget;
}
}

std::optional<WeaponAimNotice> update_weapon_aim(WeaponAimState& state,const WeaponAimInput& in){
    const auto& t=in.settings;
    // 0022f660..0022f75c: +40 = now; dt = float(now - previous) / 1000.
    const auto elapsed=static_cast<std::int32_t>(in.now_ms-state.time_40);
    state.time_40=in.now_ms;
    const float dt=game_int_to_float(elapsed)/1000.0F;
    bool visible=in.target&&!in.target_manager_22cb10;   // s6
    auto reset=[&](bool notice)->std::optional<WeaponAimNotice>{
        state.accuracy_24=0;state.progress_20=0;store_target(state,in);
        (void)notice;return std::nullopt;
    };
    if(!in.record)return reset(false);
    const auto& rec=*in.record;
    float multiplier=1.0F;bool ignore_size=false;
    if(in.shooter.actor){
        if(!(in.shooter.blinded_33a8<=0.0F))visible=false;   // c.ole.s: NaN also blinds
        if(in.shooter.player_54)multiplier=in.shooter.accuracy_multiplier;
        ignore_size=in.shooter.ignore_size_3470;
    }
    const bool aiming=in.shooter.ready_d4||!in.shooter.player_54||rec.hold_class_f0()==7;
    if(!in.target)return reset(false);
    const auto& target=*in.target;
    const bool same=state.target_28==target.object&&state.target_id_2c==target.id_c&&state.subtarget_3c==in.subtarget;
    if(!(same&&visible&&aiming))return reset(false);
    // From here on the notice step runs at the end.
    auto finish=[&](float progress,float accuracy)->std::optional<WeaponAimNotice>{
        state.progress_20=progress;state.accuracy_24=accuracy;store_target(state,in);
        if(visible&&target.controller&&target.controller->player_54&&target.controller->owned_14&&in.subtarget==0)
            return WeaponAimNotice{target.object,static_cast<std::int32_t>(progress*game_int_to_float(rec.damage_mid_64())),in.subtarget,progress};
        return std::nullopt;
    };
    RootVector diff{};
    for(std::size_t k=0;k<4;++k)diff[k]=target.eye_c0[k]-in.shooter.eye_c0[k];
    const float d=length3(diff);
    const float near=game_int_to_float(rec.near_34()),mid=game_int_to_float(rec.mid_38()),far=game_int_to_float(rec.far_3c());
    if(!(d<=far))return finish(0.0F,0.0F);
    float progress=state.progress_20;
    const float an=game_int_to_float(rec.accuracy_near_40())/100.0F,am=game_int_to_float(rec.accuracy_mid_44())/100.0F,
        af=game_int_to_float(rec.accuracy_far_4c())/100.0F;
    const float tn=rec.aim_time_near_50(),tf=rec.aim_time_far_54();
    float accuracy,aim_time;
    if(d<near){accuracy=an;aim_time=tn;}
    else{
        const float f2=d-near;
        if(d<mid)accuracy=an+(f2*(am-an))/(mid-near);
        else accuracy=am+((d-mid)*(af-am))/(far-mid);
        aim_time=tn+(f2*(tf-tn))/(far-near);
    }
    float k=1.0F+game_int_to_float(target.stealth_44)/100.0F;k=k*k;
    if(!ignore_size){aim_time=aim_time*k;accuracy=accuracy/k;}
    if(in.shooter.player_54)aim_time=aim_time*1.0F;     // 0048acc8
    else aim_time=aim_time+t.npc_extra_aim_time;
    const float rate_scale=aim_time!=0.0F?1.0F/aim_time:10.0F;
    float rate=dt*rate_scale;
    const auto stance=in.shooter.stance_c8;
    if(stance==4){rate=rate*t.kneel_aim_factor;accuracy=accuracy*t.kneel_acc_factor;}
    else if(stance==2){accuracy=0;progress=0;rate=0;}
    if(in.shooter.velocity_74){
        const float f=length3(*in.shooter.velocity_74)/t.max_velocity;
        rate=rate*std::max(1.0F-(1.0F-t.run_aim_factor)*f,0.0F);
        accuracy=accuracy*std::max(1.0F-(1.0F-t.run_acc_factor)*f,0.0F);
    }
    float skill=state.skill_38;
    if(!in.shooter.player_54){
        skill=skill*1.0F;                                 // 0048acc0
        if(d<t.npc_near_dist)skill=skill*t.npc_near_mul;
    }
    rate=rate*(skill*target.size_cc);
    float penalty=0;
    if(target.stance_c8==2){
        accuracy=accuracy*t.roll_targ_acc_factor;
        if(!in.shooter.player_54)penalty=penalty+t.npc_rolling_targ_val;
    }
    if(target.stance_c8==3||target.stance_c8==4)accuracy=accuracy*t.crouch_targ_acc_factor;
    if(target.controller&&target.controller->velocity_74){
        RootVector n{-diff[1],diff[0],diff[2],0};
        const float length=length3(n);
        if(length==0.0F)throw std::runtime_error("Weapon aim: zero eye-to-eye direction with a moving target (division by zero)");
        const float inverse=1.0F/length;
        for(std::size_t i=0;i<3;++i)n[i]=n[i]*inverse;
        const auto& v=*target.controller->velocity_74;
        const float dot=(v[0]*n[0]+v[1]*n[1])+v[2]*n[2];
        const float lateral=std::fabs(dot)/t.max_velocity;
        const float q=game_int_to_float(static_cast<std::int32_t>(5.0F*lateral))/5.0F;
        accuracy=accuracy*std::max(1.0F-(1.0F-t.moving_targ_acc_factor)*q,0.0F);
        if(!in.shooter.player_54)penalty=penalty+t.npc_moving_targ_val*q;
    }
    accuracy=clamp01((accuracy*multiplier)*skill);
    if(in.shooter.player_54){
        if(state.accuracy_24<accuracy)progress=(state.accuracy_24*state.progress_20)/accuracy;
    }else{
        if(!in.shooter.ready_d4)penalty=penalty+t.reload_aim_loss;
        rate=rate-penalty*rate_scale;
    }
    progress=clamp01(progress+rate);
    return finish(progress,accuracy);
}

bool weapon_hit_roll(const WeaponAimState& state,bool player,const std::function<std::uint32_t()>& random){
    if(!player&&state.progress_20<0.9F)return false;      // 00493988
    const float chance=state.progress_20*state.accuracy_24;
    if(!random)throw std::runtime_error("Weapon hit roll: no game RNG");
    const float draw=game_int_to_float(static_cast<std::int32_t>(random()))/2147483648.0F;
    return draw<chance;
}

}
