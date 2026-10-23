#include "npc_follow.hpp"
#include "npc_alert.hpp"
#include "npc_perception_think.hpp"
#include "npc_script_control.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("NPC AI mode 9 (00153870): "+what);}
template <class F> const F& need(const F& f,const char* what){if(!f)fail(std::string("the host does not supply ")+what);return f;}
// VU results flush denormals to zero (sign kept); each product and sum is
// one binary32 operation (VU_ROUNDING.md).
float flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float vmul(float a,float b){return flush(a*b);}
float vadd(float a,float b){return flush(a+b);}
float vsub(float a,float b){return flush(a-b);}
// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w with vf0.w = 1: (x*x + y*y) + z*z.
float sum_squares(const RootVector& v){return vadd(vadd(vmul(v[0],v[0]),vmul(v[1],v[1])),vmul(v[2],v[2]));}
// vsqrt Q, then vaddq.x vf4 = vf0.x + Q.
float vu_length(const RootVector& v){return vadd(0.0F,flush(vu::sqrt(sum_squares(v))));}
// vaddx.xyz vf1, vf1, vf0 with the MAC zero flags of x, y and z (0xe).
bool zero_xyz(const RootVector& v){return vadd(v[0],0.0F)==0.0F&&vadd(v[1],0.0F)==0.0F&&vadd(v[2],0.0F)==0.0F;}
// The inline normalization: length = vf4.x, Q = vf0.w / length (vdiv; a
// zero length gives +FLT_MAX, the interpreter's zero divisor), xyz *= Q.
float normalize_in_place(RootVector& v){
    const float length=vu_length(v);
    const float q=length==0.0F?std::numeric_limits<float>::max():flush(vu::div(1.0F,length));
    for(int i=0;i<3;++i)v[i]=vmul(v[i],q);
    return length;
}
// +100..+10c as the vector of the vmaddw chain (the two words are floats' bits).
RootVector offset_of(const NpcAiComponent& c){
    return {c.follow_100,c.follow_104,std::bit_cast<float>(c.follow_108),std::bit_cast<float>(c.follow_10c)};
}
// The target region and the goal (00153870..00153a64), the common part of every message.
struct Setup {
    std::uint64_t target{};
    RootVector goal{};
    std::int32_t region{-1};
    NpcSenseObject view;
};
// 0015fa70(+1b0, goal, region): its byte is the handler's result.
bool route_point(const Setup& s,NpcFollowHost& f){
    const auto r=need(f.route_to_point,"0015fa70 (NpcFollowHost::route_to_point)")(s.goal,s.region);
    if(!r)fail("0015fa70 returned an unset stack byte (0015efb0 updated a queued request in place)");
    return *r!=0;
}
void set_speed_50(NpcAiComponent& c,float v){
    if(!(c.speed_priority_64<51))return;
    c.travel_speed_60=v;c.speed_priority_64=50;c.flags_141=static_cast<std::uint8_t>(c.flags_141&~0x40U);
}
}

std::shared_ptr<NpcRouteBuffer> npc_new_route_buffer(const NpcPathFollower& pf){
    auto b=std::make_shared<NpcRouteBuffer>();
    b->destination_10=-1;b->start_14=-1;b->array_20=false;b->capacity_24=pf.capacity_78;b->nodes.clear();   // 00161930
    return b;
}

