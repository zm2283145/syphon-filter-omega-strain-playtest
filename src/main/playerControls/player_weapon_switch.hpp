#pragma once
#include <cstdint>
#include <functional>

// The Select weapon switch of the player pad producer 0024b5d0
// (0x0024ca48..0x0024cd30), right after the flashlight block 0024c980 and
// before the target markers 0x0024ce5c. Select is sp+f80 (0055a44c & 0x100,
// zeroed by the input gates 0x0024be18 / 0x0024c010 and, with current
// priority bit 0xb, by 0x0024c0e8..0x0024c17c); input +a4 is its previous
// frame value, latched at the end of the producer (0x0024faa8..0x0024fad4).
//   press   (+a4 clear, Select): with 00250470 and 00143bc0(+3524) true,
//           +108 = 1 and +10c = clock + 200 (0x0024ca8c..0x0024cb60)
//   held    (+a4 set, Select): +108 set and clock >= +10c: 0045ca20(HUD
//           +6a4, 1), the choose menu, and +108 = 0 (0x0024cb8c..0x0024cc80)
//   no Select: +108 = 0 (0x0024cc88)
//   release (+a4 set, no Select), GuiGameScreen +d4 != 1 and 00250470:
//           00143a60(+3524, !(+128 && +128's +98 != 0)) (0x0024cc90..0x0024cd0c);
//           in multiplayer (byte 005721c8) then 002493c0(input, actor)
// The clock is the millisecond clock (*00533878 + T1 / 5.76) / 100.
namespace omega {

struct PlayerWeaponSwitchState {
    bool select_a4{};           // input +a4: the previous frame's sp+f80 != 0
    bool armed_108{};           // input +108
    std::int32_t menu_at_10c{}; // input +10c: the clock at which a held Select opens the choose menu
};

struct PlayerWeaponSwitchInput {
    float select_f80{};
    std::uint32_t clock_ms{};
    bool priority_bit11{};        // 00250350(+38, 0xb): the slots are zeroed (0x0024c0e8)
    std::int32_t selector_38{};   // input +38, read by 00250470
    std::int32_t mode_320c{};     // actor +320c, read by 00250470
    bool game_screen_d4_one{};    // *(*004ffc2c + 6a4) + d4 == 1
    bool menu_128{};              // input +128: the pickup menu is open
    bool menu_start_98{};         // its +98 is not zero
    bool multiplayer_5721c8{};
};

struct PlayerWeaponSwitchHost {
    std::function<bool()> switch_allowed;         // 00143bc0(+3524)
    std::function<void(bool holster)> cycle;      // 00143a60(+3524, holster)
    // Empty: 0045ca20 / 002493c0 throw naming their address.
    std::function<void()> choose_menu_45ca20;
    std::function<void()> multiplayer_2493c0;
};

// 00250470(input, actor): false with input +38 == 6 or actor +320c 0x17, 0x14
// or 0x15.
bool player_weapon_switch_predicate(std::int32_t selector_38,std::int32_t mode_320c);

// The block 0x0024ca48..0x0024cd30, then the end latch of +a4 (0x0024faa8).
// Returns whether 00143a60 ran.
bool run_player_weapon_switch(PlayerWeaponSwitchState& state,const PlayerWeaponSwitchInput& input,
                              const PlayerWeaponSwitchHost& host);

}
