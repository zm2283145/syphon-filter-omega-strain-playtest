#include "actor_melee.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Actor melee: "+what);}
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)fail(std::string("host ")+what+" is not hosted");
    return f;
}
constexpr float reach=std::bit_cast<float>(0x40b00000U);              // 00489d90 (5.5)
constexpr float vertical=std::bit_cast<float>(0x40000000U);           // 00489d88 (2.0)
constexpr float facing=std::bit_cast<float>(0x3f8ccccdU);             // 00489d80 (1.1)
constexpr float behind=std::bit_cast<float>(0x40000000U);             // 00489d98 (2.0)
constexpr float knife_phase=std::bit_cast<float>(0x3ecccccdU);        // 0048a268 (0.4)
constexpr float unarmed_phase=std::bit_cast<float>(0x3ecccccdU);      // 0048a278 (0.4)
constexpr float blend=std::bit_cast<float>(0x3f000000U);              // 0049d038 (0.5)
constexpr float victim_blend=std::bit_cast<float>(0x3c23d70aU);       // 00196b78 (0.01)
constexpr float victim_rate=std::bit_cast<float>(0x42c80000U);        // 00196b84 (100.0)
constexpr float takedown_damage=std::bit_cast<float>(0x461c4000U);    // 00196cfc (10000.0)

// {w, w, 1/w, 1/w} with the FLT_MAX rate for a zero width (c.eq.s).
ActorMotionTransition transition_of(float w){
    const float rate=w==0.0F?std::numeric_limits<float>::max():check(1.0F/w);
    return {w,w,rate,rate};
}
// The request block: 0018a910 tokens {-1, 0}, 0018a8c0 the hook vector, byte
// +0 set only by the token stores; 003ad500 with f12 = -1, f13 = 0.
MotionRequest request_of(const ActorMotionRegistration& r,const ActorMotionTransition& t){
    MotionRequest q;q.node=*r.node;q.selector=r.selector;
    q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;
    q.limit=-1;q.initial_phase=0;
    return q;
}
// 001cae20's name: the bounded 47-character copy into a 48-byte buffer.
std::array<std::uint8_t,48> name48(std::string_view s){
    std::array<std::uint8_t,48> out{};
    for(std::size_t k=0;k<47&&k<s.size()&&s[k]!='\0';++k)out[k]=static_cast<std::uint8_t>(s[k]);
    return out;
}
// The VU product M x p (vmulax / vmadday / vmaddaz / vmaddw).
RootVector transform(const TargetingMatrix& m,const RootVector& p){
    RootVector r{};
    for(std::size_t k=0;k<4;++k)r[k]=check(check(check(check(m[0][k]*p[0])+check(m[1][k]*p[1]))+check(m[2][k]*p[2]))+check(m[3][k]*p[3]));
    return r;
}
}

TargetAngles melee_relative_angles(const TargetingMatrix& matrix_f4,const RootVector& eye_c0,const RootVector& point_c0){
    TargetingMatrix frame=matrix_f4;
    frame[3]=eye_c0;
    return target_angles(frame,point_c0);
}

MeleeTarget choose_melee_target(const MeleeTargetHost& h){
    const auto& position=need(h.position_e4,"position_e4 (virtual +e4)");
    const auto relative=[&](std::uint64_t a,std::uint64_t b){        // 0022ab20(a, b, out)
        const auto m=need(h.matrix_f4,"matrix_f4 (virtual +f4)")(a);
        const auto& eye=need(h.eye_c0,"eye_c0 (virtual +c0)");
        const RootVector from=eye(a);
        const RootVector at=eye(b);
        return melee_relative_angles(m,from,at);
    };
    const RootVector origin=position(h.attacker);
    float best=check(reach*reach);
    std::uint64_t chosen=0;
    for(const auto& node:h.candidates){
        if(!node.object)continue;
        if(need(h.object_id,"object_id (+c)")(node.object)!=node.id)continue;
        if(node.object==h.attacker)continue;
        if(need(h.player,"player (controller +58 / +54)")(node.object))continue;
        const auto life=need(h.life_state,"life_state (virtual +108)")(node.object);
        if(life==2||life==3)continue;
        if(!need(h.actor_class_4c,"actor_class_4c (+4c)")(node.object))continue;
        const RootVector p=position(node.object);
        const float dx=check(p[0]-origin[0]),dy=check(p[1]-origin[1]),dz=check(p[2]-origin[2]);
        const float d2=check(check(dx*dx)+check(dy*dy));
        if(!(d2<best))continue;                                          // c.olt.s
        if(!(std::fabs(dz)<vertical))continue;
        if(!(std::fabs(relative(h.attacker,node.object).angles[2])<facing))continue;
        best=d2;chosen=node.object;
    }
    MeleeTarget out;out.target=chosen;
    if(!chosen)return out;
    if(need(h.invulnerable_33a4,"invulnerable_33a4 (001b8740)")(chosen))return out;
    if(std::fabs(relative(chosen,h.attacker).angles[2])<=behind)return out;   // c.ole.s
    const auto v=need(h.velocity_310,"velocity_310 (physical +310)")(chosen).value_or(RootVector{});   // static 004e1d90
    const float x=check(v[0]*v[0]),y=check(v[1]*v[1]),z=check(v[2]*v[2]);
    const float speed=vu::sqrt(check(check(x+y)+z));
    if(speed<1.0F)out.takedown=true;
    return out;
}

