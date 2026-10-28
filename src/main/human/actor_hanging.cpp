#include "actor_hanging.hpp"
#include "actor_ledge_action.hpp"
#include "actor_running_jump.hpp"
#include "game_libm.hpp"
#include "projectile_launch.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor hanging: nonfinite arithmetic");
    return flush(v);
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float maximum(float a,float b){return a<b?b:a;}                   // max.s
float minimum(float a,float b){return b<a?b:a;}                   // min.s
RootVector add4(const RootVector& a,const RootVector& b){return {add(a[0],b[0]),add(a[1],b[1]),add(a[2],b[2]),add(a[3],b[3])};}
RootVector sub4(const RootVector& a,const RootVector& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}

// Transition {d, d, 1/d, 1/d} as built by 0018a380(d) or the explicit stores
// (FLT_MAX rate for a zero duration).
ActorMotionTransition transition_of(float d){
    const float rate=d==0.0F?std::numeric_limits<float>::max():check(1.0F/d);
    return {d,d,rate,rate};
}
// Token block (0018a910 + explicit stores): ids at +4 + 8k for the listed k,
// receiver = this actor; the others stay {-1, 0}.
struct Tokens {std::array<std::int32_t,7> ids;};
MotionRequest request_of(const ActorMotionRegistration& r,const ActorMotionTransition& t,
    const std::optional<Tokens>& tokens,std::uint64_t receiver){
    MotionRequest q;q.node=*r.node;q.selector=r.selector;
    q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;
    q.limit=-1;q.initial_phase=0;                       // f12 = -1, f13 = 0
    if(tokens){
        q.notifications_active=true;
        for(std::size_t k=0;k<7;++k)if(tokens->ids[k]>=0)q.notifications[k]={tokens->ids[k],receiver};
    }
    return q;
}
void require_host(const ActorHangMotionHost& h,bool retained=false){
    if(!h.resolve||!h.install||(retained&&!h.retained))throw std::runtime_error("Actor hanging: motion host required");
}
// 001926d0/001926c0 on model +0x44 (named roots), used by 001a1590 and 001a0b30.
void require_root_host(const ActorHangMotionHost& h,bool retained=false){
    if(!h.resolve_root||!h.install||(retained&&!h.retained))throw std::runtime_error("Actor hanging: named-root motion host required");
}
// 0018c5a0 name lookup, then 003ad500 when it resolves.
void install(std::vector<ActorHangInstall>& out,const ActorHangMotionHost& h,const char* name,MotionInstallMode mode,
    const ActorMotionTransition& t,const std::optional<Tokens>& tokens,std::uint64_t receiver,
    std::vector<MotionPhaseHook> hooks={}){
    const auto r=h.resolve(name);
    if(!r.node)return;
    auto q=request_of(r,t,tokens,receiver);
    q.phase_hooks=std::move(hooks);
    out.push_back({name,mode});
    h.install(q,mode);
}
const float restore_374=bits(0x3fdeb852U);                            // 004a03e0 (1.74)
// 00189590(actor, 5): clear action 5; with a physical component +9c0 = 0 and
// +374 = 004a03e0. The other identifiers of the 0019f724 table (0x10, 0x11,
// 0x15, 0x1f) have no effect there.
void leave_air_action(ActorAnimationReceiverState& a,ActorJumpPhysical& p){
    set_actor_action(a,5,false);
    if(p.present){p.keep_contact_9c0=0;p.radius_374=restore_374;}
}
}

