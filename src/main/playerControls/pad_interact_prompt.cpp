#include "pad_interact_prompt.hpp"

#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace omega {

namespace {

// EE COP1 / VU0 as the interpreter models them: binary32 round-to-nearest,
// denormal results flushed to zero.
float ftz(float v) { return v != 0.0F && std::fabs(v) < FLT_MIN ? std::copysign(0.0F, v) : v; }
float cvt_s_w(std::int32_t v) {
    const float f = static_cast<float>(v);
    if (static_cast<double>(f) != static_cast<double>(v))
        throw std::runtime_error("Interact prompt: an inexact cvt.s.w of the text width (0x0045d288)");
    return f;
}

}  // namespace

std::array<float, 16> interact_prompt_resize(InteractResizeGlobals& g, const std::array<float, 16>& element_matrix,
                                             const std::function<std::optional<std::array<float, 4>>()>& background_rect,
                                             std::optional<std::int32_t> text_width) {
    if (!g.cached_61c8) {                                                    // 0x0045d180
        g.cache_61d0 = element_matrix;                                       // 0x0045d18c..0x0045d1b8
        g.cached_61c8 = 1;                                                   // 0x0045d1bc
    }
    if (!g.b_6210) g.b_6210 = 1;                                             // 0x0045d1c0..0x0045d1d4
    if (g.first_7b10) {                                                      // 0x0045d1d8
        if (const auto r = background_rect()) {                              // 0x0045d1f4 / 0x0045d20c
            g.rect_6220 = *r;                                                // 0x0045d230..0x0045d248
            g.rect_6220[2] = ftz(g.rect_6220[2] - 70.0F);                    // 0x0045d254 (0x428c0000)
        }
        g.first_7b10 = 0;                                                    // 0x0045d264
    }
    const float f4 = text_width ? cvt_s_w(*text_width) : 0.0F;               // 0x0045d270..0x0045d290
    const float w = g.rect_6220[2];                                          // 0x0045d29c
    if (f4 <= w || w <= 0.0F) return g.cache_61d0;                           // 0x0045d2a0..0x0045d2b8 -> 0x0045d3a0
    std::array<float, 16> m = g.cache_61d0;                                  // 0x0045d2cc..0x0045d2fc
    const float s = ftz(f4 / w);                                             // 0x0045d30c (w > 0)
    for (int k = 0; k < 4; ++k) m[static_cast<std::size_t>(k)] = ftz(m[static_cast<std::size_t>(k)] * s);  // vmulx.xyzw
    m[12] = ftz(m[12] + ftz(ftz(f4 - w) / 2.0F));                            // 0x0045d370..0x0045d37c
    return m;
}

bool game_screen_interact_shown(const GameScreenInteract& gs){
    return gs.widget_50&&(gs.widget_flags_14&2U)!=0;                        // 0045cf80
}

void game_screen_interact_hide(GameScreenInteract& gs,bool client){
    if(!gs.widget_50)return;                                                 // 0x0045cfc0
    if(client)gs.client_55=false;                                            // 0x0045cfd4
    else{
        gs.shown_54=false;                                                   // 0x0045cfd8
        if(gs.client_55){                                                    // 0x0045cfe0
            gs.log.push_back("41beb0 "+gs.client_text_58);                  // 0x0045cff8 (001692d0(+58))
            gs.log.push_back("41e0f0 10 "+std::to_string(gs.client_arg_64)); // 0x0045d010
        }
    }
    if(gs.client_55||gs.shown_54)gs.widget_flags_14|=2U;                     // 0x0045d018..0x0045d058
    else gs.widget_flags_14&=static_cast<std::uint16_t>(~2U);
}

void game_screen_interact_show(GameScreenInteract& gs,const std::optional<std::string>& text,std::uint8_t a2,bool client){
    if(!gs.widget_50)return;                                                 // 0x0045d090
    if(!text||text->empty()){game_screen_interact_hide(gs,client);return;}   // 0x0045d098..0x0045d0b4
    if(client){                                                              // 0x0045d0c8
        gs.client_55=true;                                                   // 0x0045d0d8
        gs.client_text_58=*text;                                             // 0x0045d0d4..0x0045d0ec (0013b480/0013d680/00138b70)
        gs.client_arg_64=a2;                                                 // 0x0045d0f8
    }else gs.shown_54=true;                                                  // 0x0045d104
    gs.widget_flags_14|=2U;                                                  // 0x0045d110..0x0045d118
    gs.log.push_back("41beb0 "+*text);                                      // 0x0045d120
    gs.log.push_back("41e0f0 10 "+std::to_string(a2));                       // 0x0045d138
    gs.log.push_back("41db80 textbackground");                               // 0x0045d148 (004c2238)
    gs.log.push_back("41e0f0 5");                                            // 0x0045d164; its IE answer and 003e8040 not hosted
}