MeleeAttackResult start_melee_attack(ActorAnimationReceiverState& a,const MeleeAttackInput& in,const MeleeAttackHost& h){
    MeleeAttackResult out;
    const std::uint32_t cls=in.hold_class&0xffU;
    const bool baton=cls==4||cls==10,unarmed=cls==11;
    const float phase=unarmed?unarmed_phase:knife_phase;
    const std::int32_t token=unarmed?0x99:0x98;
    if(a.byte_33e8)return out;
    if(a.byte_14&&!(in.guard_24<=0.0F))return out;                     // c.ole.s: a NaN returns
    out.started=true;
    const std::uint32_t target_word=in.target?static_cast<std::uint32_t>(in.target_id):0xffffffffU;
    bool takedown=!baton&&in.takedown&&in.target;
    if(takedown){
        const auto life=need(h.life_state,"life_state (target virtual +108)")(in.target);
        takedown=life!=2&&life!=3;
    }
    if(takedown){
        out.takedown=true;
        const char* first=unarmed?"snapping_neck_pt1":"throat_cut_pt1";
        const char* victim=unarmed?"animNeckSnap":"animThroatSlit";
        const char* second=unarmed?"snapping_neck_pt2":"throat_cut_pt2";
        if(in.attacker_physical)need(h.clear_physical_3d0,"clear_physical_3d0 (physical +3d0)")(in.actor);
        if(in.target_physical)need(h.clear_physical_3d0,"clear_physical_3d0 (physical +3d0)")(in.target);
        const auto& resolve=need(h.resolve,"resolve (0018c5a0)");
        const auto& install=need(h.install,"install (003ad500)");
        // The attacker's part 1: tokens 0x94 / 0x95 / 0x96 / 0x97 at slots
        // 0 / 3 / 5 / 6, the phase hook [0.4, 0.4] (00192750 flag 0) with
        // token 0x98 (knife) / 0x99 (unarmed) by 00190f90, mode 3.
        if(const auto r=resolve(in.actor,first);r.node){
            auto q=request_of(r,transition_of(blend));
            q.notifications_active=true;
            const std::array<std::int32_t,7> ids{0x94,-1,-1,0x95,-1,0x96,0x97};
            for(std::size_t k=0;k<7;++k)if(ids[k]>=0)q.notifications[k]={ids[k],in.actor};
            const auto max_s=[](float x,float y){return x<y?y:x;};
            const auto min_s=[](float x,float y){return y<x?y:x;};
            const float hi=min_s(max_s(phase,phase),1.0F);                   // 00192750: max.s(hi, lo), min.s(1)
            const float lo=min_s(max_s(phase,0.0F),hi);                      // max.s(lo, 0), min.s(hi)
            q.phase_hooks.push_back({lo,hi,{token,in.actor}});
            install(in.actor,q,MotionInstallMode::reset);
        }
        if(in.attacker_physical)need(h.physical_reset_11,"physical_reset_11 (physical +11)")(in.actor);
        // The retained motion +326c, queued (no null test in the original).
        const auto kept=need(h.retained,"retained (+326c)")(in.actor);
        if(!kept.node)fail("the retained motion (+326c) is unresolved");
        install(in.actor,request_of(kept,transition_of(blend)),MotionInstallMode::queue);
        if(in.target){
            auto m=need(h.matrix_f4,"matrix_f4 (actor virtual +f4)")(in.actor);
            const auto& off=unarmed?in.unarmed_offset:in.knife_offset;
            m[3]=transform(m,{off[0],off[1],0.0F,1.0F});
            need(h.teleport,"teleport (message 0055d4e0)")(in.target,m);
            if(const auto r=resolve(in.target,victim);r.node){
                const ActorMotionTransition t{victim_blend,victim_blend,victim_rate,victim_rate};
                install(in.target,request_of(r,t),MotionInstallMode::replace);
            }
            need(h.death_motion,"death_motion (0018c9d0)")(in.target,second,1);
            need(h.damage,"damage (target virtual +100)")(in.target,in.actor,2,unarmed?10:9,takedown_damage);
        }
        if(a.byte_14&&need(h.player,"player (controller +58 / +54)")(in.actor))
            need(h.push_world_object,"push_world_object (00131540)")(in.actor);
        a.byte_33e8=1;a.target_33cc=target_word;
    }else if(!unarmed&&in.secondary){
        SecondaryOneShot event;                                          // 0018a300 on 00542b60, vtable 004da930
        event.code=4;event.action=0;event.receiver=in.actor;event.posted=0;event.value=0.0F;
        const bool accepted=need(h.one_shot,"one_shot (001cae20)")(name48(baton?"stun_baton":"knife_stabbing"),3,event);
        out.one_shot=accepted;
        if(accepted){
            if(baton||!in.target){
                const auto p=need(h.position_e4,"position_e4 (actor virtual +e4)")(in.actor);
                need(h.sound,"sound (0020f860)")(baton?melee_baton_sound:melee_knife_miss_sound,p);
            }
            a.byte_33e8=1;a.target_33cc=target_word;
        }
    }
    if(a.byte_33e8==1&&a.byte_14){
        need(h.replicate,"replicate (message 004eeee8)")(in.actor,in.target,in.takedown);
        out.replicated=true;
    }
    return out;
}