namespace {
// 001914c0 actions 0x15/0x16 (00191b30..00192698).
ActorHangResult handle_running_jump(ActorAnimationReceiverState& actions,ActorJumpState& jump,
    const ActorJumpGrabRequest& r,const ActorHangMotionHost& host){
    ActorHangResult result;
    if(r.kind!=4)return result;                                        // 00191b48 / 00191c8c
    for(float v:r.payload)check(v);
    require_root_host(host);
    if(!r.receiver)throw std::runtime_error("Actor running jump: receiver required");
    if(r.action==0x15){
        jump.kind_3490=1;jump.grab_34a0.position=r.payload;            // 00191c50: +34b0 as sent
    }else{
        // 00191c94: half = 0.5 w; +34a0 = (0, 0, 1, 0) * sinf(half) with W
        // cosf(half) (00121688 first, then 00121928); +34b0 = (xyz, 1).
        const float half=mul(0.5F,r.payload[3]);
        const float c=check(game_cosf(half)),sn=check(game_sinf(half));
        jump.kind_3490=2;
        jump.grab_34a0.orientation={mul(sn,0.0F),mul(sn,0.0F),mul(sn,1.0F),c};
        jump.grab_34a0.position={r.payload[0],r.payload[1],r.payload[2],1.0F};
    }
    set_actor_action(actions,4,true);                                  // 00191d94..00191dc0
    if(r.action==0x15){actions.flags_3248|=0x800U;actions.flags_3248&=~0x1000U;}    // 00191dd8
    else{actions.flags_3248|=0x1000U;actions.flags_3248&=~0x800U;}                  // 00191e04
    actions.flags_3248&=~0x400U;actions.flags_3248&=~0x2000U;          // 00191e20
    const bool right=r.lead_foot_31dd==1;                              // 00191e7c
    const char* takeoff=right?"blendRunningJumpTakeOff_Right":"blendRunningJumpTakeOff_Left";
    const char* in_air=right?"blendRunningJumpInAir_Right":"blendRunningJumpInAir_Left";
    const ActorMotionTransition quick{bits(0x3e4ccccdU),bits(0x3e4ccccdU),5.0F,5.0F};
    // 001921c0: both phase hooks at [0.8, 0.8] (max.s/min.s of 0.8 with [0, 1]).
    const float at=bits(0x3f4ccccdU);
    const auto lookup=[&](const char* name){
        const auto reg=host.resolve_root(name);                         // 001926d0/001926c0
        if(!reg.node)throw std::runtime_error(std::string("Actor running jump: named root ")+name+" is unresolved (the original does not check)");
        return reg;
    };
    auto q=request_of(lookup(takeoff),quick,Tokens{{9,10,-1,13,-1,14,15}},r.receiver);
    q.phase_hooks={{at,at,{0x0b,r.receiver}},{at,at,{0x0c,r.receiver}}};
    result.motions.push_back({takeoff,MotionInstallMode::replace});
    host.install(q,MotionInstallMode::replace);                        // 00192428 (a2 = 1)
    result.motions.push_back({in_air,MotionInstallMode::queue});
    host.install(request_of(lookup(in_air),quick,Tokens{{0x10,-1,-1,0x11,-1,0x12,0x13}},r.receiver),
        MotionInstallMode::queue);                                     // 00192638 (a2 = 0)
    result.handled=true;
    return result;
}
}

ActorHangResult handle_actor_jump_to_grab(ActorAnimationReceiverState& actions,ActorJumpState& jump,
    const ActorJumpGrabRequest& r,const ActorHangMotionHost& host){
    ActorHangResult result;
    if(r.life_state==2||r.life_state==3)return result;               // virtual +108
    if(actions.highest_action==4||actions.highest_action==5)return result;
    if(r.action==0x15||r.action==0x16)return handle_running_jump(actions,jump,r,host);
    if(r.action!=0x14||r.kind!=14)return result;
    require_host(host);
    if(!r.receiver)throw std::runtime_error("Actor jump to grab: receiver required");
    const float height=sub(check(r.edge.position[2]),check(r.floor_z));
    const char* name=height<10.0F?"jump_straight_up_for_grab_8_to_10_feet_longer":   // 001915a8: 0x41200000
        "jump_straight_up_for_grab_10_to_12_feet_longer";
    jump.kind_3490=2;jump.grab_34a0=r.edge;                            // 001916f8..00191728
    const ActorMotionTransition quick{bits(0x3e4ccccdU),bits(0x3e4ccccdU),5.0F,5.0F};
    // 00190f90 phase hooks: 0x0b at [0.5, 0.5] (max/min clamp at 0019180c),
    // 0x0c at [0.5, 0.7] (00192750).
    install(result.motions,host,name,MotionInstallMode::replace,quick,Tokens{{9,10,-1,13,-1,14,15}},r.receiver,
        {{0.5F,0.5F,{0x0b,r.receiver}},{0.5F,bits(0x3f333333U),{0x0c,r.receiver}}});
    install(result.motions,host,"jump_idle_in_air",MotionInstallMode::queue,quick,
        Tokens{{0x10,-1,-1,0x11,-1,0x12,0x13}},r.receiver);
    set_actor_action(actions,4,true);                                  // 00191ab4
    actions.flags_3248|=0x400U;
    actions.flags_3248&=~0x800U;actions.flags_3248&=~0x1000U;actions.flags_3248&=~0x2000U;
    result.handled=true;
    return result;
}

