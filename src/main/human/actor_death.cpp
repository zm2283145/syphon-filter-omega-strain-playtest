#include "actor_death.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor death: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float maximum(float a,float b){return a<b?b:a;}                     // max.s
float minimum(float a,float b){return b<a?b:a;}                     // min.s
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
// trunc(scale * (float(r) / (1 + 2^31))) of one RNG draw (0018a9e4, 0018b068, 0018b210).
std::int32_t scaled_draw(const ActorDeathHost& h,float scale){
    const std::uint32_t draw=h.random();
    if(draw>0x7fffffffU)throw std::runtime_error("Actor death: RNG value outside 0..2^31-1");
    return static_cast<std::int32_t>(mul(scale,check(vu::div(game_int_to_float(static_cast<std::int32_t>(draw)),add(1.0F,bits(0x4f000000U))))));
}
using Matrix=std::array<float,16>;
using Row=std::array<float,4>;
Row row(const Matrix& m,std::size_t i){return {m[4*i],m[4*i+1],m[4*i+2],m[4*i+3]};}
// 0019f350: out row i = C0*N[i].x + C1*N[i].y + C2*N[i].z + C3*N[i].w
// (vmulax/vmadday/vmaddaz/vmaddw, xyzw).
Matrix world_of(const Matrix& n,const Matrix& c){
    Matrix out{};
    for(std::size_t i=0;i<4;++i)for(std::size_t k=0;k<4;++k){
        float acc=mul(c[k],n[4*i]);
        acc=add(acc,mul(c[4+k],n[4*i+1]));
        acc=add(acc,mul(c[8+k],n[4*i+2]));
        out[4*i+k]=add(acc,mul(c[12+k],n[4*i+3]));
    }
    return out;
}
// 0019ec50..0019ecb4: rows of the rotation of q (vf8, vf9, vf10).
std::array<Row,3> rotation_rows(const RootVector& q){
    for(float v:q)check(v);
    const Row t{add(q[0],q[0]),add(q[1],q[1]),add(q[2],q[2]),add(q[3],q[3])};
    Row x{},y{},z{};                                                   // vf4 = q*2x, vf5 = q*2y, vf6 = q*2z
    for(std::size_t i=0;i<4;++i){x[i]=mul(q[i],t[0]);y[i]=mul(q[i],t[1]);z[i]=mul(q[i],t[2]);}
    std::array<Row,3> r{};
    r[0]={sub(sub(1.0F,y[1]),z[2]),add(x[1],z[3]),sub(x[2],y[3]),0.0F};
    r[1]={sub(y[0],z[3]),sub(sub(1.0F,x[0]),z[2]),add(y[2],x[3]),0.0F};
    r[2]={add(z[0],y[3]),sub(z[1],x[3]),sub(sub(1.0F,x[0]),y[1]),0.0F};
    return r;
}
// 0019f2a0: xyz lane i = ((R_i.x*u.x) + R_i.y*u.y) + R_i.z*u.z; w kept.
Row rotate(const std::array<Row,3>& r,const Row& u){
    Row out=u;
    for(std::size_t i=0;i<3;++i)out[i]=add(add(mul(r[i][0],u[0]),mul(r[i][1],u[1])),mul(r[i][2],u[2]));
    return out;
}
}

