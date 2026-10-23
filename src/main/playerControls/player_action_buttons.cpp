#include "player_action_buttons.hpp"
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void require_host(const PlayerButtonHost& host){
    if(!host.actor||!host.send||!host.interaction)throw std::runtime_error("Player buttons: complete host required");
}
bool held_pressure(float pressure){
    if(std::isnan(pressure))throw std::runtime_error("Player buttons: nonfinite pressure");
    return !(pressure<=0.0F);                                   // c.ole.s f12, 0
}
// 0024a820
bool entered_zone(const PlayerButtonState& s){return !s.zone_previous_25&&s.zone_24;}
// 0024a6e0: stand (action 10) unless crouched (+3204 bit 4) without headroom.
bool stand_up(const PlayerButtonHost& host){
    const auto a=host.actor();
    if(a.crouched_3204_bit4&&a.physical_present&&a.physical_3d2)return false;
    host.send(10,false);return true;
}
// 0024a5a0 with 0024a6e0 (stand) and 0024a7a0 (crouch, action 9).
bool toggle_crouch(const PlayerButtonState& s,const PlayerButtonHost& host){
    if((s.current_bits_3c>>4)&1U)return stand_up(host);
    if(s.zone_24){host.send(9,false);return true;}
    return false;
}
}

void update_player_cross(PlayerButtonState& s,float pressure,const PlayerButtonHost& host){
    require_host(host);
    bool held=held_pressure(pressure);
    const bool edge=!s.cross_held_ac&&held;
    s.cross_held_ac=held;
    if(edge||(entered_zone(s)&&held)){
        if(toggle_crouch(s,host))held=false;
    }
    if(host.actor().mode_320c==0x10)host.send(held?0x1c:0x1d,false);
    if(held)host.send(0x10,false);
    host.send(held&&!s.zone_24?1:2,false);
}

void update_player_triangle(PlayerButtonState& s,float pressure,const PlayerButtonHost& host){
    require_host(host);
    bool held=held_pressure(pressure);
    bool edge=!s.triangle_held_ad&&held;
    s.triangle_held_ad=held;
    if(!held)s.triangle_blocked_bd=false;
    else if(s.triangle_blocked_bd){held=false;edge=false;}
    if(edge){
        host.send(s.alternate_138?0x2d:0x2c,false);
        if(host.actor().mode_320c==0x14){host.send(0x28,true);held=false;edge=false;}
    }
    if(edge){
        auto a=host.actor();
        const bool skip=(a.physical_present&&a.physical_dc)||a.life_state==2||a.life_state==3;
        if(!skip){
            a=host.actor();
            if((a.mode_320c==0||a.mode_320c==0x11||a.mode_320c==0x12)&&host.interaction()){
                s.interaction_12c=true;s.word_c8=0;s.word_cc=-1;held=false;edge=false;s.byte_bc=true;
                s.pending_bits_44&=~0x20U;                                // 0024a3f8: clear pending bit 5
                std::int32_t top=11;
                while(top>=0&&!((s.pending_bits_44>>top)&1U))--top;   // rescan 11..0
                s.pending_top_40=top;
            }
        }
    }
    if(edge||(entered_zone(s)&&held)){
        if(toggle_crouch(s,host))held=false;
    }
    if(held){host.send(0x0f,false);host.send(0x1b,false);}
}

bool player_circle_roll_request(bool actor,std::uint32_t current_bits_3c,float circle){
    if(!actor)return false;                                               // 0024cef0 (s2 = 0 -> 0x0024d1c0)
    for(const unsigned bit:{6U,8U,1U})                                    // 0024cef8 / 0024cf08 / 0024cf18 00250350(+38, k)
        if((current_bits_3c>>bit)&1U)return false;
    return !(circle==0.0F);                                               // 0024cf28..0024cf38 c.eq.s sp+f98, 0.0
}

float player_aim_entry_value(float l1,float r1,bool object_128,std::uint32_t current_bits_3c){
    bool on=!(l1==0.0F);                                                  // 0024f844 c.eq.s
    if(object_128)on=false;                                               // 0024f858
    else if(!(r1==0.0F)&&!((current_bits_3c>>8)&1U))on=false;             // 0024f870 / 00250350(+38, 8)
    return on?1.0F:0.0F;
}