ActorLedgeCatchResult catch_actor_ledge(ActorAnimationReceiverState& actions,ActorJumpPhysical& physical,
    std::uint64_t receiver,const ActorHangMotionHost& host){
    ActorLedgeCatchResult result;
    if(bit(actions.active_actions,7)||bit(actions.active_actions,8))return result;
    require_host(host);
    if(!receiver)throw std::runtime_error("Actor ledge catch: receiver required");
    if(bit(actions.active_actions,5))leave_air_action(actions,physical); // 001a1cd0
    set_actor_action(actions,7,true);
    const float width=bit(actions.flags_3248,10)?0.25F:bits(0x3e99999aU);
    install(result.motions,host,"DATA_ID_JumpGrabLedge",MotionInstallMode::replace,transition_of(width),
        Tokens{{0x14,0x15,-1,0x16,-1,0x17,0x18}},receiver);
    install(result.motions,host,"sliderHang",MotionInstallMode::queue,transition_of(0.5F),
        Tokens{{0x19,-1,-1,0x1a,-1,0x1b,0x1c}},receiver);
    if(physical.present){set_actor_physical_mode_bit(physical.pending,5);set_actor_physical_mode_bit(physical.pending,1);}
    result.sounds.push_back("M_SHIMMY_ON");                            // 001a22f4 .. 003c9420(004ffb50)
    ++result.delayed_sounds;                                           // 001a236c .. 003ea1d0
    result.caught=true;
    return result;
}

RootVector place_actor_local_point(const RootVector& t,const RootVector& q,const RootVector& p){
    for(float v:t)check(v);
    for(float v:q)check(v);
    for(float v:p)check(v);
    // r = q (t, 0): vopmula/vmaddaw/vopmsub for xyz, vmul/vsubaz/vmsubay/vmsubx for w.
    const float rx=sub(add(mul(q[1],t[2]),mul(t[0],q[3])),mul(t[1],q[2]));
    const float ry=sub(add(mul(q[2],t[0]),mul(t[1],q[3])),mul(t[2],q[0]));
    const float rz=sub(add(mul(q[0],t[1]),mul(t[2],q[3])),mul(t[0],q[1]));
    const float rw=sub(sub(sub(mul(q[3],0.0F),mul(q[2],t[2])),mul(q[1],t[1])),mul(q[0],t[0]));
    // c = (0 - q.xyz, q.w); out.xyz = r c (opmula, maddaw r*c.w, maddaw c*r.w, opmsub).
    const float cx=sub(0.0F,q[0]),cy=sub(0.0F,q[1]),cz=sub(0.0F,q[2]),cw=q[3];
    RootVector out{sub(add(add(mul(ry,cz),mul(rx,cw)),mul(cx,rw)),mul(cy,rz)),
                   sub(add(add(mul(rz,cx),mul(ry,cw)),mul(cy,rw)),mul(cz,rx)),
                   sub(add(add(mul(rx,cy),mul(rz,cw)),mul(cz,rw)),mul(cx,ry)),t[3]};
    if(out[3]==1.0F){for(std::size_t i=0;i<3;++i)out[i]=add(out[i],p[i]);}
    else{
        const float w=out[3];
        for(std::size_t i=0;i<3;++i)out[i]=add(out[i],mul(p[i],w));
        out[3]=mul(p[3],w);
    }
    return out;
}

