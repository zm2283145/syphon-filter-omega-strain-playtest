#include "player_weapon_switch.hpp"

#include <stdexcept>

namespace omega {

bool player_weapon_switch_predicate(std::int32_t selector_38,std::int32_t mode_320c){
    if(selector_38==6)return false;                                   // 0x00250478
    return mode_320c!=0x17&&mode_320c!=0x14&&mode_320c!=0x15;         // 0x00250488..0x0025049c
}

bool run_player_weapon_switch(PlayerWeaponSwitchState& state,const PlayerWeaponSwitchInput& input,
                              const PlayerWeaponSwitchHost& host){
    const bool select=!input.priority_bit11&&!(input.select_f80==0.0F);
    const auto clock=static_cast<std::int32_t>(input.clock_ms);
    const auto need=[](const auto& f,const char* what){
        if(!f)throw std::runtime_error(std::string("Weapon switch (0024b5d0): ")+what+" is not hosted");};
    if(!state.select_a4&&select){                                     // 0x0024ca48..0x0024ca64
        if(player_weapon_switch_predicate(input.selector_38,input.mode_320c)){   // 0x0024ca6c 00250470
            need(host.switch_allowed,"00143bc0");
            if(host.switch_allowed()){                                // 0x0024ca7c
                state.armed_108=true;                                 // 0x0024ca94
                state.menu_at_10c=clock+200;                          // 0x0024cb60
            }
        }
    }else if(state.select_a4&&select){                                // 0x0024cb68..0x0024cb84
        if(state.armed_108&&clock>=state.menu_at_10c){                // 0x0024cb8c / 0x0024cc60
            if(!host.choose_menu_45ca20)
                throw std::runtime_error("Weapon switch (0024b5d0): Select held 200 ms opens the choose menu 0045ca20(HUD +6a4, 1) (0x0024cc74), which is not hosted");
            host.choose_menu_45ca20();
            state.armed_108=false;                                    // 0x0024cc80
        }
    }else state.armed_108=false;                                      // 0x0024cc88
    bool cycled=false;
    if(state.select_a4&&!select&&!input.game_screen_d4_one            // 0x0024cc90..0x0024ccc4
       &&player_weapon_switch_predicate(input.selector_38,input.mode_320c)){   // 0x0024cccc
        const bool holster=!(input.menu_128&&input.menu_start_98);    // 0x0024ccdc..0x0024cd08
        need(host.cycle,"00143a60");
        host.cycle(holster);                                          // 0x0024cd0c
        cycled=true;
        if(input.multiplayer_5721c8){                                 // 0x0024cd14
            if(!host.multiplayer_2493c0)
                throw std::runtime_error("Weapon switch (0024b5d0): the multiplayer send 002493c0 (0x0024cd24) is not hosted");
            host.multiplayer_2493c0();
        }
    }
    state.select_a4=select;                                           // 0x0024fac8
    return cycled;
}

}