void npc_follow_point(NpcPathFollower& pf,const NpcNodeGraph& graph,const RootVector& point,const NpcPathHost& host){
    if(!pf.route_74)return;                                                  // 0015fbe8
    auto& b=*pf.route_74;
    b.nodes.clear();                                                         // 0015fbf0..0015fbfc: +28 = 0 when positive
    b.point_00=point;b.destination_10=-1;                                    // 0015fc14 / 0015fc1c
    pf.current_50=-1;                                                        // 0015fc20
    pf.index_48=0;                                                           // 0015fc2c (+28, now 0)
    pf.field_58=-1;                                                          // 0015fc3c (+10)
    const bool segment=step_npc_route(pf,graph,host);                        // 0015fc38 00160a00
    pf.flags_60=static_cast<std::uint8_t>((pf.flags_60&~0x08U)|(segment?0U:0x08U));
    pf.saved_20=point;pf.target_00=point;                                    // 0015fc68..0015fc74
    if(!host.actor_translation)throw std::runtime_error("NPC path: 0015fbd0 needs the actor's +f4 (NpcPathHost::actor_translation)");
    const RootVector t=host.actor_translation();                             // 0015fc80 virtual +f4 row 3
    for(int i=0;i<4;++i)pf.direction_10[i]=vsub(point[i],t[i]);              // 0015fca0 vsub.xyzw
}

