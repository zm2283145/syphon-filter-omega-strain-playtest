#include "actor_object_action.hpp"
#include "actor_ledge_action.hpp"
#include "game_libm.hpp"
#include "player_camera.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor object action: nonfinite arithmetic");
    if(std::fpclassify(v)==FP_SUBNORMAL)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
// 0049d038 after static initialization (verified in the boot snapshot).
const float crossfade=bits(0x3f000000U);
float rate(float d){return d==0.0F?std::numeric_limits<float>::max():check(1.0F/d);}
ActorMotionTransition transition_of(float width,float bias){return {width,bias,rate(width),rate(bias)};}
constexpr RootAnchorKey flor{std::uint8_t{'f'},std::uint8_t{'l'},std::uint8_t{'o'},std::uint8_t{'r'}}; // 0049d040
constexpr RootAnchorKey edge{std::uint8_t{'e'},std::uint8_t{'d'},std::uint8_t{'g'},std::uint8_t{'e'}}; // 0049d048
bool action_bit(const ActorAnimationReceiverState& s,unsigned b){return ((s.active_actions>>b)&1U)!=0;}
}

NamedObjectActionMessage build_named_object_action(std::int32_t action,std::optional<std::uint64_t> object,
    std::string_view name,float time,bool byte_64){
    NamedObjectActionMessage m;
    m.object=object;
    if(time==-1.0F)m.time=-1.0F;                                           // c.eq.s *00493938
    else if(time==-2.0F)m.time=bits(0x7f7fffffU);                          // c.eq.s *00493940
    else m.time=time;
    // 001692d0's c_str ends at the first NUL.
    std::string copy(name.substr(0,name.find('\0')));
    if(!copy.empty()){
        m.action=0x25;                                                     // 003e69e8
        for(char& c:copy){
            const auto b=static_cast<std::int8_t>(c);                      // lb: signed
            if(b>=0x41&&b<0x5b)c=static_cast<char>(b+0x20);
        }
    }else m.action=action;
    for(std::size_t i=0;i<47&&i<copy.size();++i)m.name[i]=copy[i];
    m.byte_64=byte_64?1:0;
    return m;
}

ActorObjectActionMessage build_object_action(std::int32_t action,std::optional<std::uint64_t> object,float time,std::uint8_t byte_38){
    ActorObjectActionMessage m;
    m.action=static_cast<std::int8_t>(action&0xff);                       // dsll32/dsra32 by 0x18
    m.kind=7;m.object=object;m.motion_id=-1;m.byte_38=byte_38;
    if(time==-1.0F)m.time=-1.0F;                                           // c.eq.s *00493938
    else if(time==-2.0F)m.time=bits(0x7f7fffffU);                          // c.eq.s *00493940
    else m.time=time;
    return m;
}

PerformActionMessage perform_action_message(std::uint32_t action_word,std::optional<std::string_view> name,float time){
    const auto action=static_cast<std::int32_t>(static_cast<std::int8_t>(action_word&0xffU));   // lb +4
    if(!name||name->empty()||(*name)[0]=='\0')return build_object_action(action,std::nullopt,time,0);
    return build_named_object_action(0x25,std::nullopt,*name,time,false);
}

