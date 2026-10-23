#include "actor_body_carry.hpp"
#include "game_libm.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor body carry: nonfinite arithmetic");
    if(std::fpclassify(v)==FP_SUBNORMAL)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
float mul(float a,float b){return check(check(a)*check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
// 0049d038 after static initialization (the boot snapshot).
const float crossfade=bits(0x3f000000U);
float rate(float d){return d==0.0F?std::numeric_limits<float>::max():check(1.0F/d);}
// 0018a380 (width = bias = d) and 001ad750 (width, bias).
ActorMotionTransition transition_of(float width,float bias){return {width,bias,rate(width),rate(bias)};}
// 0018a910 / 0018a8c0, then 001ae050(block, k, id, actor) for the listed k.
struct Tokens {std::array<std::int32_t,7> ids;};
constexpr std::int32_t none=-1;
MotionRequest request(const ActorMotionRegistration& r,const ActorMotionTransition& t,
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
struct Context {
    std::uint64_t receiver;
    const ActorBodyCarryHost& host;
    ActorBodyCarryResult& result;
    // 0018c5a0 on +2ec0, then 003ad500 (f12 = -1, f13 = 0) when resolved.
    void direct(const std::string& name,const ActorMotionTransition& t,MotionInstallMode mode,
        const std::optional<Tokens>& tokens,std::vector<MotionPhaseHook> hooks={}){
        const auto r=host.resolve(name);
        if(!r.node)return;
        auto q=request(r,t,tokens,receiver);q.phase_hooks=std::move(hooks);
        host.install(q,mode);
        result.motions.push_back(name);
    }
};
void need(const auto& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Actor body carry: ")+what+" is not hosted");
}
}

ActorBodyCarryResult handle_actor_carry_pickup(std::uint32_t flags_324c,std::uint64_t receiver,
    const ActorCarryPickupInput& in,const ActorBodyCarryHost& host){
    ActorBodyCarryResult result;
    if(in.life_state==2||in.life_state==3)return result;                 // 001abbf8
    if(bit(flags_324c,1))return result;                                 // 001abc20
    if(in.byte_14&&in.mode_320c!=0)return result;                       // 001abc34
    if(in.linked)return result;                                         // 001abc50
    if(in.kind_28!=5)return result;                                     // 001abc60
    if(!in.body||!in.body->human)return result;                         // 001abc70 / 001abc80
    const auto& body=*in.body;
    if(in.byte_14&&body.mode_320c!=0x1f)return result;                  // 001abc90
    result.handled=true;
    need(host.link,"the link");host.link();                             // 001abcb0
    if(!body.active){                                                   // 001abcbc
        if(!body.byte_14)throw std::runtime_error("001abd30: an inactive body without +14 (its virtual +20) is not native");
        need(host.activate_body,"the body activation");host.activate_body();   // virtual +5c(1)
    }
    need(host.holster,"the holster");host.holster();                    // 001abd40 00143b60(+3524)
    if(body.backpack_3464){need(host.hide_backpack,"the backpack hide");host.hide_backpack();}  // 00256120
    // 001abd60..001abe78: the axis (0, 0, 1), v = -row (001ada90), yaw =
    // atan2f(-v.x, v.y) - pi/2 (00121ab8, 0x3fc90fdb), 001ada60's half angle
    // 0.5 * yaw: (sinf * axis, cosf) (00140e80 cos first, 00140e70 sin), the
    // body's 'flor' position, 0017f460 and 001ad9c0 on the own 'edge'.
    const RootVector v{-body.component_row2[0],-body.component_row2[1],-body.component_row2[2],-body.component_row2[3]};
    const float yaw=sub(check(game_atan2f(check(-v[0]),v[1])),bits(0x3fc90fdbU));
    const float half=mul(bits(0x3f000000U),yaw);
    const float c=check(game_cosf(half)),s=check(game_sinf(half));
    RootParentTransform t;
    t.orientation={mul(s,0.0F),mul(s,0.0F),mul(s,1.0F),c};
    t.position=body.floor_position;
    need(host.place_edge_root,"the edge placement");host.place_edge_root(t);
    need(host.relate,"the relation");host.relate();                     // 001abe8c 001ad8c0
    need(host.attach_child_edges,"the child edges");host.attach_child_edges();   // 001abe98..001abf1c
    need(host.body_action,"the body action");host.body_action(0x2e);   // 001abf24..001abf78
    if(in.controller_is_player&&in.byte_14){                            // 001abf7c..001ac01c
        need(host.notify_body_moved,"AIBodyMovedMsg");host.notify_body_moved(true);
    }
    need(host.resolve,"the motion lookup");need(host.install,"the motion install");
    Context ctx{receiver,host,result};
    const auto fade=transition_of(crossfade,crossfade);
    // 001ac020..001ac17c: the hook 00192750(0.12, 0.12) with token 0x73 (00190f90).
    const float hook=bits(0x3df5c28fU);
    ctx.direct("pickupbody_pt1",fade,MotionInstallMode::replace,Tokens{{0x71,0x72,none,0x74,none,0x75,0x76}},
        {MotionPhaseHook{hook,hook,MotionToken{0x73,receiver}}});
    ctx.direct("switchCarryBody_Carrier",fade,MotionInstallMode::queue,     // 001ac198..001ac2dc
        Tokens{{0x77,0x78,none,0x79,none,0x7a,0x7b}});
    return result;                                                      // 001ac2e0: 005721c8 is multiplayer
}

ActorBodyCarryResult handle_actor_carry_drop(ActorMotionTransitionState& transition,std::uint32_t flags_324c,
    std::uint64_t receiver,const ActorCarryDropInput& in,const ActorBodyCarryHost& host){
    ActorBodyCarryResult result;
    if(in.action!=0x2c&&in.action!=0x2d)throw std::logic_error("Actor body carry: the drop takes 0x2c or 0x2d");
    if(in.life_state==2||in.life_state==3)return result;                // 001ac370
    if(bit(flags_324c,4))return result;                                 // 001ac398
    if(in.byte_14&&bit(flags_324c,2))return result;                     // 001ac3ac
    if(in.byte_14&&in.physical_dc)return result;                        // 001ac3c8
    const bool fallback=(in.byte_14&&!bit(flags_324c,3))||!in.linked||  // 001ac3f8..001ac44c
        (in.byte_14&&!bit(in.body_flags_324c,6));
    need(host.resolve,"the motion lookup");need(host.install,"the motion install");
    if(fallback){
        // 001ac8c0: a linked actor's retained motion through the override
        // (001adff0 0.5 s on +2ef4, 0018a420 tag 2 on +326c), replace, no tokens.
        if(!in.linked)return result;
        result.handled=true;
        need(host.retained,"the retained motion");
        set_actor_motion_transition(transition,transition_of(crossfade,crossfade));
        const auto r=host.retained();
        if(!r.node)return result;
        const auto used=transition.value;
        transition.pending=false;
        host.install(request(r,used,std::nullopt,receiver),MotionInstallMode::replace);
        result.motions.push_back(std::string());
        return result;
    }
    result.handled=true;
    if(in.edge_root_weight==0.0F){                                      // 001ac450..001ac480
        need(host.floor_root_world_placement,"the floor placement");need(host.place_edge_root,"the edge placement");
        host.place_edge_root(host.floor_root_world_placement());
    }
    need(host.attach_child_edges,"the child edges");host.attach_child_edges();   // 001ac488..001ac514
    const bool lay=in.action==0x2c;                                     // 001ac51c..001ac5dc
    need(host.body_action,"the body action");host.body_action(lay?0x2f:0x30);    // 001ac5ec..001ac640
    Context ctx{receiver,host,result};
    ctx.direct(lay?"animLayDownBody_Carrier":"animTossBody_Carrier",transition_of(crossfade,crossfade),
        MotionInstallMode::replace,Tokens{{0x7c,0x7d,none,0x7e,none,0x7f,0x80}});   // 001ac650..001ac778
    // 001ac788..001ac83c: +326c (001ae270 / 001ae260) installed directly with 001ad750(0.5, 0).
    need(host.retained,"the retained motion");
    const auto r=host.retained();
    if(r.node){
        host.install(request(r,transition_of(crossfade,0.0F),std::nullopt,receiver),MotionInstallMode::queue);
        result.motions.push_back(std::string());
    }
    return result;                                                      // 001ac844: 005721c8 is multiplayer
}

ActorBodyCarryResult handle_actor_body_action(std::int32_t action,std::uint32_t flags_324c,std::uint64_t receiver,
    const ActorBodyCarryHost& host){
    ActorBodyCarryResult result;
    need(host.resolve,"the motion lookup");need(host.install,"the motion install");
    Context ctx{receiver,host,result};
    const auto fade=transition_of(crossfade,crossfade);
    if(action==0x2e){                                                   // 001acae0
        if(bit(flags_324c,5))return result;
        result.handled=true;
        ctx.direct("pickupbody_pt2",fade,MotionInstallMode::replace,Tokens{{0x81,0x82,none,0x83,none,0x84,0x85}});
        ctx.direct("switchCarryBody_Body",fade,MotionInstallMode::queue,Tokens{{0x86,0x87,none,0x89,none,0x88,0x8a}});
        return result;
    }
    if(action==0x2f||action==0x30){                                     // 001acd88
        if(bit(flags_324c,7))return result;
        result.handled=true;
        const bool lay=action==0x2f;
        ctx.direct(lay?"animLayDownBody_Body":"animTossBody_Body",fade,MotionInstallMode::replace,
            Tokens{{0x8b,0x8c,none,0x8d,none,0x8e,0x8f}});
        ctx.direct(lay?"animBodyFinalPoseAfterLaidDown":"animBodyFinalPoseAfterTossed",transition_of(crossfade,0.0F),
            MotionInstallMode::queue,std::nullopt);
        return result;
    }
    throw std::logic_error("Actor body carry: the body actions are 0x2e, 0x2f and 0x30");
}

void unlink_actor_body(const ActorCarryUnlinkInput& in,const ActorCarryUnlinkHost& host){
    need(host.detach_relation_roots,"the relation roots");host.detach_relation_roots();   // 001a08d0
    need(host.clear_relation,"the relation clear");host.clear_relation();                 // 001a07f0
    if(!in.linked)return;
    if(in.controller_is_player&&in.byte_14){
        need(host.notify_body_moved,"AIBodyMovedMsg");host.notify_body_moved(false);
    }
    need(host.turn_on_weapon_pickup,"cTurnOnWeaponPickupMsg");host.turn_on_weapon_pickup();
    if(!(bit(in.flags_325c,4)&&!bit(in.flags_3254,4))){                 // 001a04dc..001a0518
        need(host.body_landing_pose,"the body's landing pose 0019e9a0");
        const std::string name=host.body_landing_pose();                  // 001a059c
        need(host.resolve_body,"the body's motion lookup");
        const auto reg=host.resolve_body(name);                           // 001a0630
        if(reg.node){
            need(host.install_body,"the body's motion install");
            MotionRequest q;q.node=*reg.node;q.selector=reg.selector;
            q.width=0.5F;q.bias=0.5F;q.rate=2.0F;q.secondary_rate=2.0F;   // 001a05bc..001a05d0
            q.limit=-1;q.initial_phase=0;q.notifications_active=false;
            host.install_body(q,MotionInstallMode::replace);              // 001a0694 (a2 = 1)
        }
    }
    need(host.unlink,"the unlink");host.unlink();
}

void leave_actor_carry(const ActorCarryLeaveHost& host){
    need(host.detach_floor_children,"the floor children");host.detach_floor_children();   // 0019fd60 'flor'
    need(host.detach_edge_children,"the edge children");host.detach_edge_children();      // 0019fd60 'edge'
    need(host.restore_inventory,"the inventory restore");host.restore_inventory();        // 00143cf0
    need(host.unlink,"the unlink");host.unlink();                                         // 001a02c0
}
ActorCarryPairPlan plan_actor_carry_pair(const ActorCarryPairInput& in){
    ActorCarryPairPlan out;
    if(!bit(in.flags_324c,3))return out;                             // 0019d97c
    out.carrying=true;out.linked=in.linked;
    if(!in.linked)return out;                                         // 0019dbe8
    const float speed=check(in.speed_31d0),turn=check(in.turn_31d4),y=check(in.desired_y_31c4);
    out.channel_14=crossfade;                                         // 0.5
    if(!(speed<=0.0F)){                                               // 0019d9cc
        if(!(y<=0.0F))out.selector_4=1;                               // 0019d9e8
        else if(y<0.0F)out.selector_4=2;                              // 0019d9fc
        else out.pending_bit0=true;                                   // 0019da08
    }else{
        // 0019da10..0019da5c: without +324c bit 1 and with a turn, max.s / min.s
        // clamp 0.5 - turn * 0.2222 (0x3e636811) to [0, 1].
        if(!bit(in.flags_324c,1)&&turn!=0.0F)
            out.channel_14=std::min(std::max(sub(crossfade,mul(turn,bits(0x3e636811U))),0.0F),1.0F);
        out.pending_bit0=true;
    }
    return out;
}
}