bool npc_follow_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(!host.follow)fail("mode 9 needs its host (NpcAiHost::follow)");
    auto& f=*host.follow;
    if(!host.object_by_id)fail("the target lookup 0040c540 (NpcAiHost::object_by_id)");
    Setup s;
    s.target=host.object_by_id(c.object_f4);                                 // 001538b4 0040c540(&+f4)
    if(s.target==0){                                                         // 00154600 virtual +e0
        npc_resume_ai_mode(c,host);
        return false;
    }
    if(!host.sense||!host.sense->object)fail("the target's view (NpcAiHost::sense)");
    {
        const auto v=host.sense->object(s.target);
        if(!v)fail("no view of the target");
        s.view=*v;
    }
    const auto& world_f4=need(f.world_f4,"the +f4 matrix (NpcFollowHost::world_f4)");
    {
        const auto mtx=world_f4(s.target);                                   // 001538d4 virtual +f4
        const RootVector o=offset_of(c);
        RootVector g{};
        for(int i=0;i<4;++i){                                                // 00153908..00153914
            float acc=vmul(mtx[0][i],o[0]);
            acc=vadd(acc,vmul(mtx[1][i],o[1]));
            acc=vadd(acc,vmul(mtx[2][i],o[2]));
            g[i]=vadd(acc,vmul(mtx[3][i],o[3]));
        }
        const auto t=world_f4(s.target)[3];                                  // 00153934 virtual +f4 again
        for(int i=0;i<4;++i)s.goal[i]=vadd(g[i],t[i]);                       // 00153954 vadd.xyzw
    }
    using C=NpcSenseObject::Controller;
    if(s.view.controller==C::player){                                        // 00153960..001539ac
        if(!host.sense->player_region)fail("004ed9c0 (NpcPerceptionHost::player_region)");
        if(s.view.team<0)fail("004ed9c0 indexed by a negative 003cc950");
        s.region=host.sense->player_region(s.view.team);
    }else if(s.view.controller==C::ai){                                      // 001539b0..00153a30
        const std::int32_t hold=need(f.hold_4c,"an AI target's path follower +4c (NpcFollowHost::hold_4c)")(s.target);
        s.region=s.view.region_5c;
        if(hold>=0&&hold!=c.path.hold_4c&&hold!=c.path.previous_54){
            c.path.hold_4c=hold;                                             // 00153a04
            if(f.self_14!=0){                                                // 00153a0c: actor +14
                NpcAiSend n;n.kind=NpcAiSend::Kind::network;n.transport=0;n.key=npc_network_message;
                n.action=0x2d;n.action_kind=4;n.node=hold;                   // 00153a24 002d7290(actor, 0x2d, 4, node, 0, 0, 0)
                need(host.send,"the sends (NpcAiHost::send)")(n);
            }
        }
    }else{                                                                   // 00153a38..00153a64
        // f20 here is the one 00153870 was entered with (saved at 001538a8,
        // not yet written): the sender's (NpcAiHost::sender_f20; the base
        // update's dt for its enter, 132uk6d; nullopt elsewhere).
        s.region=need(f.region_at,"0015d790 (NpcFollowHost::region_at)")(s.view.position,c.path.destination_5c,host.sender_f20,
            "00153a5c: mode 9's entry f20 (its message caller's)");
    }

    if(m.kind==NpcAiMessage::Kind::ai){
        if(m.event==2){
            const auto& own=need(host.actor.world_matrix,"the actor's +f4")();   // 00153aac
            RootVector dir{};
            for(int i=0;i<4;++i)dir[i]=vsub(s.goal[i],own[3][i]);            // 00153acc vsub.xyzw
            dir[3]=1.0F;                                                     // 00153b04 sw 1.0, +2fc
            float distance=0.0F;                                             // 00153a98 +31c
            if(!zero_xyz(dir))distance=normalize_in_place(dir);              // 00153b0c..00153b7c
            float target_speed=0.0F,own_speed;                               // 00153b84 +318
            if(s.view.actor_class){                                          // 00153b8c *0049d010
                const auto& speed=need(f.speed,"+3584 / +30e0 (NpcFollowHost::speed)");
                target_speed=speed(s.target);own_speed=speed(f.self);
            }else{
                const auto& velocity=need(f.velocity_74,"+58 virtual +74 (NpcFollowHost::velocity_74)");
                auto v=velocity(s.target);                                   // 00153bf4
                if(!v)fail("the target's +58 is null (00153be8 calls its virtual +74 through null)");
                if(!zero_xyz(*v))target_speed=normalize_in_place(*v);
                const auto w=velocity(f.self);                               // 00153c98
                if(!w)fail("the actor's +58 is null (00153c88)");
                own_speed=vu_length(*w);                                     // 00153ca4..00153cc8, no zero test
            }
            bool clear=true;                                                 // 00153d48
            if(npc_sense_present(s.view)){                                   // 00153cd0..00153d10
                if(!host.sense->visibility)fail("0022cb10 (NpcPerceptionHost::visibility)");
                clear=host.sense->visibility->blocker(f.self,s.target)==0;   // 00153d2c 0022cb10(004f77c0, actor, target, 0, 0)
            }
            if(c.mode_counter_ec!=3&&(!(target_speed<=0.0F)||(c.path.flags_60&0x08U)==0)){   // 00153d50..00153d88
                const auto row1=world_f4(s.target)[1];                       // 00153d9c
                RootVector lead{};
                for(int i=0;i<4;++i)lead[i]=vadd(s.goal[i],vmul(row1[i],10.0F));   // 00153dc4 vmulx, 00153dec vadd
                if(!host.graph)fail("0015fbd0 needs the node collection");
                NpcPathHost ph;
                ph.actor_translation=[&host]{return host.actor.world_matrix()[3];};
                ph.send=[&host](const NpcPathEvent& e){
                    NpcAiSend n;n.kind=NpcAiSend::Kind::component;n.transport=0;n.message={NpcAiMessage::Kind::path,e.event,e.value};
                    host.send(n);
                };
                npc_follow_point(c.path,*host.graph,lead,ph);                // 00153e0c 0015fbd0(+1b0, lead, region)
            }
            if(distance<=1.0F){c.mode_counter_ec=0;c.carry_node_f0=s.region;}   // 00153e28..00153e3c
            else{
                const float reach=6.0F+vu_length(offset_of(c));                 // 00153e40..00153e80
                if(distance<=reach||c.mode_counter_ec==3){                       // 00153fd8
                    if(!(target_speed<=0.0F)&&clear){
                        const auto row1=world_f4(s.target)[1];                   // 00154004
                        const float dot=vadd(vadd(vmul(dir[0],row1[0]),vmul(dir[1],row1[1])),vmul(dir[2],row1[2]));   // 00154024..00154038
                        c.mode_counter_ec=dot<-0.5F?1:2;
                    }
                }else if(clear)c.mode_counter_ec=2;                              // 00153fc8
                else{
                    c.mode_counter_ec=3;                                         // 00153eb0
                    // f20 = own_speed (00153bd0 / 00153bd8 or 00153cc8; nothing writes it before 00153ee0).
                    c.carry_node_f0=need(f.region_at,"0015d790 (NpcFollowHost::region_at)")(
                        need(f.position_e8,"the actor's +e8 (NpcFollowHost::position_e8)")(f.self),c.carry_node_f0,own_speed,
                        "00153ee0: the actor's own speed");   // 00153ee0
                    const std::int32_t start=c.path.hold_4c;                     // 00153ef4
                    if(!c.path.route_74)c.path.route_74=npc_new_route_buffer(c.path);   // 00153ef8..00153f88
                    need(f.request_route,"0015efb0 (NpcFollowHost::request_route)")(start,c.carry_node_f0,s.region,s.goal,c.path.route_74);
                }
            }
            float s21=target_speed;                                          // 0015407c
            if(c.mode_counter_ec==2||c.mode_counter_ec==3)s21=2.0F*s21+npc_travel_speed(2);   // 00154090..001540b4
            if(c.mode_counter_ec==1){                                        // 001540c8..001540fc
                s21=0.5F*(s21-npc_travel_speed(1));
                if(s21<0.0F)s21=0.0F;
            }
            bool stop=false;
            constexpr float still=std::bit_cast<float>(0x3727c5acU);         // 1e-5
            if(target_speed<=still){                                         // 00154114
                if(c.mode_counter_ec==0||c.mode_counter_ec==1)stop=true;     // 00154138
                else if(std::isnan(distance))fail("a NaN distance at the double compare 00100260 (VU lengths are never NaN)");
                else if(static_cast<double>(distance)>6.0){                  // 00154140 0011bd70, 00100260
                    if(c.travel_speed_60<npc_travel_speed(2))set_speed_50(c,npc_travel_speed(3));   // 00154160..001541e0
                }else set_speed_50(c,npc_travel_speed(1));                   // 001541e8
            }else if(own_speed<=still)set_speed_50(c,npc_travel_speed(1));   // 00154238
            else{
                const float ratio=own_speed/s21;                             // 0015429c div.s
                if(ratio<std::bit_cast<float>(0x3f666666U)){                 // 001542ac (0.9)
                    set_speed_50(c,c.travel_speed_60+std::bit_cast<float>(0x3ccccccdU));   // 001542b8..00154308
                    if(!(c.travel_speed_60<=1.0F))set_speed_50(c,1.0F);      // 00154310..0015436c
                }
                if(!(ratio<=std::bit_cast<float>(0x3f8ccccdU))){             // 00154380 (1.1); 0015438c..001543a4 reach 001543a8 for every +ec
                    set_speed_50(c,c.travel_speed_60-std::bit_cast<float>(0x3ccccccdU));   // 001543a8..001543f8
                    if(c.travel_speed_60<0.0F)set_speed_50(c,0.5F*npc_travel_speed(1));    // 00154400..00154474
                }
            }
            if(stop){if(c.state_13c!=0)set_npc_movement_state(c,0,20,host);}  // 00154478..00154494
            else if(c.state_13c==0)set_npc_movement_state(c,1,20,host);        // 001544a0..001544b4
            return true;
        }
        if(m.event==3)return route_point(s,f);
        if(m.event==0){                                                      // 001544e8..00154528
            c.mode_counter_ec=0;c.timer_e8=-1;
            need(f.release_routes,"00163c50 (NpcFollowHost::release_routes)")();
            c.carry_node_f0=c.path.destination_5c;
            return true;
        }
        if(m.event==1){                                                      // 00154530..00154570
            if(c.speed_priority_64<51)c.speed_priority_64=50;
            npc_follow(c,std::nullopt,0.0F,0.0F);                            // virtual +fc(0, 0, 0) = 001488f0
            return true;
        }
        return false;
    }
    if(m.kind==NpcAiMessage::Kind::path){                                    // 00154578 004ed9b8
        if(m.event==5||m.event==6)return c.mode_counter_ec==3?route_point(s,f):false;   // 001545b0..001545d4
        if(m.event==1||m.event==3)c.timer_e8=30;                             // 001545f0
        return false;
    }
    return false;
}
}
