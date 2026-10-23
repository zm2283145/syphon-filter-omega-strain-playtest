#include "actor_ledge_action.hpp"
#include "actor_support_motion.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor ledge action: nonfinite arithmetic");
    if(std::fpclassify(v)==FP_SUBNORMAL)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
float minimum(float a,float b){return b<a?b:a;}  // min.s
float maximum(float a,float b){return a<b?b:a;}  // max.s
// 0049d038 after static initialization (verified in the boot snapshot).
const float crossfade=bits(0x3f000000U);
// sp+1000-style four-word transition: width = bias = d, rates 1/d (FLT_MAX at 0).
float rate(float d){return d==0.0F?std::numeric_limits<float>::max():check(1.0F/d);}
ActorMotionTransition transition_of(float width,float bias){return {width,bias,rate(width),rate(bias)};}
// Token block built by 0018a910 + explicit stores: ids at +4 + 8k for the
// listed k, receiver = this actor; the others stay {-1, 0}.
struct Tokens {std::array<std::int32_t,7> ids;};
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
    ActorMotionTransitionState& transition;
    std::uint64_t receiver;
    const ActorLedgeActionHost& host;
    ActorLedgeActionResult& result;
    // Direct install: 0018c5a0 name lookup, 003ad500 with the four-word transition.
    void direct(const char* name,MotionInstallMode mode,const Tokens& tokens){
        const auto r=host.resolve(name);
        if(!r.node)return;
        host.install(request(r,transition_of(crossfade,crossfade),tokens,receiver),mode);
        result.motions.push_back(name);
    }
    // Actor submission: the override at +2ef8 is set, then 0018a420 resolves
    // (tag 0 name or tag 2 registration); a resolved request consumes the
    // override and is installed with it. An unresolved one leaves it pending.
    void submit(const std::optional<std::string>& name,const ActorMotionTransition& t,
        MotionInstallMode mode,const std::optional<Tokens>& tokens,std::vector<MotionPhaseHook> hooks={}){
        set_actor_motion_transition(transition,t);
        const auto r=name?host.resolve(*name):host.retained();
        if(!r.node)return;
        const auto used=transition.value;
        transition.pending=false;
        auto q=request(r,used,tokens,receiver);q.phase_hooks=std::move(hooks);
        host.install(q,mode);
        result.motions.push_back(name?*name:std::string());
    }
};
void enable_climb(ActorAnimationReceiverState& actions,ActorPhysicalMode* mode,bool down){
    set_actor_action(actions,1,true);                    // 001898c0 / 00189840 / 001897a0 / 00189740
    if(!mode)return;
    set_actor_physical_mode_bit(*mode,5);set_actor_physical_mode_bit(*mode,3);
    if(down)set_actor_physical_mode_bit(*mode,2);
}
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
}

void set_actor_physical_mode_bit(ActorPhysicalMode& mode,unsigned index){
    mode.bits|=1U<<index;
    std::int32_t top=12;
    while(top>=0&&!bit(mode.bits,static_cast<unsigned>(top)))--top;
    mode.value=top;
}