void launch_actor_jump(ActorJumpState& jump,ActorJumpPhysical& physical,const ActorJumpLaunchInput& in){
    if(!physical.present)return;                                       // 001a2488
    if(jump.kind_3490!=1&&jump.kind_3490!=2)
        throw std::runtime_error("Actor jump launch without a jump kind (+3490 1 or 2) is not native");
    const auto& pos=in.position;
    const RootParentTransform start=in.clip_start?*in.clip_start:RootParentTransform{{0,0,0,1},{0,0,0,1}};
    for(float v:start.position)check(v);
    const auto offset=sub4(pos,in.transform_position);                 // 001a2604 / 001a28a4
    RootVector target;float arc;
    std::optional<float> radius;                                       // stored after the solver here (no effect before a throw)
    if(jump.kind_3490==2){
        radius=mul(0.5F,std::fabs(start.position[1]));                 // 001a25b8
        const auto local=add4(start.position,RootVector{0.0F,bits(0xbee66666U),0.0F,0.0F});
        target=add4(place_actor_local_point(local,jump.grab_34a0.orientation,jump.grab_34a0.position),offset);
        target[3]=1.0F;                                                // 001a26f8
        const float rise=sub(check(jump.grab_34a0.position[2]),check(pos[2]));
        arc=sub(0.5F,mul(sub(12.0F,rise),bits(0x3cb851ecU)));
        arc=bit(in.flags_3248,10)?minimum(maximum(arc,bits(0x3dcccccdU)),1.0F):minimum(maximum(arc,0.5F),1.0F);
    }else{
        // 001a2900..001a29c4: rot(flor, start) keeps start.w; then three xyzw sums.
        const auto rotated=rotate_actor_local_point(start.position,in.flor_orientation);
        target=add4(add4(add4(jump.grab_34a0.position,rotated),offset),RootVector{});
        target[3]=1.0F;                                                // 001a2a28
        const float rise=sub(target[2],check(pos[2]));                 // 001a2a2c (+34c8)
        arc=sub(0.5F,mul(sub(9.0F,rise),bits(0x3cb851ecU)));          // 0x41100000
        arc=minimum(maximum(arc,bits(0x3dcccccdU)),1.0F);              // 001a2a44 / 001a2a4c
    }
    // 003a8c80 with g = 004f5438 and the minimum angle pi/4 (0x3f490fdb).
    auto solution=solve_projectile_trajectory(pos,target,bits(game_gravity_bits),arc,bits(0x3f490fdbU));
    // 001a2a80..001a2ae4 (Checkpoint 132fx1): +3248 bit 10 with +3310 +1f4 and its +54: both velocities += +54 +a0.
    if(bit(in.flags_3248,10)&&physical.support_velocity_a0){
        solution.initial_velocity=add4(solution.initial_velocity,*physical.support_velocity_a0);
        solution.terminal_velocity=add4(solution.terminal_velocity,*physical.support_velocity_a0);
    }
    if(radius)physical.radius_374=*radius;
    jump.target_34c0=target;
    jump.arrival_34d0=solution.terminal_velocity;
    physical.keep_contact_9c0=1;
    physical.override_9d0=solution.initial_velocity;
}

// 0019f450 head (actor +14): drop = +30a4 - 'flor' z. Airborne (+dc): a drop above 100
// (0049cf18) calls virtual +100 with type 0xf and the integer health unless
// the life state is 2/3. Landed: thresholds 16/19.5 (0049cef0/0049cef8), or
// 12/16 (0049cf00/0049cf08) with action 21 or +33a5; a drop above 0 is
// stored at +30a8; above the lower threshold virtual +100 is called with
// type 0xc and ((drop - lo) * (1 / (hi - lo))) * +3398, or the integer
// health above the upper threshold. The thresholds are written by the static
// initializer 004c6c60.
namespace {
std::optional<ActorFallDamage> fall_damage(const ActorAnimationReceiverState& a,ActorJumpState& jump,
    bool airborne,const ActorAirInput& in,const ActorAirHost& host){
    const float drop=sub(check(jump.fall_start_30a4),check(host.floor_z()));
    const auto whole_health=[&]{                                       // cvt.w.s (truncation), cvt.s.w
        const float h=check(in.health_40);
        if(!(std::fabs(h)<2147483648.0F))throw std::runtime_error("Actor fall damage: health outside the integer range");
        return static_cast<float>(static_cast<std::int32_t>(h));};
    std::optional<ActorFallDamage> out;
    if(airborne){
        if(drop<=bits(0x42c80000U))return out;                          // 100
        if(in.life_state==2||in.life_state==3)return out;
        out=ActorFallDamage{0xf,whole_health()};
    }else{
        const bool lowered=bit(a.active_actions,21)||in.byte_33a5;
        const float lo=lowered?bits(0x41400000U):bits(0x41800000U);   // 12 / 16
        const float hi=lowered?bits(0x41800000U):bits(0x419c0000U);   // 16 / 19.5
        if(drop<=0.0F)return out;
        jump.fall_height_30a8=drop;                                     // delay slot of the lower test
        if(drop<=lo)return out;
        float amount;
        if(drop<=hi)amount=mul(mul(sub(drop,lo),check(vu::div(1.0F,sub(hi,lo)))),static_cast<float>(in.health_3398)); // div.s, cvt.s.w
        else amount=whole_health();
        out=ActorFallDamage{0xc,amount};
    }
    if(!host.damage)throw std::runtime_error("Actor fall damage: actor virtual +100 (type "+std::to_string(out->type)+") is not hosted");
    host.damage(out->type,out->amount);
    return out;
}
}