MeleeHitResult apply_melee_hit(const MeleeHitInput& in,const MeleeHitHost& h){
    MeleeHitResult out;
    using Outcome=MeleeHitResult::Outcome;
    if(in.mode_84!=5)return out;                                                       // 001418b0
    const auto record=need(h.record,"record (00147840)")(in.item_58);
    const auto cls=record.hold_class_f0;
    if(cls!=3&&cls!=4&&cls!=10)return out;                                             // 001418d8..001418ec
    if(in.knife_damage==0)return out;                                                  // doKnifeDmg 00489da0
    if(need(h.player,"player (controller +58 / +54)")(in.target))return out;           // 00141908..00141930
    const auto& position=need(h.position_e4,"position_e4 (virtual +e4)");
    const RootVector origin=position(in.attacker);                                     // sp+190
    const RootVector at=position(in.target);                                           // sp+a0
    RootVector d{};                                                                    // vsub.xyzw, sp+180
    for(std::size_t k=0;k<4;++k)d[k]=check(at[k]-origin[k]);
    if(need(h.blocked,"blocked (003caa50 / 00148290)")(origin,d,melee_hit_los_mask))return out;
    const std::uint8_t type=cls==4?7:9;                                                // s1
    std::int32_t penetration=record.penetration_f8;                                    // sp+1ac
    const auto refused=[&]{
        const auto life=need(h.life_state,"life_state (virtual +108)")(in.target);
        return life==2||life==3||!need(h.actor_class_4c,"actor_class_4c (+4c)")(in.target);
    };
    if(cls==4){                                                                        // 00141a58
        if(refused()||need(h.stun_immune_346a,"stun_immune_346a (+346a)")(in.target)){out.outcome=Outcome::refused;return out;}
        if(need(h.player,"player (controller +58 / +54)")(in.attacker)&&need(h.byte_14,"byte_14 (+14)")(in.attacker)
            &&!need(h.invulnerable_33a4,"invulnerable_33a4 (001b8740)")(in.target)){
            need(h.statistic,"statistic (00333f10)")(melee_knockout_statistic,1);
            out.statistic=true;
        }
        need(h.stun,"stun (00184ce0)")(in.target,0,1,std::bit_cast<float>(0x3f000000U),std::bit_cast<float>(0x461c4000U));
        out.outcome=Outcome::stunned;
        return out;
    }
    if(cls==10){                                                                       // 00141b64
        if(refused()){out.outcome=Outcome::refused;return out;}
        need(h.ignite,"ignite (00186360)")(in.target,1,in.attacker,0,0,std::bit_cast<float>(0xbf800000U));
        out.outcome=Outcome::ignited;
        return out;
    }
    float amount=game_int_to_float(record.damage_64);                                  // 00141be4 (cvt.s.w)
    std::uint8_t category=3;
    if(in.forced){
        amount=std::bit_cast<float>(0x47c35000U);                                      // 100000.0
        category=2;
    }else if(need(h.actor_class_4c,"actor_class_4c (+4c)")(in.target)){
        // (x*x + y*y) + z*z (vmulax / vmadday / vmaddz), Q = 1 / sqrt, xyz * Q;
        // w = 1.0 (sp+18c).
        const float w=check(check(check(d[0]*d[0])+check(d[1]*d[1]))+check(d[2]*d[2]));
        const float q=check(vu::rsqrt(w));
        RootVector dir{check(d[0]*q),check(d[1]*q),check(d[2]*q),1.0F};
        need(h.hit_volumes,"hit_volumes (00188140)")(in.target,origin,dir,2,1.0F);
        out.hit_volumes=true;
    }
    need(h.damage,"damage (virtual +100)")(in.target,in.attacker,category,type,penetration,amount);
    out.outcome=Outcome::damaged;
    return out;
}

}