ActorLedgeActionResult handle_actor_ledge_action(ActorAnimationReceiverState& actions,
    ActorPhysicalMode* pending_mode,ActorMotionTransitionState& transition,std::uint64_t receiver,
    std::uint8_t life_state,float edge_root_weight,const ActorLedgeActionMessage& m,const ActorLedgeActionHost& host){
    ActorLedgeActionResult result;
    if(life_state==2||life_state==3)return result;       // 001ad490: actor virtual +108
    if(check(edge_root_weight)!=0.0F)return result;      // root +3294 +d0
    if(m.kind!=14)return result;
    if(!host.owner_matrix||!host.place_edge_root||!host.bind_support||!host.floor_root_world_position||
       !host.edge_root_world_position||!host.resolve||!host.retained||!host.install||!receiver)
        throw std::runtime_error("Actor ledge action: complete host required");
    const auto mask=m.mask;
    // Unsupported branches reject before any effect.
    const bool ladder=bit(mask,12);
    if(ladder&&(!host.place_ladd_root||(pending_mode&&!host.set_ladder_reference)))
        throw std::runtime_error("Actor ledge action: ladder mount (mask bit 12) is not hosted");
    if(ladder&&!bit(mask,13)&&!bit(mask,14)&&!bit(mask,16)&&!bit(mask,17))
        throw std::runtime_error("Actor ledge action: ladder mount without bits 13/14/16/17 (empty node name, 0018c5a0 index-0 fallback) is not native");
    const bool other=!ladder&&!bit(mask,1)&&!bit(mask,3);
    const bool jump=other&&(bit(mask,5)||bit(mask,6)),air=other&&!jump&&bit(mask,7);
    if(jump&&(!host.edge_root_world_placement||!host.jump_to_grab))
        throw std::runtime_error("Actor ledge action: jump-to-grab (action 0x14, 001914c0) is not hosted");
    if(air&&!host.catch_ledge)throw std::runtime_error("Actor ledge action: air catch (001a1c80) is not hosted");
    // 001930fc..001931a0: object-relative placement to world.
    const bool has_object=m.object.has_value();
    // With a transform component: 1831b0 matrix -> 195ee0 placement, then
    // 195de0 (the 190100 arithmetic) owner * local; otherwise local is world.
    const auto owner=has_object?host.owner_matrix(*m.object):std::nullopt;
    const RootParentTransform owner_placement=owner?actor_support_matrix_placement(*owner):
        RootParentTransform{{0,0,0,1},{0,0,0,1}};
    RootParentTransform world=owner?compose_actor_support_placement(m.placement,owner_placement):m.placement;
    if(bit(mask,16)||bit(mask,17))world.position[2]=sub(world.position[2],1.0F); // 004a0430
    host.place_edge_root(world);
    if(pending_mode){
        // Slot 1 (00193238..001932f4): object, local placement, previous owner
        // (195ee0 of the 1831b0 matrix, or of the identity 0055c9f0 from
        // 0014b210 without a component), enabled byte.
        ActorLedgeSupportBinding b;
        b.object=has_object?*m.object:0;b.local=m.placement;
        b.previous_owner=owner_placement;b.enabled=true;
        host.bind_support(1,b);
    }
    Context c{transition,receiver,host,result};
    using M=MotionInstallMode;
    if(ladder){                                             // 001932f8
        if(pending_mode){                                   // slot 2 like slot 1 (1930c..1933c4)
            ActorLedgeSupportBinding b;
            b.object=has_object?*m.object:0;b.local=m.placement;
            b.previous_owner=owner_placement;b.enabled=true;
            host.bind_support(2,b);
        }
        host.place_ladd_root(world);                        // 001933d0, root +160 = 0
        // Phase hooks [p, p] -> token 0x5c to the actor (0018a210 vector).
        const auto hooks=[&](std::initializer_list<std::uint32_t> phases){
            std::vector<MotionPhaseHook> out;
            for(const auto ph:phases)out.push_back({bits(ph),bits(ph),{0x5c,receiver}});
            return out;
        };
        const char* name;std::vector<MotionPhaseHook> phase;
        if(bit(mask,13)){name="animLadderGetOnTop";phase=hooks({0x3f19999aU,0x3f200000U,0x3f79999aU});}      // 0.6, 0.625, 0.975
        else if(bit(mask,14)){name="animLadderGetOn_LeftHand";phase=hooks({0x3ea8f5c3U,0x3f59999aU});}      // 0.33, 0.85
        else if(bit(mask,16)){name="animLadderGetOnRight_LeftHand";phase=hooks({0x3f19999aU,0x3f266666U});} // 0.6, 0.65
        else{name="animLadderGetOnLeft_LeftHand";phase=hooks({0x3f0ccccdU,0x3f266666U});}                  // 0.55, 0.65
        if(!bit(mask,13))host.place_ladd_root(world);       // placed again for bits 14/16/17
        const auto t=transition_of(crossfade,crossfade);
        c.submit(std::string(name),t,M::replace,Tokens{{0x46,0x47,-1,0x48,-1,0x49,0x4a}},std::move(phase));
        c.submit(std::string("sliderLadderClimbStop_LeftHand"),t,M::queue,Tokens{{0x4b,0x4c,-1,0x4d,-1,0x4e,0x4f}});
        if(pending_mode)host.set_ladder_reference(world.position);   // 00193e7c: +980
        result.handled=true;
        return result;
    }
    if(bit(mask,1)){
        if(bit(mask,8)){                                    // 00193ec0: drop into a hang
            c.direct("animClimbDown8",M::replace,{{0x38,0x39,-1,0x3a,-1,0x3b,0x3c}});
            c.direct("DATA_ID_JumpGrabLedge",M::queue,{{0x14,0x15,-1,0x16,-1,0x17,0x18}});
            c.direct("sliderHang",M::queue,{{0x19,-1,-1,0x1a,-1,0x1b,0x1c}});
        }else{                                              // 001945c8: climb down
            const auto t=transition_of(crossfade,crossfade);
            c.submit(std::string(bit(mask,2)?"animClimbDown8":"animClimbDown4"),t,M::replace,
                Tokens{{0x33,0x34,-1,0x35,-1,0x36,0x37}});
            c.submit(std::nullopt,t,M::queue,std::nullopt);
        }
        enable_climb(actions,pending_mode,true);
    }else if(bit(mask,3)){                                  // 00194bc8: climb up
        const float flor=check(host.floor_root_world_position()[2]);
        const float edge=check(host.edge_root_world_position()[2]);
        const float height=sub(edge,flor);
        if(height<bits(0x40900000U)){                      // 004a03b8 = 4.5
            c.submit(std::string("DATA_ID_Climb4"),transition_of(crossfade,crossfade),M::replace,
                Tokens{{0x29,0x2a,-1,0x2b,-1,0x2c,0x2d}});
        }else{
            // 001951b0: function statics 4.0 (0049d180), 7.0 (0049d188) and
            // 1/(7-4) (0049d190); bias = min(min(0.5, 0.5(1 - 2t)), 0.5).
            const float scale=check(1.0F/sub(7.0F,4.0F));
            const float t=minimum(maximum(mul(scale,sub(height,4.0F)),0.0F),1.0F);
            const float shaped=mul(crossfade,sub(1.0F,mul(2.0F,t)));
            const float bias=minimum(minimum(crossfade,shaped),crossfade);
            c.submit(std::string("DATA_ID_HangClimbUp"),transition_of(crossfade,bias),M::replace,
                Tokens{{0x29,0x2a,-1,0x2b,-1,0x2c,0x2d}});
        }
        c.submit(std::nullopt,transition_of(crossfade,crossfade),M::queue,std::nullopt);
        enable_climb(actions,pending_mode,false);
    }else if(jump){                                         // 00195840: action 0x14 to self
        host.jump_to_grab(host.edge_root_world_placement());
    }else if(air){                                          // 001958c8
        host.catch_ledge();
    }
    result.handled=true;
    return result;
}
}