std::optional<ActorObjectActionMessage> named_object_action_resend(std::uint32_t action_word,std::uint8_t kind,
    std::optional<std::uint64_t> object,const std::array<char,48>& name,float time,std::uint8_t byte_64,
    const std::function<std::optional<std::uint32_t>(const std::array<char,48>&)>& find){
    if(action_word!=0x25U&&action_word!=0x26U&&action_word!=0x27U)return std::nullopt;   // lw +24
    if(kind!=0xaU)return std::nullopt;                                                   // lbu +28
    std::int32_t id=-1;                                                                  // 0018d3f8
    if(name[0]!=0){
        const auto found=find(name);                                                     // 0018c740
        if(!found){
            const std::string text(name.data(),static_cast<std::size_t>(std::find(name.begin(),name.end(),'\0')-name.begin()));
            throw std::runtime_error("Actor object action: named motion not in the model (0018c740 id-0 fallback): "+text);
        }
        if(*found>static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("Actor object action: motion node id outside the message range");
        id=static_cast<std::int32_t>(*found);
    }
    ActorObjectActionMessage m;
    m.action=static_cast<std::int8_t>(action_word&0xffU);                               // dsll32/dsra32 of lb +24
    m.kind=7;m.object=object;m.motion_id=id;m.time=time;m.byte_38=byte_64;
    return m;
}

std::array<RootVector,3> actor_object_node_rows(float yaw){
    const float s=check(game_sinf(check(yaw))),c=check(game_cosf(check(yaw)));
    // 00132130 for the unit axis (0, 0, 1), every product and sum kept (the
    // off-axis lanes are 0 - s*z and friends, so a zero sine gives +0).
    constexpr float x=0.0F,y=0.0F,z=1.0F;
    const float k=check(1.0F-c);
    const auto mul=[](float a,float b){return check(a*b);};
    const float xy=mul(mul(y,k),x),yz=mul(mul(z,k),y),zx=mul(mul(x,k),z);
    return {RootVector{check(c+mul(mul(k,x),x)),check(xy+mul(s,z)),check(zx-mul(s,y)),0.0F},
            RootVector{check(xy-mul(s,z)),check(c+mul(mul(k,y),y)),check(yz+mul(s,x)),0.0F},
            RootVector{check(zx+mul(s,y)),check(yz-mul(s,x)),check(c+mul(mul(k,z),z)),0.0F}};
}

ActorObjectActionResult handle_actor_object_action(ActorAnimationReceiverState& actions,
    ActorMotionTransitionState& transition,std::uint64_t receiver,const ActorObjectActionMessage& m,
    const ActorObjectActionTarget& target,const ActorObjectActionHost& host){
    if(m.action!=0x25&&m.action!=0x26&&m.action!=0x27)
        throw std::runtime_error("Actor object action: action is not 0x25/0x26/0x27");
    if(!host.anchor_key||!host.place_edge_root||!host.floor_root_world_placement||!host.resolve||
       !host.retained||!host.install||!host.reject||!receiver)
        throw std::runtime_error("Actor object action: complete host required");
    ActorObjectActionResult result;
    // Ability gate: priority actions 27, 31 and 23 must all be clear.
    if(action_bit(actions,27)||action_bit(actions,31)||action_bit(actions,23)){
        if(m.object)host.reject(*m.object);
        result.rejected=true;result.handled=true;
        return result;
    }
    if(m.kind!=5&&m.kind!=7)throw std::runtime_error("Actor object action: message kind is not 5/7");
    if(m.motion_id<-1)throw std::runtime_error("Actor object action: negative motion id");
    const bool by_id=m.motion_id!=-1;
    if(by_id&&(!host.anchor_key_by_id||!host.resolve_id))
        throw std::runtime_error("Actor object action: motion id hosting (0018c6b0 / tag 1) required");
    float time=-1.0F;bool byte_38=false;
    if(m.kind==7){
        const float t=check(m.time);
        // cvt.w.s: truncation, saturating to 0x7fffffff / 0x80000000 (EE goldens: tests/band_damage_zero_goldens.hpp,
        // bullet_hit.cpp). 132ms5: a script time of -2.0 (BELARUS2's STONE_SLASHED_WITH_KNIFE / STONE_SITTING_IDLE)
        // is sent as FLT_MAX (003e6910's c.eq.s *00493940), which saturates.
        const std::int32_t w=t>=2147483648.0F?std::numeric_limits<std::int32_t>::max():
                             t<=-2147483648.0F?std::numeric_limits<std::int32_t>::min():static_cast<std::int32_t>(t);
        time=static_cast<float>(w);
        byte_38=m.byte_38!=0;
    }
    const auto id=static_cast<std::uint32_t>(m.motion_id);
    const std::string name=by_id?"#"+std::to_string(id):m.action==0x27?"dataCheckBodyForPulse":"DATA_ID_PushElevatorButton";
    const auto leaf=by_id?host.anchor_key_by_id(id):host.anchor_key(name);   // 0018da90 / 0018dad8
    const RootAnchorKey anchor=leaf?*leaf:edge;    // 001914a0 non-leaf: default 'edge'
    const bool has_node=m.object&&target.node;
    if(anchor==flor&&has_node)throw std::runtime_error("Actor object action: 'flor' anchor at a node (00192fe0) is not hosted");
    if(anchor!=flor&&anchor!=edge)throw std::runtime_error("Actor object action: motion anchor other than 'flor'/'edge' is not hosted");
    const bool tail_push=m.object&&target.world_object&&actions.byte_14&&!actions.byte_326b;
    if(tail_push&&(!host.controller_is_player||!host.push_world_object))
        throw std::runtime_error("Actor object action: world object push hosting is unresolved");
    // Node matrix (0015c170) before any write.
    std::optional<RootParentTransform> node_placement;
    if(has_node){
        const auto rows=actor_object_node_rows(target.node->yaw);
        node_placement=RootParentTransform{camera_matrix_to_quaternion(rows),
            {check(target.node->position[0]),check(target.node->position[1]),check(target.node->position[2]),1.0F}};
    }
    set_actor_action(actions,20,true);
    actions.flags_324c=(actions.flags_324c&~1U)|(byte_38?1U:0U);
    std::int32_t s1=0,s0=0;
    if(anchor==edge){
        if(node_placement){host.place_edge_root(*node_placement);s1=0x63;s0=0x66;}
        else{host.place_edge_root(host.floor_root_world_placement());s1=0x64;s0=0x65;}
    }else{s1=0x63;s0=0x65;}
    result.anchor=anchor;
    const auto t=transition_of(crossfade,crossfade);
    const auto submit=[&](const std::optional<std::string>& motion,MotionInstallMode mode,bool primary){
        set_actor_motion_transition(transition,t);
        const auto r=!motion?host.retained():primary&&by_id?host.resolve_id(id):host.resolve(*motion);
        if(!r.node)return;
        const auto used=transition.value;
        transition.pending=false;
        MotionRequest q;q.node=*r.node;q.selector=r.selector;
        q.width=used.width;q.bias=used.bias;q.rate=used.rate;q.secondary_rate=used.secondary_rate;
        q.limit=primary?time:-1.0F;q.initial_phase=0;
        if(primary){
            const std::array<std::int32_t,7> ids{s1,s0,-1,0x68,-1,0x69,0x6a};
            q.notifications_active=true;
            for(std::size_t k=0;k<7;++k)if(ids[k]>=0)q.notifications[k]={ids[k],receiver};
            q.phase_hooks.push_back({crossfade,crossfade,{0x67,receiver}});
        }
        host.install(q,mode);
        result.motions.push_back(motion?*motion:std::string());
    };
    submit(name,MotionInstallMode::replace,true);
    submit(std::nullopt,MotionInstallMode::queue,false);
    // 0018fe68 tail.
    actions.controller_3474=m.object?*m.object:0;
    if(tail_push&&host.controller_is_player()){
        host.push_world_object(target.world_object);
        actions.byte_326b=1;result.pushed_world_object=true;
    }
    result.handled=true;
    return result;
}

namespace {
// The idle, follower and leader branches of 0018d2c0, by action.
struct BuddyClimbBranch {
    const char* first;                       // the replacing direct install
    std::array<std::int32_t,7> tokens;
    bool idle{},leader{};
};
BuddyClimbBranch buddy_branch(std::int32_t action){
    switch(action){
    case 0x31:return {"animBuddyClimbIdle",{0x6b,0x6d,-1,0x6e,-1,0x6f,0x70},true,false};               // 0049dc70
    case 0x35:return {"animBuddyClimbIdle_SecondHalf",{0x6c,0x6d,-1,0x6e,-1,0x6f,0x70},true,false};    // 0049dc90
    case 0x33:return {"buddyclimb_secondguyup",{0x3d,0x3e,-1,0x3f,-1,0x40,0x41},false,false};          // 0049dcb0
    case 0x37:return {"animBuddyClimbFollower_SecondHalf",{0x3d,0x3e,-1,0x3f,-1,0x40,0x41},false,false}; // 0049dd00
    case 0x34:return {"buddyclimb_firstguyup",{0x3d,0x42,-1,0x43,-1,0x44,0x45},false,true};            // 0049dd50
    case 0x38:return {"animBuddyClimbLeader_SecondHalf",{0x3d,0x42,-1,0x43,-1,0x44,0x45},false,true};  // 0049dcd0
    default:throw std::runtime_error("Actor buddy climb: action is not 0x31/0x33/0x34/0x35/0x37/0x38");
    }
}
MotionRequest direct_request(const ActorMotionRegistration& r,const std::array<std::int32_t,7>* tokens,std::uint64_t receiver){
    const auto t=transition_of(crossfade,crossfade);
    MotionRequest q;q.node=*r.node;q.selector=r.selector;
    q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;
    q.limit=-1;q.initial_phase=0;                                          // f12 = -1, f13 = 0
    if(tokens){
        q.notifications_active=true;
        for(std::size_t k=0;k<7;++k)if((*tokens)[k]>=0)q.notifications[k]={(*tokens)[k],receiver};
    }
    return q;
}
}

ActorObjectActionResult handle_actor_buddy_climb_action(ActorAnimationReceiverState& actions,ActorPhysicalMode* pending_mode,
    std::uint64_t receiver,const ActorObjectActionMessage& m,const ActorObjectActionTarget& target,const ActorObjectActionHost& host){
    const auto branch=buddy_branch(m.action);
    if(!host.place_edge_root||!host.resolve||!host.retained||!host.install||!host.reject||!receiver)
        throw std::runtime_error("Actor buddy climb: complete host required");
    ActorObjectActionResult result;
    if(action_bit(actions,27)||action_bit(actions,31)||action_bit(actions,23)){
        if(m.object)host.reject(*m.object);
        result.rejected=true;result.handled=true;
        return result;
    }
    if(m.kind!=5&&m.kind!=7)throw std::runtime_error("Actor buddy climb: message kind is not 5/7");
    if(m.motion_id!=-1)throw std::runtime_error("Actor buddy climb: an explicit motion id (not sent by 00216230) is not hosted");
    const bool tail_push=m.object&&target.world_object&&actions.byte_14&&!actions.byte_326b;
    if(tail_push&&(!host.controller_is_player||!host.push_world_object))
        throw std::runtime_error("Actor buddy climb: world object push hosting is unresolved");
    std::optional<RootParentTransform> node_placement;
    if(m.object&&target.node){                                             // 0015c170 before any write
        const auto rows=actor_object_node_rows(target.node->yaw);
        node_placement=RootParentTransform{camera_matrix_to_quaternion(rows),
            {check(target.node->position[0]),check(target.node->position[1]),check(target.node->position[2]),1.0F}};
    }
    // Direct installs (0018c5a0 then 003ad500 with the {0.5, 0.5} transition;
    // the actor override +2ef8 is not touched; an unresolved name is skipped).
    const auto named=[&](const char* name,MotionInstallMode mode,const std::array<std::int32_t,7>& tokens){
        const auto r=host.resolve(name);
        if(!r.node)return;
        host.install(direct_request(r,&tokens,receiver),mode);
        result.motions.push_back(name);
    };
    named(branch.first,MotionInstallMode::replace,branch.tokens);
    if(branch.leader){
        // 0018fae4..0018fc34: +326c's node and selector, queued, token block inactive.
        const auto r=host.retained();
        if(!r.node)throw std::runtime_error("Actor buddy climb: the retained motion +326c has no node (read unchecked at 0018fc2c)");
        host.install(direct_request(r,nullptr,receiver),MotionInstallMode::queue);
        result.motions.push_back(std::string());
    }else if(!branch.idle){
        named("DATA_ID_JumpGrabLedge",MotionInstallMode::queue,{0x14,0x15,-1,0x16,-1,0x17,0x18});    // 0049dd30
        named("sliderHang",MotionInstallMode::queue,{0x19,-1,-1,0x1a,-1,0x1b,0x1c});                // 0049dcf0
    }
    if(node_placement){
        host.place_edge_root(*node_placement);                             // 00191100, edge +160 = 0
        result.anchor=edge;
        if(branch.idle&&pending_mode)set_actor_physical_mode_bit(*pending_mode,5);   // 0018ed9c: +3310 +2c |= 0x20
    }
    // 0018fe68 tail.
    actions.controller_3474=m.object?*m.object:0;
    if(tail_push&&host.controller_is_player()){
        host.push_world_object(target.world_object);
        actions.byte_326b=1;result.pushed_world_object=true;
    }
    result.handled=true;
    return result;
}

ActorObjectActionResult handle_actor_buddy_climb_cancel(const ActorAnimationReceiverState& actions,std::uint64_t receiver,
    const ActorObjectActionMessage& m,const ActorObjectActionHost& host){
    if(m.action!=0x32&&m.action!=0x36)throw std::runtime_error("Actor buddy climb cancel: action is not 0x32/0x36");
    if(!host.retained||!host.install||!receiver)throw std::runtime_error("Actor buddy climb cancel: complete host required");
    ActorObjectActionResult result;result.handled=true;
    if(action_bit(actions,27)||action_bit(actions,31))return result;       // 001ad3a8..001ad3c0
    const auto r=host.retained();                                          // 1ae270 / 1ae260 on +326c
    if(!r.node)throw std::runtime_error("Actor buddy climb cancel: the retained motion +326c has no node (read unchecked at 001ad410)");
    host.install(direct_request(r,nullptr,receiver),MotionInstallMode::replace);
    result.motions.push_back(std::string());
    return result;
}
}