void update_player_aim_entry(PlayerButtonState& s,const PlayerAimEntryInput& in,const PlayerAimEntryHost& host){
    auto need=[](const auto& f,const char* what)->const auto&{
        if(!f)throw std::runtime_error(std::string("Player aim entry: host ")+what+" required");
        return f;};
    if(in.selector_38==8&&!need(host.camera_moving,"camera_moving")())need(host.set_byte_2e00,"set_byte_2e00")(2);   // 0024a874 / 0017a610
    if(in.value==0.0F||((s.current_bits_3c>>11)&1U))s.pending_bits_44&=~0x100U;   // 0024a898 / 0024a8b0
    else s.pending_bits_44|=0x100U;
    // The scan reads +44 bit 31 at index -1 (word (-1+31)>>5 = 0, bit 31);
    // both of its outcomes store -1.
    std::int32_t top=11;
    while(top>=0&&!((s.pending_bits_44>>top)&1U))--top;
    s.pending_top_40=top;
    if(in.previous_30==8&&in.selector_38!=8){                             // 0024aa08
        need(host.set_byte_2e00,"set_byte_2e00")(0);
        if(in.multiplayer)throw std::runtime_error("Player aim entry: the multiplayer aim-view exit 00249460 is not hosted");
    }
}

void run_player_button_tail(PlayerButtonState& s,const PlayerButtonTailInput& in,const PlayerButtonTailHost& host){
    require_host(host.buttons);
    if(!host.latch_passive)throw std::runtime_error("Player buttons: passive latch host required");
    // 0024f818: mode 2 requires +3248 bit 3 (0024fd50) to reach 0024a850/Triangle.
    const bool mode_gate=host.buttons.actor().mode_320c!=2||((in.actor_flags_3248>>3)&1U);
    if(mode_gate){
        PlayerAimEntryInput a;
        a.selector_38=in.selector_38;a.previous_30=in.previous_selector_30;
        a.value=player_aim_entry_value(in.l1,in.r1,in.object_128,s.current_bits_3c);
        a.multiplayer=in.multiplayer;
        update_player_aim_entry(s,a,host.aim);                            // 0024f8cc
    }
    if(mode_gate&&!((s.current_bits_3c>>8)&1U)&&!((s.current_bits_3c>>1)&1U))
        update_player_triangle(s,in.triangle,host.buttons);              // 0024f900
    if(!s.cross_latched_2d)update_player_cross(s,in.cross,host.buttons);  // 0024f91c
    if(host.buttons.actor().physical_present){                           // 0024fa0c..fa34
        host.latch_passive(7);host.latch_passive(8);
    }
    // 0x0024faa8: the end latch +a4 + i = (sp+f80 + 4 i != 0) for every
    // button, also when the handlers above did not run.
    s.cross_held_ac=!(in.cross==0.0F);                                    // +ac (sp+fa0)
    s.triangle_held_ad=!(in.triangle==0.0F);                              // +ad (sp+fa4)
}

bool actor_forwards_hotbox_code(std::uint32_t code){
    return code==8||code==9||(code>=0x0c&&code<=0x10)||code==0x12;
}

void receive_player_hotbox_event(PlayerButtonState& s,const PlayerHotboxEvent& e,const PlayerButtonHost& host){
    if(!host.actor||!host.send)throw std::runtime_error("Player hotbox event: actor and send hosts required");
    if(e.payload!=0&&e.payload!=2)return;
    const bool enter=e.payload==0;
    switch(e.code){
    case 9:
        s.zone_24=enter;                                                   // 0024b5c0 / 0024b5b0
        if(enter){                                                         // 0024a660
            if(((s.current_bits_3c>>4)&1U)&&host.actor().human)host.send(9,false);
        }else if(host.actor().human)stand_up(host);                       // 0024a600
        return;
    case 0x0d:s.zone_count_134+=enter?1:-1;return;                        // 00177588
    case 0x0f:case 0x10:                                                   // 00177460
        if(enter){s.jump_zone_26=true;s.jump_side_27=static_cast<std::uint8_t>(e.code==0x10?2:1);s.jump_target_28=e.word_2c;}
        else s.jump_zone_26=false;
        return;
    case 8:case 0x0c:case 0x0e:return;                                     // 001775c0
    case 0x12:                                                             // 001774c8
        if(!host.object_exists||!host.trigger_object)throw std::runtime_error("Player hotbox code 0x12: the object lookup 0040c540 and its trigger 0021b790/003c9420 are not hosted");
        if(!host.object_exists(e.word_2c))return;                         // 001774dc
        if(!enter){                                                        // 002473e0: +128 and its +0 is the object -> 247320
            if(host.close_menu_for)host.close_menu_for(e.word_2c);
            return;
        }
        s.interaction_12c=false;s.pickup_16c=true;                         // 0017752c / 00177534
        host.trigger_object(e.word_2c,s);                                  // 00177548 / 0017755c
        return;
    default:throw std::runtime_error("Player hotbox code "+std::to_string(e.code)+" is not forwarded to the component");
    }
}
}