std::optional<ActorFallDamage> update_actor_fall_state(const ActorAnimationReceiverState& a,ActorJumpState& jump,
    bool physical_present,bool unsupported_dc,bool first,const ActorAirInput& in,const ActorAirHost& host){
    if(!host.floor_z)throw std::runtime_error("Actor fall state: floor host required");
    const bool airborne=physical_present&&unsupported_dc;
    ActorFallStateTrace trace;
    if(host.trace_fall_state){
        trace.physical_present=physical_present;trace.unsupported_dc=unsupported_dc;trace.first=first;trace.byte_14=in.byte_14;
        trace.byte_33a5=in.byte_33a5;trace.fall_start_before=jump.fall_start_30a4;trace.floor_z=host.floor_z();
        trace.health_40=in.health_40;trace.health_3398=in.health_3398;trace.active_actions=a.active_actions;trace.life_state=in.life_state;
    }
    std::optional<ActorFallDamage> out;
    if(in.byte_14)out=fall_damage(a,jump,airborne,in,host);
    // 0019f640: +30a4 = 'flor' z on the first unsupported frame, -FLT_MAX when supported.
    if(airborne){if(first)jump.fall_start_30a4=check(host.floor_z());}
    else jump.fall_start_30a4=bits(0xff7fffffU);
    if(host.trace_fall_state){
        trace.fall_start_after=jump.fall_start_30a4;trace.fall_height_after=jump.fall_height_30a8;trace.damage=out;
        host.trace_fall_state(trace);
    }
    return out;
}


