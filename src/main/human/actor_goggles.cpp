#include "actor_goggles.hpp"
#include "secondary_playback.hpp"
#include <stdexcept>

namespace omega {
ActorGogglesResult switch_actor_goggles(ActorAnimationReceiverState& state,std::uint8_t value,bool a2,bool slot6_item,
    SecondaryComponent* secondary,std::uint64_t actor,bool networked){
    ActorGogglesResult r;
    if(state.byte_33e5==2)return r;                                         // 001992d4..001992d8
    if((slot6_item||a2)&&secondary&&state.byte_33e5==0){                   // 001992e0..00199308
        SecondaryOneShot event;                                             // sp+0x50
        event.code=3;event.action=0;event.receiver=actor;event.posted=0;event.value=0.0F;   // 00199324..00199354
        r.requested=true;
        r.accepted=request_secondary_one_shot(*secondary,secondary_motion_name("switch_on_ir"),1,event);   // 001993b4
        if(r.accepted){
            state.byte_33e5=1;                                              // 001993e4
            if(networked&&!a2)                                              // 001993e8..001993f4
                throw std::runtime_error("001992b0: the goggles' replication message 004eee80 (003c9420, 005721c8 set) is not hosted");
        }
    }
    state.byte_33e6=value;                                                  // 00199460
    return r;
}

void switch_actor_ir(ActorAnimationReceiverState& state,std::uint8_t ir,const GogglesActorView& a,std::vector<GogglesEvent>& events){
    const bool record=a.slot6_item!=-1;                                     // 001995b8 0013fb50
    const std::int8_t s1=record?a.record_100:std::int8_t{0};                // 001995d4 lb +100
    state.byte_346f=ir?static_cast<std::uint8_t>(s1):std::uint8_t{0};       // 001995d8..001995e4
    if(a.controller_player&&a.byte_14&&state.byte_33e7){                    // 001995e8..00199628
        events.push_back({GogglesEvent::Kind::hud_ir,s1,ir});               // 00199634 00244a80
        if(!record){state.byte_33e6=0;state.byte_33e7=0;}                   // 0019963c..00199648
    }
    if(a.slot6_item!=-1)events.push_back({GogglesEvent::Kind::render_byte,6,ir});   // 00199658 00142e00
    events.push_back({GogglesEvent::Kind::switch_sound,static_cast<std::int32_t>(goggles_switch_sound),0});   // 00199660..0019969c
}

void update_actor_vision(ActorAnimationReceiverState& state,const GogglesActorView& a,std::vector<GogglesEvent>& events){
    const std::uint8_t v=state.byte_346f!=0?state.byte_346f:a.crosshair_346e;   // 001b31c8..001b31ec
    if(v!=state.vision_3470){                                               // 001b31f8
        state.vision_3470=v;                                                // 001b3200
        if(a.byte_14&&a.controller_player){                                 // 001b3204..001b3238
            const std::int32_t mode=v>=1&&v<=3?v:0;                         // 001b3240..001b3280
            events.push_back({GogglesEvent::Kind::hud_mode,mode,0});        // 001b3290 002609e0
            events.push_back({GogglesEvent::Kind::hud_vision,v==1?1:0,0});  // 001b32a4 00241e30
            events.push_back({GogglesEvent::Kind::vision_flag,v==1?1:0,0}); // 001b32b8 0053b540
        }
    }
    if(v==2){                                                               // 001b32c4
        if(state.loop_33b9)return;                                          // 001b32d0
        events.push_back({GogglesEvent::Kind::loop_start,static_cast<std::int32_t>(goggles_vision_loop),0});   // 001b32e0..001b32fc
        state.loop_33b9=1;                                                  // 001b3310
        return;
    }
    if(!state.loop_33b9)return;                                             // 001b334c
    events.push_back({GogglesEvent::Kind::loop_stop,static_cast<std::int32_t>(goggles_vision_loop),0});   // 001b335c..001b3378
    state.loop_33b9=0;                                                      // 001b3384
}
}