ActorLandingPose choose_actor_landing_pose(const ActorLandingPoseInput& in){
    for(float v:in.component)check(v);
    // 0019e9e0..0019ec00: matrices A (node 0), B (node 2), C (node 1), D (node 3).
    Matrix a=in.component,b=in.component,c=in.component,d=in.component;
    if(in.node_matrices){
        for(const auto& n:*in.node_matrices)for(float v:n)check(v);
        a=world_of((*in.node_matrices)[0],in.component);b=world_of((*in.node_matrices)[2],in.component);
        c=world_of((*in.node_matrices)[1],in.component);d=world_of((*in.node_matrices)[3],in.component);
    }
    const float up_z=a[10];                                             // sp+698: A row 2 z
    const bool inverted=up_z<0.0F;
    ActorLandingPose out;
    out.upright=!(std::fabs(up_z)<=bits(0x3f350481U));                   // 0.70710677
    Row v=row(b,1);                                                     // sp+640
    if(!inverted&&out.upright){                                         // 0019eb30: vadd.xyzw
        const Row c1=row(c,1),d1=row(d,1);
        for(std::size_t i=0;i<4;++i)v[i]=add(c1[i],d1[i]);
    }
    const auto r=rotation_rows(in.flor_orientation);
    // vmul.xyzw, then w = ((x + y) + z) (vmulax.w/vmadday.w/vmaddz.w).
    const float dot=add(add(mul(v[0],r[2][0]),mul(v[1],r[2][1])),mul(v[2],r[2][2]));
    out.face_down=dot<=0.0F&&!in.on_back_a3;                             // 0019ecf4 c.ole.s, 0019ed00
    if(out.face_down)out.motion=in.last_frame_only_t0?"animBodyLandFaceDown_LastFrameOnly":"animBodyLandFaceDown";
    else out.motion=in.last_frame_only_t0?"animBodyLandOnBack_LastFrameOnly":"animBodyLandOnBack";
    // 0019ee68 / 0019eff0: the facing vector (A row 1 or row 2; on the back,
    // the negated row 1: ACC = 0, vmsubw).
    Row u=out.upright?row(a,1):row(a,2);
    if(!out.face_down&&out.upright)for(auto& x:u)x=check(0.0F-x);
    const Row turned=rotate(r,u);
    const float angle=game_atan2f(check(-turned[0]),turned[1]);          // 00121ab8(-x, y)
    const float turn=mul(add(angle,bits(0x40490fdbU)),bits(0x3e22f983U));   // (a + pi) / 2pi
    out.heading_channel=minimum(maximum(turn,0.0F),1.0F);               // max.s 0, min.s 1
    // Blend: (1.2 - (0.2 / -16) * vz) * 0.25, clamped to [0.125, 0.25].
    const float k=check(vu::div(bits(0x3e4ccccdU),bits(0xc1800000U)));
    const float vz=in.physical_present?check(in.velocity_z_310):0.0F;
    out.blend=minimum(maximum(mul(sub(bits(0x3f99999aU),mul(k,vz)),bits(0x3e800000U)),bits(0x3e000000U)),bits(0x3e800000U));
    out.sound=check(in.fall_height_30a8)<=12.0F?"M_BODY_FALL":"M_BODY_DROP";   // 0049cf10
    return out;
}

ActorLandingPose run_actor_landing_pose(const ActorDeathHost& h,bool on_back_a3,bool last_frame_only_t0){
    if(!h.landing_pose_input||!h.set_channel||!h.sound||!h.landing_effect)
        throw std::runtime_error("Actor landing pose: hosts (0019e9a0) required");
    auto pin=h.landing_pose_input();pin.on_back_a3=on_back_a3;pin.last_frame_only_t0=last_frame_only_t0;
    const auto pose=choose_actor_landing_pose(pin);
    h.set_channel(0x16,pose.heading_channel);
    h.sound(pose.sound);
    h.landing_effect(0,1);
    return pose;
}

ActorDeadLandingResult land_dead_actor(ActorAnimationReceiverState& actions,bool physical_present,bool unsupported_dc,
    bool controller_ai,std::uint64_t receiver,const ActorDeathHost& h){
    ActorDeadLandingResult result;
    if(physical_present&&unsupported_dc)return result;                  // 0019e6b0 (a1 = 0)
    if(!bit(actions.active_actions,0x1d)&&!bit(actions.active_actions,0x1e))return result;
    if(!h.landing_pose_input||!h.set_channel||!h.sound||!h.landing_effect||!h.resolve_name||!h.install)
        throw std::runtime_error("Actor dead landing: hosts (0019e9a0, 0018c5a0, 003ad500) required");
    result.handled=true;
    set_actor_action(actions,0x1d,false);set_actor_action(actions,0x1e,false);
    auto pin=h.landing_pose_input();pin.on_back_a3=false;pin.last_frame_only_t0=false;
    result.pose=choose_actor_landing_pose(pin);
    h.set_channel(0x16,result.pose.heading_channel);
    h.sound(result.pose.sound);
    h.landing_effect(0,1);
    const auto reg=h.resolve_name(result.pose.motion);                  // 0019e86c
    if(reg.node){
        MotionRequest q;q.node=*reg.node;q.selector=reg.selector;
        const float b=result.pose.blend;
        const float rate=b==0.0F?std::numeric_limits<float>::max():check(1.0F/b);
        q.width=b;q.bias=b;q.rate=rate;q.secondary_rate=rate;q.limit=-1;q.initial_phase=0;
        q.notifications_active=true;
        for(std::size_t k:{std::size_t{4},std::size_t{5},std::size_t{6}})q.notifications[k]={0x92,receiver};
        h.install(q,MotionInstallMode::replace);                         // 0019e8d0 (a2 = 1)
        result.motion=result.pose.motion;
    }
    result.clear_ai_114=controller_ai;                                  // 0019e914..0019e97c
    return result;
}