ActorAirResult update_actor_air_state(ActorAnimationReceiverState& a,ActorActionHistory& history,
    ActorJumpState& jump,ActorJumpPhysical& p,std::array<ActorEdgeSlotState,9>& slots,
    const ActorAirInput& in,const ActorAirHost& host){
    if(!host.launch||!host.catch_ledge||!host.floor_z||!host.component_message)
        throw std::runtime_error("Actor air state: complete host required");
    ActorAirResult result;
    const float dt=check(in.elapsed);
    // 0019f890 reactions that are not native, checked before any effect.
    const ActorPrioritySnapshot older=history.newer_3220,newer{a.highest_action,a.active_actions,a.changes_3218};
    // 0019f6c0: priority history and the cleared change word, +3248 history.
    history.older_3234=older;history.newer_3220=newer;
    a.changes_3218=0;
    history.flags_3258=history.flags_3250;history.flags_3250={a.flags_3248,a.flags_324c};
    // Each 0019f890 test: (older && !newer) || (!older && !newer && newer change).
    const auto left=[&](unsigned b){
        return (bit(older.bits,b)&&!bit(newer.bits,b))||(!bit(older.bits,b)&&!bit(newer.bits,b)&&bit(newer.changes,b));};
    // 0019f8a4..0019f96c: left action 21 (the body carry): 0019fd60 on 'flor'
    // then 'edge', 00143cf0 on +3524, then the unlink 001a02c0.
    if(left(21)){
        if(!host.leave_carry)throw std::runtime_error("Actor history 0019f890: the carry leave (0019f8a4) is not hosted");
        host.leave_carry();
    }
    // 0019f970..0019fb48: left action 17 or 18 (the buddy-climb idles) without action 19 now.
    if((left(17)||left(18))&&!bit(newer.bits,19)&&a.controller_3474){
        const std::int32_t code=in.life_state==2||in.life_state==3?0xd:0xf;   // virtual +108
        if(!host.object_code_immediate)
            throw std::runtime_error("Actor history 0019f890: the CAS code to the object +3474 (0019fb28) is not hosted");
        const auto object=a.controller_3474;
        a.controller_3474=0;                                           // 0019fb48 (after the send)
        host.object_code_immediate(object,code);result.object_code=code;
    }
    // 0019f890 bit 16 (0019fb50): left the ladder by the same test as bit 6.
    const bool left_ladder=left(16);
    if(left_ladder){
        if(!host.component_message_immediate||!host.restore_inventory)
            throw std::runtime_error("Actor history 0019f890: the ladder reaction (code 12, 00143cf0) is not hosted");
        host.component_message_immediate(12);result.component_code12=true;
        host.restore_inventory();
    }
    // 0019f890 bit 6: (older && !newer) || (!older && !newer && newer change).
    const bool left_hang=(bit(older.bits,6)&&!bit(newer.bits,6))||(!bit(older.bits,6)&&!bit(newer.bits,6)&&bit(newer.changes,6));
    if(left_hang){                                                     // 0019fcd0
        a.flags_3248&=~0x100U;a.flags_3248&=~0x200U;
        host.component_message(7);result.component_code7=true;
    }
    // 0019f724: table 0049d5e0 {5, 0x10, 0x11, 0x15, 0x1f}: an active entry
    // other than the mode goes through 00189590; only 5 has an effect.
    for(unsigned id:{5U,0x10U,0x11U,0x15U,0x1fU}){
        if(static_cast<std::int32_t>(id)==a.highest_action||!bit(a.active_actions,id))continue;
        if(id==5){leave_air_action(a,p);result.action5_cleared=true;}
    }
    // 001a2c00.
    if(!p.present)return result;
    if(bit(a.flags_3248,14)){                                           // 001a2c28
        a.flags_3248&=~0x4000U;
        if(bit(a.active_actions,4)){host.launch();result.launched=true;}
    }
    const std::uint32_t acts=a.active_actions;
    const auto land=[&]{
        if(!host.land)throw std::runtime_error("Actor landing 001a0b30 is not native");
        host.land();};
    const auto fall=[&]{
        if(!host.fall)throw std::runtime_error("Actor fall 001a1590 is not native");
        host.fall();};
    if(bit(acts,4)){
    }else if(bit(acts,5)){
        if(bit(a.flags_3248,10)){
            if(in.life_state!=2&&in.life_state!=3){
                // 001a3170: +310 + (+330 * +2e4) * dt per lane, z compared;
                // the actor's virtual +e4 (00132860) is read and discarded.
                const float predicted=add(p.velocity_310[2],mul(mul(p.force_330[2],p.inverse_mass_2e4),dt));
                if(predicted<=jump.arrival_34d0[2]){host.catch_ledge();result.caught=true;}
            }
        }else if(bit(a.flags_3248,11)||bit(a.flags_3248,12)){
            if(p.velocity_310[2]<=0.0F){if(p.supported_dd)land();else fall();}
        }
    }else if(bit(acts,10)){
        if(p.supported_dd)land();
    }else{
        const std::int32_t mode=a.highest_action;
        bool test=true;
        if(mode!=0&&!(bit(acts,2)&&bit(a.flags_3248,3))&&!bit(acts,9)){
            if(mode==0x1b||mode==0x1f)test=!bit(acts,29);
            else test=false;
        }
        // 001a2ed8: action 9 with +dd lands; otherwise the airtime test.
        if(test){
            if(bit(acts,9)&&p.supported_dd)land();
            else if(jump.airtime_30a0>bits(0x3d0b4396U)&&p.floor_2c4>1.5F){         // 0049cee0, 0049cee8
                const float threshold=mode==0x1b?-8.0F:mode==0x1f?-3.0F:bits(0xbf87f319U); // 0049cf30/28/20
                if(p.velocity_310[2]<threshold){
                    a.flags_3248&=~0x400U;a.flags_3248&=~0x800U;a.flags_3248&=~0x1000U;a.flags_3248&=~0x2000U;
                    fall();
                }
            }
        }
    }
    // 001a3050: airtime, 0019f450, 0019e670 and the slot-5 request.
    bool first=false;
    if(p.enabled_12&&p.keep_velocity_13&&p.unsupported_dc){
        first=jump.airtime_30a0==0.0F;
        jump.airtime_30a0=add(jump.airtime_30a0,dt);
    }else jump.airtime_30a0=0.0F;
    result.damage=update_actor_fall_state(a,jump,p.present,p.unsupported_dc!=0,first,in,host);   // 0019f450(actor, first)
    if(!p.unsupported_dc&&(bit(a.active_actions,29)||bit(a.active_actions,30))){   // 0019e670(actor, 0)
        if(!host.land_dead)throw std::runtime_error("Actor 0019e670 for actions 29/30 is not hosted");
        host.land_dead();
    }
    if((in.byte_14||!in.component_is_player)&&a.highest_action==10){      // 001a3128
        slots[5].request=true;slots[5].query_only=false;result.slot5_latched=true;
    }
    return result;
}

bool actor_hang_slot_climbable(const std::array<ActorEdgeSlotState,9>& slots){
    for(std::size_t i:{6U,5U,3U})if(slots[i].result)return bit(slots[i].mask,4);
    return false;
}

