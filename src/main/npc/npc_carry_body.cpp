#include "npc_carry_body.hpp"
#include "npc_alert.hpp"
#include "npc_perception_think.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("NPC AI mode 8 (00153150): "+what);}
template <class F> const F& need(const F& f,const char* what){if(!f)fail(std::string("the host does not supply ")+what);return f;}
std::uint8_t byte_of(const std::optional<std::uint8_t>& r,const char* site){
    if(!r)fail(std::string(site)+" returned an unset stack byte (0015efb0 updated a queued request in place)");
    return *r;
}
// 003c9e30(msg, 0055d480), vtable 004e01a0, +24 = action, +28 = 5, +2c = +f4, +20 = 1; 003c9420(msg, +30, 0).
void send_action(NpcAiComponent& c,std::int32_t action,NpcAiHost& host){
    NpcAiSend s;s.kind=NpcAiSend::Kind::action;s.transport=0;s.action=action;s.action_kind=5;s.word_2c=c.object_f4;
    need(host.send,"the sends (NpcAiHost::send)")(s);
}
// Event 3 with +ec != 1 (001531e0..00153344): the route toward the body.
std::uint8_t route_to_body(NpcAiComponent& c,NpcAiHost& host,NpcCarryHost& k){
    const auto body=need(host.object_by_id,"0040c540 (NpcAiHost::object_by_id)")(c.object_f4);   // 001531e8
    if(body==0)return 0;
    c.timer_e8=3;                                                            // 00153204
    if(!host.sense||!host.sense->object)fail("the body's view (NpcAiHost::sense)");
    const auto view=host.sense->object(body);
    if(!view)fail("no view of the body");
    if(view->life_108!=3)return 1;                                           // 00153210 virtual +108
    using C=NpcSenseObject::Controller;
    std::int32_t region=-1,node=-1;
    if(view->controller==C::player){                                         // 00153240 +54, 00153260 003cc950
        if(!host.sense->player_region)fail("004ed9c0 (NpcPerceptionHost::player_region)");
        if(view->team<0)fail("004ed9c0 indexed by a negative 003cc950");
        region=host.sense->player_region(view->team);
    }else if(view->controller==C::ai){                                       // 0015328c +58
        region=view->region_5c;                                              // 001532cc +1b0 +5c
        const auto hold=need(k.hold_4c,"the body's path follower +4c (NpcCarryHost::hold_4c)")(body);   // 001532c8 +18c
        if(hold>=0)node=hold;                                                // 001532d8 node +78
    }else fail("a body without a player or AI controller (+58 null: 00153284 calls its virtual +58 through null)");
    const auto point=need(k.head_row3,"00133ed0 (NpcCarryHost::head_row3)")(body);   // 001532e0..0015330c
    c.mode_counter_ec=0;                                                     // 00153314
    std::uint8_t r=1;
    if(node>=0)(void)need(k.route_to_node,"0015fb20 (NpcCarryHost::route_to_node)")(node);   // 0015331c, its byte not read
    else r=byte_of(need(k.route_to_point,"0015fa70 (NpcCarryHost::route_to_point)")(point,region),"0015fa70");   // 00153334
    c.timer_e8=-1;                                                           // 00153344
    return r;
}
}

bool npc_carry_body_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host){
    if(m.kind==NpcAiMessage::Kind::ai){
        if(m.event==3){
            if(!host.carry)fail("mode 8 needs its host (NpcAiHost::carry)");
            auto& k=*host.carry;
            std::uint8_t r;
            if(c.mode_counter_ec==1){                                        // 001531b0
                c.mode_counter_ec=2;
                r=byte_of(need(k.route_to_node,"0015fb20 (NpcCarryHost::route_to_node)")(c.carry_node_f0),"0015fb20");   // 001531c8
                c.timer_e8=-1;
            }else r=route_to_body(c,host,k);
            if(r==0)npc_resume_ai_mode(c,host);                              // 00153588 virtual +e0
            return r!=0;
        }
        if(m.event==0){                                                      // 00153360..001533bc
            c.timer_e8=3;c.mode_counter_ec=-1;
            const float walk=npc_travel_speed(1);                            // virtual +148(1)
            if(c.speed_priority_64<11){
                c.travel_speed_60=walk;c.speed_priority_64=10;
                c.flags_141=static_cast<std::uint8_t>(c.flags_141&~0x40U);
            }
            return true;
        }
        if(m.event==1){                                                      // 001533c0..001533e4
            if(c.speed_priority_64<11)c.speed_priority_64=10;
            return true;
        }
        return false;
    }
    if(m.kind==NpcAiMessage::Kind::path){                                    // 001533e8 004ed9b8
        if(m.event==6){
            if(c.mode_counter_ec==0){                                        // 00153418..001534a8
                c.mode_counter_ec=1;c.timer_e8=1;
                send_action(c,npc_carry_pickup_action,host);
            }else if(c.mode_counter_ec==2){                                  // 001534b0..00153548
                send_action(c,npc_carry_drop_action,host);
                npc_resume_ai_mode(c,host);                                  // 00153528 virtual +e0
            }
            return false;
        }
        if((m.event==1||m.event==3)&&(c.mode_counter_ec==0||c.mode_counter_ec==2))npc_resume_ai_mode(c,host);   // 00153550..00153598
        return false;
    }
    return false;
}
}