void apply_actor_secondary_mode(std::uint8_t& b312,std::uint8_t& b313,std::uint8_t mode){
    b313=mode;
    if(b312!=0x15&&b312!=0x16&&b312!=0x18)b312=mode;
}

ActorDeathResult handle_actor_death(ActorAnimationReceiverState& actions,std::int32_t kind,std::int32_t code,
    const ActorDeathActor& actor,const ActorDeathHost& h){
    ActorDeathResult result;
    if(bit(actions.active_actions,27)||bit(actions.active_actions,31)){result.already_dying=true;return result;}   // 0018a988
    if(!h.self_action)throw std::runtime_error("Actor death: self action host required");
    if(actor.controller_is_player){                                     // 0018a9d8: voice line 8 or 9
        if(!h.random||!h.voice)throw std::runtime_error("Actor death: the RNG and the voice host (0018c770) are required");
        h.voice(8+scaled_draw(h,2.0F));
    }
    h.self_action(0x24);                                                 // 0018aa68 (transport 1)
    if(kind!=2)return result;                                            // 0018aa90
    result.handled=true;
    if(actor.controller_is_player&&actor.byte_14&&actor.byte_326b){
        if(!h.pop_world_object)throw std::runtime_error("Actor death: world-pop host required for player +326b");
        h.pop_world_object();                                           // 0018ab68: 00131240(004ffb50)
        result.byte_326b=std::uint8_t{0};                               // 0018ab70
    }
    if(actor.physical_present&&!(actor.controller_is_player&&actor.byte_14))result.physical_14=std::uint8_t{2};   // 0018abd0
    std::string name;
    float blend=bits(0x3f000000U);                                       // 0049d038 (0.5) at sp+89c
    bool immediate_dead=false;
    std::int32_t token_4=0x92;                                           // s0
    std::optional<std::uint32_t> extra_action;                          // s4 (-1: none)
    const auto draw=[&]{
        if(!h.random)throw std::runtime_error("Actor death: the RNG host is required");
        return h.random();};
    // Codes 0..8 (jump table 0049dbe0): the name is copied into sp+830 from
    // the code's string (the multi-choice lists are static tables built once).
    if(code==0){                                                         // 0018b598
        if(bit(actor.flags_3204,3)||bit(actor.flags_3204,4)||bit(actor.flags_3204,5)){
            static const char* const crouched[]={"DATA_ID_DeathCrouchFallBackwards","DATA_ID_DeathCrouchFallLeft","DATA_ID_DeathCrouchGutshot"};
            name=crouched[draw()%3U];                                    // 0018b758 divu by 3
        }else{
            static const char* const standing[]={"DATA_ID_DeathGutShot","DATA_ID_DeathRiddledWithBullets","DATA_ID_DeathStaggerForwards",
                "DATA_ID_DeathFacePlant","DATA_ID_DeathFacePlant2","DATA_ID_DeathSpinBlastFront"};
            name=standing[draw()%6U];                                    // 0018bac8 divu by 6
        }
    }else if(code==1){                                                   // 0018ac00
        if(bit(actor.flags_3204,3)||bit(actor.flags_3204,4)){
            static const char* const crouched[]={"DATA_ID_DeathCrouchFallBackwards","DATA_ID_DeathCrouchFallLeft"};
            name=crouched[draw()&1U];                                    // 0018ad30
        }else name="DATA_ID_DeathByKnifeOrHeadShot";
    }else if(code==2||code==3){                                          // 0018b338 / 0018b3f0
        name=code==2?"dataDeathFallOffLedge":"dataDeathFallOverRail";
        token_4=0x91;extra_action=0x1cU;
    }else if(code==4)name="death_from_explosion";                        // 0018af00
    else if(code==5)name="death_from_explosion_front";                   // 0018afb0
    else if(code==6)                                                     // 0018b060: trunc(2 r / (1 + 2^31))
        name=scaled_draw(h,2.0F)!=0?"DATA_ID_DeathShotgunBlastBack":"death_shot_from_behind";
    else if(code==7){                                                    // 0018b208: table 0048a290 by trunc(4 r / (1 + 2^31))
        static const char* const front[]={"DATA_ID_DeathShotgunBlastFront","death_shot_from_front","death_shot_front_gut_shot",
            "death_shotgun_blast_front2"};
        name=front[static_cast<std::size_t>(scaled_draw(h,4.0F))];
    }else if(code==8)name="death_from_taser";                            // 0018ae50
    else if(code==9||code==10){                                          // 0018b4a8 / 0018b4c8
        if(!h.landing_pose_input||!h.set_channel||!h.sound||!h.landing_effect)
            throw std::runtime_error("Actor death: landing pose hosts (0019e9a0) required");
        auto pin=h.landing_pose_input();pin.on_back_a3=false;pin.last_frame_only_t0=code==10;
        result.pose=choose_actor_landing_pose(pin);
        h.set_channel(0x16,result.pose->heading_channel);
        h.sound(result.pose->sound);
        h.landing_effect(0,1);
        name=result.pose->motion;blend=result.pose->blend;
    }else if(code==11){name="jump_idle_in_air";immediate_dead=true;}    // 0018b4e8
    else throw std::runtime_error("Actor death: death code "+std::to_string(code)+" is outside the jump table 0049dbe0 (0..11)");
    set_actor_action(actions,0x1b,true);                                 // 0018bb38
    if(extra_action)set_actor_action(actions,*extra_action,true);        // 0018bb74 (codes 2/3: 0x1c)
    // 0018bbc8: +34e4[code], else the node-name map (0 when absent).
    std::int32_t id=actor.motion_override_34e4.at(static_cast<std::size_t>(code));
    if(id<0){
        if(!h.node_id)throw std::runtime_error("Actor death: node-name map host required");
        id=static_cast<std::int32_t>(h.node_id(name).value_or(0U));
    }
    if(!(actor.object_2d&&actor.object_2c&&!actor.object_2e)){            // 0018bc80
        if(!h.resolve_name||!h.install||!h.dead_state)
            throw std::runtime_error("Actor death: inactive final-pose hosts (0018bc80) required");
        constexpr std::string_view final_pose="animBodyFinalPoseAfterTossed";
        const auto reg=h.resolve_name(final_pose);
        if(reg.node){
            MotionRequest q;q.node=*reg.node;q.selector=reg.selector;
            q.limit=-1.0F;q.initial_phase=0.0F;q.notifications_active=true;
            q.notifications[4]={token_4,actor.receiver};
            h.install(q,MotionInstallMode::reset);                       // 003ad500 (a2 = 3)
            result.motion=final_pose;
        }
        if(actor.physical_present)result.physical_11=std::uint8_t{1};    // physical +11
        h.dead_state();result.dead_state=true;                           // 001a3780
    }else{
    if(!h.resolve_id||!h.install)throw std::runtime_error("Actor death: motion install host required");
    const auto reg=h.resolve_id(static_cast<std::uint32_t>(id));         // 0018c6b0
    if(reg.node){
        MotionRequest q;q.node=*reg.node;q.selector=reg.selector;
        const float rate=blend==0.0F?std::numeric_limits<float>::max():check(1.0F/blend);
        q.width=blend;q.bias=blend;q.rate=rate;q.secondary_rate=rate;q.limit=-1;q.initial_phase=0;
        q.notifications_active=true;                                    // token block: slots 5/6 id 0 (s5), slot 4 0x92
        if(!immediate_dead)q.notifications[4]={token_4,actor.receiver};
        q.notifications[5]={0,actor.receiver};q.notifications[6]={0,actor.receiver};
        h.install(q,MotionInstallMode::replace);                         // 003ad500 (a2 = 1)
        result.motion=name;
    }
    if(immediate_dead){                                                  // 0018c064
        if(!h.dead_state)throw std::runtime_error("Actor death: dead state host (001a3780) required");
        h.dead_state();result.dead_state=true;
    }
    }
    // 0018c504: 001977f0(actor, 0). Nothing held (+3524 +84 == 6): 00143510
    // gives item kind 0 and 001437a0 11, so the flag +31b2 is 0 and the mode
    // is 1 on every path of the tail (00197c58, 00197db0). With a held item
    // the choice reads the actor after the actions above (+320c included).
    std::uint8_t mode=1;
    result.byte_31b2=0;
    if(actor.inventory_mode_84!=6){
        if(!h.secondary_input)throw std::runtime_error("Actor death: 001977f0 with a held item needs the actor's fields");
        auto in=h.secondary_input();
        in.raise=false;in.state_320c=actions.highest_action;in.inventory_84=actor.inventory_mode_84;
        const auto choice=choose_secondary_mode(in);
        result.byte_31b2=choice.flag_31b2;mode=choice.mode;
    }
    if(actor.secondary_present){
        if(!h.secondary_mode)throw std::runtime_error("Actor death: secondary component host (001cb120) required");
        h.secondary_mode(mode);result.secondary_mode=mode;
    }
    result.word_2dd0=0;result.word_2dd4=-1;result.word_2dec=0;           // 0018c50c..0018c540
    return result;
}
}