ActorPullUpResult handle_actor_pull_up(ActorAnimationReceiverState& actions,ActorJumpPhysical& physical,
    const std::array<ActorEdgeSlotState,9>& slots,const ActorHangActionInput& in,const ActorHangMotionHost& host){
    ActorPullUpResult result;result.handled=true;
    bool accept=physical.present&&in.life_state!=2&&in.life_state!=3;
    if(accept){
        const bool hanging=(actions.highest_action==7||actions.highest_action==8)&&actor_hang_slot_climbable(slots);
        if(!hanging)accept=!in.byte_14&&in.component_is_player;
    }
    if(!accept){result.rejected=true;return result;}                   // 001ab5a8: code 9 to +58
    require_host(host,true);
    if(!in.receiver)throw std::runtime_error("Actor pull-up: receiver required");
    // 001ae270/001ae260 then 003ad500 without a null test: the original
    // installs whatever +326c holds; an unresolved one is not modelled.
    const auto retained=host.retained();
    if(!retained.node)throw std::runtime_error("Actor pull-up: retained motion (+326c) is unresolved");
    const auto half=transition_of(0.5F);                               // 0018a380(0049d038)
    install(result.motions,host,"pulling_up_from_ledge",MotionInstallMode::replace,half,
        Tokens{{0x2e,0x2f,-1,0x30,-1,0x31,0x32}},in.receiver);
    result.motions.push_back({"",MotionInstallMode::queue});
    host.install(request_of(retained,half,std::nullopt,in.receiver),MotionInstallMode::queue);
    set_actor_action(actions,1,true);
    if(physical.present){set_actor_physical_mode_bit(physical.pending,5);set_actor_physical_mode_bit(physical.pending,3);}
    return result;
}

bool handle_actor_hang_drop(ActorAnimationReceiverState& actions,const ActorHangActionInput& in,
    const std::function<void()>& fall){
    if(in.life_state==2||in.life_state==3)return false;
    const bool hanging=bit(actions.active_actions,7)||bit(actions.active_actions,8);
    if(!hanging&&(in.byte_14||!in.component_is_player))return false;
    if(!fall)throw std::runtime_error("Actor fall 001a1590 (drop from a hang) is not native");
    actions.flags_3248&=~0x3c00U;                                      // 001ae2a0 bits 10..13
    fall();
    return true;
}

ActorFallResult fall_actor(ActorAnimationReceiverState& actions,const ActorFallInput& in,const ActorHangMotionHost& host){
    ActorFallResult result;
    if(actions.highest_action==0x1b||actions.highest_action==0x1f){       // 001a15b4
        set_actor_action(actions,0x1d,true);return result;
    }
    if(bit(actions.active_actions,10))return result;                      // 001a1618
    require_root_host(host);
    if(!in.receiver)throw std::runtime_error("Actor fall: receiver required");
    // 001a1868..001a19ac: v = (+310.xy, 0, w); f = (+20.xy, 0, w) normalized
    // over xyz (vrsqrt, vmulq.xyz); dot over xyz in X, Y, Z order.
    RootVector v=in.physical_present?in.velocity_310:RootVector{};
    v[2]=0.0F;
    RootVector f=in.forward_row;f[2]=0.0F;
    for(float x:v)check(x);
    for(float x:f)check(x);
    const float squared=add(add(mul(f[0],f[0]),mul(f[1],f[1])),mul(f[2],f[2]));
    if(squared==0.0F)throw std::runtime_error("Actor fall: vertical forward axis (zero horizontal length) is not native");
    const float q=check(vu::rsqrt(squared));
    for(std::size_t i=0;i<3;++i)f[i]=mul(f[i],q);
    const float dot=add(add(mul(v[0],f[0]),mul(v[1],f[1])),mul(v[2],f[2]));
    std::uint32_t now=actions.flags_3248;
    if(!(dot<=10.0F)&&!bit(now,10)&&!bit(now,11)&&!bit(now,12))now|=0x2000U;
    // 001a19f0: statics 0049d080/d0a0/d0e0 = "blendFall", 0049d0c0 = "blendJumpUpInAir".
    const char* name=bit(now,11)||bit(now,13)||!bit(now,12)?"blendFall":"blendJumpUpInAir";
    const auto r=host.resolve_root(name);                                  // 001926d0/001926c0 (model +0x44)
    if(!r.node)throw std::runtime_error(std::string("Actor fall: named root ")+name+" is unresolved (the original does not check)");
    actions.flags_3248=now;
    auto request=request_of(r,transition_of(bits(0x3f000000U)),Tokens{{0x21,-1,-1,0x22,-1,0x23,0x24}},in.receiver);
    result.motions.push_back({name,MotionInstallMode::replace});
    host.install(request,MotionInstallMode::replace);                     // 001a1c00
    result.installed=true;
    return result;
}