void game_screen_display_interact(GameScreenInteract* gs,bool hud,const std::string& key,std::uint8_t button,
                                  const std::function<std::optional<std::string>(const std::string&)>& get_string){
    if(!hud||!gs)return;                                                     // 0x0045ecb8 / 0x0045ecc4
    game_screen_interact_show(*gs,get_string(key),button,true);              // 0x0045eccc / 0x0045ece0
}

void game_screen_display_interact_text(GameScreenInteract* gs,bool hud,const std::optional<std::string>& text,
                                       std::uint8_t button){
    if(!hud||!gs)return;                                                     // 0x0045ed14 / 0x0045ed20
    game_screen_interact_show(*gs,text,button,true);                         // 0x0045ed28
}

void game_screen_clear_interact(GameScreenInteract* gs,bool hud){
    if(!hud||!gs)return;                                                     // 0x0045ec70 / 0x0045ec7c
    game_screen_interact_hide(*gs,true);                                     // 0x0045ec84
}

void run_pad_interact_prompt(const PadInteractPromptInput& in,GameScreenInteract& gs,const PadInteractPromptHost& host){
    // 0x0024ec1c: the D-pad Left pressure edge (c.eq.s: -0 counts as 0).
    bool map_shown=in.hud_6a9;
    if(!(in.pressure_f84==0.0F)&&!in.previous_a5&&!in.object_128&&!in.hud_6a8&&!in.hud_6aa){
        if(!host.map_toggle)throw std::runtime_error("Interact prompt: the map toggle 002427a0 (0x0024ec64) is not hosted");
        map_shown=host.map_toggle();                                         // 0x0024ec64
    }
    if(map_shown){                                                           // 0x0024ec70: HUD +6a9
        if(!host.map_sticks)throw std::runtime_error("Interact prompt: the map update 0026be80 (HUD +6a0, 0x0024ec80) is not hosted");
        host.map_sticks();                                                   // 0x0024ec80
    }
    if(in.object_128&&in.player_actor){                                      // 0x0024ec88
        game_screen_interact_hide(gs,false);                                 // 0x0024eca0
        if(!host.menu_update||!host.menu_close)
            throw std::runtime_error("Interact prompt: 002526f0 on input +128 (0x0024ecac; 00247320 when it returns 0) is not hosted");
        if(!host.menu_update())host.menu_close();                            // 0x0024ecac / 0x0024ecbc
        return;
    }
    if(gs.choose_menu_visible_e0){game_screen_interact_hide(gs,false);return;}   // 0x0024ecd0 -> 0x0024ee90
    const auto box=host.select_box();                                        // 0x0024ece0
    if(!in.player_actor)
        throw std::runtime_error("Interact prompt: 0x0024ecec reads actor +3478 without a player actor");
    if(in.linked_model_3478){                                                // 0x0024ecec: 002501b0
        game_screen_interact_show(gs,host.get_string("HudDropBody"),1,false);   // 004a77c8
        return;
    }
    if(in.bits_3c_bit1||(!box&&!in.word_130)||in.bits_3c_bit8||in.mode_320c==0x13){   // 0x0024ed28..0x0024ed6c
        game_screen_interact_hide(gs,false);                                 // 0x0024ee78
        return;
    }
    if(!game_screen_interact_shown(gs)&&box&&box->type_90==0xc){             // 0x0024ed74..0x0024ed94
        const auto text=host.get_string("TeamMemReqd");                     // 004a77d8
        gs.log.push_back("242b00 "+(text?*text:std::string("NULL")));
        host.notice(text);                                                   // 0x0024eda8
    }
    // 0x0024edb0..0x0024ede0: mode 0x14 (0024fd80) and +324c bit 0 (0024fd50(+320c, 0x20)).
    const bool cancel=in.mode_320c==0x14&&(in.flags_324c&1U)!=0;
    if(cancel){
        game_screen_interact_show(gs,host.get_string("Cancel"),1,false);     // 004a77e8
        return;
    }
    if(game_screen_interact_shown(gs))return;                                // 0x0024ee20
    std::optional<std::string> text;
    if(box&&box->message_a0)text=box->message_a0;                            // 0x0024ee38
    else text=host.get_string("Interact");                                   // 004a77f0
    game_screen_interact_show(gs,text,1,false);                              // 0x0024ee64
}

} // namespace omega