ActorLandResult land_actor(ActorAnimationReceiverState& actions,const ActorLandInput& in,const ActorHangMotionHost& host){
    ActorLandResult result;
    if(in.life_state==2||in.life_state==3)return result;                  // virtual +108
    if(in.argument==0){
        // 001a1258..001a12bc: 001a13f0(+2ec0, +326c, 3, 0018a380(0018a410(*(+2ec0))), the 001a1500 block
        // without tokens) -> 003ad500 with f12 = -1, f13 = 0; then physical +9c0 = 0, +11 = 1.
        if(!host.install||!host.retained)throw std::runtime_error("Actor landing: motion host required");
        if(!in.default_duration)throw std::runtime_error("Actor landing 001a0b30(actor, 0) needs the model's default duration (0018a410)");
        const auto retained=host.retained();
        if(!retained.node)throw std::runtime_error("Actor landing: retained motion (+326c) is unresolved");
        result.handled=true;
        result.motions.push_back({"",MotionInstallMode::reset});
        host.install(request_of(retained,transition_of(check(*in.default_duration)),std::nullopt,in.receiver),MotionInstallMode::reset);
        result.physical_reset=true;
        ++result.effects;                                                  // 001a12c8
        ++result.delayed_events;                                           // 001a13b0
        return result;
    }
    result.handled=true;
    if(!bit(actions.active_actions,11)){                                   // 001a0ba0
        require_root_host(host,true);
        if(!in.receiver)throw std::runtime_error("Actor landing: receiver required");
        const bool short_fall=check(in.airtime_30a0)==0.0F;               // 001a0df0
        const float first=short_fall?bits(0x3eb33333U):bits(0x3e4ccccdU);
        const float second=short_fall?first:bits(0x3f000000U);            // 0049d038
        // Statics 0049d100/d120/d140 = "blendLand" (+3248 bit 11, bit 13,
        // otherwise), 0049d160 = "blendLandFromShortFallFromHang".
        const char* name=short_fall?"blendLandFromShortFallFromHang":"blendLand";
        const auto r=host.resolve_root(name);                              // 001926d0/001926c0 (model +0x44)
        if(!r.node)throw std::runtime_error(std::string("Actor landing: named root ")+name+" is unresolved (the original does not check)");
        const auto retained=host.retained();
        if(!retained.node)throw std::runtime_error("Actor landing: retained motion (+326c) is unresolved");
        result.motions.push_back({name,MotionInstallMode::replace});
        host.install(request_of(r,transition_of(first),Tokens{{0x25,-1,-1,0x26,-1,0x27,0x28}},in.receiver),MotionInstallMode::replace);
        result.motions.push_back({"",MotionInstallMode::queue});
        host.install(request_of(retained,transition_of(second),std::nullopt,in.receiver),MotionInstallMode::queue);
    }
    ++result.effects;                                                      // 001a12c8: 00188fb0(actor, 2, 1)
    ++result.delayed_events;                                               // 001a13b0: 003ea1d0
    return result;
}

ActorTimedEvent actor_timed_event(ActorTimedEvent::Kind kind,std::uint32_t draw){
    if(draw>0x7fffffffU)throw std::runtime_error("Actor timed event: RNG value outside 0..2^31-1");
    const bool landing=kind==ActorTimedEvent::Kind::landing;
    if(!landing&&kind!=ActorTimedEvent::Kind::shimmy)throw std::runtime_error("Actor timed event: unknown kind");
    const std::int32_t base=landing?0x46:0x14,span=landing?0x1f:0x15;   // 001a1310 / 001a23ac, 001a12d4 / 001a2370
    // cvt.s.w (the EE truncates above 24 bits), div.s by 0x4f000000, mul.s by the span, cvt.w.s.
    const float u=check(vu::div(game_int_to_float(static_cast<std::int32_t>(draw)),bits(0x4f000000U)));
    const float scaled=mul(static_cast<float>(span),u);
    if(!(scaled<2147483648.0F))throw std::runtime_error("Actor timed event: delay outside the integer range");
    return {kind,base+static_cast<std::int32_t>(scaled)};
}
}
