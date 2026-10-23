#pragma once
#include <array>
#include <optional>
#include <cstdint>

namespace omega {
// Stick getter 00369a10, mode 0 (per axis; no radial limit). The controller
// constructor 0036a160 stores dead zone (float)56 at +0x11c and
// 127.5 / (127.5 - dead_zone) at +0x124 (caller 00165de8). `centered` is the
// pad byte minus 127.5 (00369d40..). Result is clamped to [-127.5, 127.5].
struct PlayerPadAxisTuning { float dead_zone{56.0F}; };
float player_pad_axis(float centered,const PlayerPadAxisTuning& tuning={});
// 0024b5d0 stick vector: (axis(h) / 127.5, -axis(v) / 127.5). Up is +Y.
std::array<float,2> player_pad_stick(float centered_h,float centered_v,const PlayerPadAxisTuning& tuning={});

// Generic smoothing 00250ae0(rate_a, rate_b, eps_d, eps_p, dt, out, target, previous).
// d = target - previous; |d|^2 (xyz) < eps_d returns target. Otherwise u is the
// xyz-normalized previous (zero when |previous|^2 < eps_p); the part of d along
// u is kept only when d.u < 0. out = (previous + (d - v) * min(1, rate_a dt)) +
// v * min(1, rate_b dt). The VU reciprocal square root is approximated by
// portable binary32 arithmetic.
std::array<float,4> smooth_player_vector(float rate_a,float rate_b,float epsilon_difference,
    float epsilon_previous,float elapsed,const std::array<float,4>& target,
    const std::array<float,4>& previous);

// Retained input-state fields used by selector cases 0 and 4.
struct PlayerMoveState {
    bool backward_2c{};      // input +0x2c, backward-motion hysteresis
    float turn_80{};         // input +0x80, previous turn before the 2.2514746 gain
    float strafe_x_a0{};     // input +0xa0, previous direction X
};
struct PlayerMoveInput {
    std::array<float,2> left{};   // player_pad_stick result (sp+0xf70)
    // Pressure words /255 for action slots 10 and 11 (0055a4b8 and 0055a4b4).
    float pressure_10{},pressure_11{};
    float elapsed{};
    bool actor_flag3204_bit0{};   // 0019d760(actor+0x3204,0) and 001ae080(actor+0x3200,0)
};
// Message action 0 / kind 12 fields (0024fda0), delivered by 001a92a0/001ae430:
// direction -> actor +31c0, speed -> +31d0, turn -> +31d4, turn_in_place -> +31b7.
struct PlayerMoveCommand {
    std::array<float,4> direction{};
    float speed{},turn{};
    bool turn_in_place{};
};
// Selector cases 0/4 of 0024b5d0 (0x0024d2a0..0x0024dc24) for an NPC-qualified
// actor, plus the shared tail that builds the message and stores +0x80/+0xa0
// (0x0024eac4.., 0x0024fa64, 0x0024fae8). Camera outputs (002503e0, the 0.47
// scale), quick-turn 00249d00 and menu/game-state gating are not included;
// the caller supplies already gated stick and pressure values.
PlayerMoveCommand prepare_player_move(PlayerMoveState& state,const PlayerMoveInput& input);

// Selector case 7 of 0024b5d0 (0x0024e590..0x0024e7c0, the hang: input
// pending priority bit 7, set by player-component message code 6) and the
// shared tail (0x0024ea08..0x0024eb6c, 0x0024fa64, 0x0024fae8). The stick
// components are taken through max-magnitude comparisons with the function's
// zero vector 004e1d90 (and f23 = 0 for X), which only turns -0 into +0.
// Y > 0.9 (0x3f666666) queues action 0x11 (pull up), otherwise Y < -0.9
// queues action 0x13 (drop): constructor 003c9e30 (type 0055d480), table
// 004e0840, event byte +20 = 1, transport 0 (003c9420 clones and queues).
// The kind-12 message carries direction (X, 0, 0, 0), speed 0 for X = 0 else
// (|X| - 0.3333) / 0.6667, turn f22 * f24 = 0 and turn-in-place 0 (s5).
// +0x2c is cleared (selector not 0, 0x0024d274), +0x80 = f22 = 0 and +0xa0 =
// 0 (sp+0x1a0 stays 0). 00247410 is called with byte 0 and changes nothing
// for selectors 0, 4 and 7 (only 5 and 8 have effects). The camera output
// 002503e0 is not included.
struct PlayerHangMove {
    PlayerMoveCommand command;
    std::int32_t queued_action{-1};   // 0x11, 0x13 or -1
};
PlayerHangMove prepare_player_hang_move(PlayerMoveState& state,const std::array<float,2>& left);

// The default selector case 0024e560 of 0024b5d0 (selectors 1, 2, 3, 9 and
// 10; pending bit 9 is set in actor modes 4/5 and bit 10 in mode 1 at
// 0x0024d1c8..0x0024d234): only the camera output 002503e0 (not included),
// then the shared tail with the values 0024b5d0 initializes before the
// dispatch (0x0024c224..0x0024c2c0): direction 0, speed 0 (f30 = 0), turn
// 0 * gain, turn-in-place 0 (s5). +0x2c is cleared (selector not 0), +0x80 =
// f22 = 0 and +0xa0 = 0 (sp+0x1a0 = 0). The stick is not read.
PlayerMoveCommand prepare_player_idle_move(PlayerMoveState& state);

// 00250d80(out, target, previous, rate f12, eps f13, dt f14): d = target -
// previous (four lanes); (dx dx + dy dy) + dz dz < eps returns target,
// otherwise previous + d * min(1, rate dt).
std::array<float,4> smooth_player_aim_vector(const std::array<float,4>& target,
    const std::array<float,4>& previous,float rate,float epsilon,float elapsed);

// Selector case 8 of 0024b5d0 (0x0024e1d0..0x0024e528, the aim view: input
// pending bit 8, set by the L1 entry 0024a850), up to the shared tail. The
// camera's +50 transform rows 0 and 1 (+10/+20; its refresh byte +50 is taken
// as clear, so virtual +58 is not called) give the yaw f21: atan2f(r0.y, r0.x)
// (0019d180) when r0.z < r1.z, else atan2f(-r1.x, r1.y) (0019d170). The
// sticks are summed (00173d00, Z cleared) and, unless all four lanes are
// zero, normalised by the VU (length sp+105c); below length 1 the sum is
// scaled by len^4 + 0.3 (1 - (2 len - 1)^2) (mula/madd). The look rates
// sp+ee0 = (+124 ? s.y : -s.y, 0, -s.x, 0) * 1.4 * (0x3f61a840 * 2 * camera
// +60) are smoothed by 00250d80 against input +90 with rate 20 (all zero) or
// 2 and epsilon 0 * f2^2. sp+f30 is sp+ee0 unless input +3c bit 1
// (00250350(+38, 1)); f22, f30, sp+f20 and s5 are zero (the 001ae080 query
// cannot change s5 with f22 = 0) and byte sp+1b0 = 1. The shared tail then
// sends direction 0, speed 0, turn 0 * gain and turn-in-place 0, and clears
// +0x2c (selector not 0), +0x80 and +0xa0. Stores sp+ee0 to +90 (0x0024fa74).
struct PlayerAimViewMoveInput {
    std::array<float,4> row0{},row1{};   // camera +50 transform +10 rows 0 and 1
    float half_fov_60{};                 // camera +60
    std::array<float,2> left{},right{};  // sp+f70, sp+f60
    bool invert_124{};                   // input byte +124
    bool bits_3c_bit1{};                 // input +3c bit 1
    std::array<float,4> previous_90{};   // input +90
    float elapsed{};                     // f27
};
struct PlayerAimViewMove {
    PlayerMoveCommand command;
    float yaw{};                         // f21, for 00247410
    std::array<float,4> look_ee0{};      // smoothed sp+ee0, stored to input +90
    std::array<float,4> rates_f30{};     // sp+f30: camera +d60 = this * gain (00247800)
    std::array<float,4> stick_c40{};     // sp+c40
    float length_105c{};                 // sp+105c
};
PlayerAimViewMove prepare_player_aim_view_move(PlayerMoveState& state,const PlayerAimViewMoveInput& input);

// Selector case 5 of 0024b5d0 (0x0024dc28..0x0024e1c8, the R1 lock: input
// pending bit 5, set by the targeting 00248820 while the R1 pick holds), up
// to the shared tail. The yaw f21 is the aim view's (camera +50 rows 0/1).
// With a valid pick (+c8 resolved by 0014d350 and its +50 set), the actor's
// virtual +f4 rows 0/1 (rebuilt by 0024ff60 with row 3 (0, 0, 0, 1)) give the
// actor's yaw the same way; sp+ca0 = 0024fde0((0, 0, f21 - yaw)): each lane
// wrapped into [-pi, pi] by repeated -2pi / +2pi. The scaled-and-clamped copy
// sp+c90 and the 0 < ca0.x < 20 degree test (0x0024e02c) are dead. s5 (turn
// in place) = +3204 bit 0 (001ae080) && |stick| == 0 (VU length) && f23 == 0
// && !(|ca0.z| <= 0), with f23 = clamp(5 (p10 - p11)) of 0x0024c1b0. With s5
// or input +3c bit 1 (00250350) the direction and f30 are zero; otherwise
// f30 = min(1, max(max(|f23|, |stick|), 0)) (0024ffd0) and the direction
// ((f23 + 0) + x, 0 + y, 0, 0), normalised (VU) unless zero, times f30.
// f22 = 0, byte sp+1b0 = 1 and sp+1a0 = 1, so +0xa0 keeps the direction X;
// +0x2c is cleared (selector not 0) and +0x80 = 0.
struct PlayerLockMoveInput {
    std::array<float,4> row0{},row1{};   // camera +50 transform +10 rows 0 and 1
    // Actor virtual +f4 rows 0 and 1, present only with a valid pick.
    std::optional<std::array<std::array<float,4>,2>> actor_rows;
    std::array<float,2> left{};          // sp+f70
    float pressure_10{},pressure_11{};   // sp+fa8, sp+fac
    bool bits_3c_bit1{};                 // input +3c bit 1
    bool actor_flag3204_bit0{};          // 001ae080(actor+0x3200, 0)
};
struct PlayerLockMove {
    PlayerMoveCommand command;
    float yaw{};                         // f21, for 00247410
    float magnitude_f30{};               // f30 (the speed before 0x0024eac4)
    std::array<float,4> difference_ca0{};// sp+ca0 x/y/z (W not compared)
};
PlayerLockMove prepare_player_lock_move(PlayerMoveState& state,const PlayerLockMoveInput& input);

// The pad block 0x0024bda4..0x0024be14 (before the selector shift 0x0024c338),
// with v = |00250380(camera)| = |camera +934| (001d3e10): sp+200 = !(v < 4.0)
// (the c.olt at 0x0024bdc0) and sp+1f0 = !sp+200 && 0017a610(camera) == 0
// (0017a610 is 1 while +9c4 != 0).
struct PlayerAimViewDistance { bool near_1f0{},far_200{}; };
PlayerAimViewDistance player_aim_view_distance(float camera_934,float camera_9c4);
// camera +d60 written by 00247800 at 0x0024ec14: sp+f30 * f24 (001e5920).
std::array<float,4> player_camera_rates_d60(const std::array<float,4>& rates_f30);

// Selector case 6 of 0024b5d0 (0x0024e7c8..0x0024ea04, the ladder: input
// pending priority bit 6, set by player-component message code 10) and the
// shared tail of case 7. Y passes the max-magnitude test against the zero
// vector 004e1d90 (only -0 becomes +0). With left = |min(min(X, -p11), 0)|
// and right = |max(max(X, p10), 0)| (p10 = sp+0xfa8 = 0055a4b8, p11 = sp+0xfac
// = 0055a4b4), the exit request is 31 when Cross (sp+0xfa0, 0055a4a4) is
// nonzero; else, with both below 0.5, 31 for Y < 0, none for Y = 0, 32 for
// Y > 0; else 34 when left <= right, otherwise 33. A request is sent to the
// actor immediately (constructor 003c9e30, type 0055d480, table 004e0840,
// +0x24 = action, +0x28 = 0, event byte +20 = 1, transport 1). The tail sends
// actions 3 and 4 with payload bytes 0048ae90/0048ae98 (both 1) and the
// kind-12 message: direction (0, 0, Y, 0), speed 0 for Y = 0 else
// (|Y| - 0.3333) / 0.6667 (unclamped), turn f22 * f24 = 0, turn-in-place 0.
// +0x2c, +0x80 and +0xa0 become 0 as in case 7.
struct PlayerLadderMove {
    PlayerMoveCommand command;
    std::int32_t exit_action{-1};     // 31..34 or -1
};
PlayerLadderMove prepare_player_ladder_move(PlayerMoveState& state,const PlayerMoveInput& input,float cross);

// Quick-turn detector 00249d00, called by 0024b5d0 (0x0024d25c) before the
// selector dispatch when an actor is supplied. Pull the left stick back from
// neutral past -0.75 and let it return below 0.5 within 0.2 s (004a7580, set
// by a static initializer) to request a turn.
struct PlayerQuickTurnState {
    float timer_114{};   // input +0x114: time the stick has been held back
    bool armed_118{};    // input +0x118: the stick passed -0.75
};
struct PlayerQuickTurnInput {
    std::array<float,2> left{};            // this frame's stick (sp+0xf70)
    std::array<float,4> previous_left_60{};// input +0x60: the previous frame's stick (0x0024fa58)
    float elapsed{};
    std::uint32_t selector_38{};           // input +0x38: only 0 and 4 detect
    bool physical_unsupported_dc{};        // actor +3310 present with +dc set
    std::int32_t actor_mode_320c{};        // a request is not sent in mode 3
};
// Returns true when the action-14 message (constructor 003c9e30, table
// 004e0840, type 0055d480) is sent to the actor. The original then retimes the
// camera orientation curve +210 to 1.0 s; its other retimes (+2e0, +3a0, +780,
// +910 with -1) do nothing.
bool update_player_quick_turn(PlayerQuickTurnState& state,const PlayerQuickTurnInput& input);

// The input gates of 0024b5d0 (PlayerInput_Update) between the pad reads and
// the selector shift, 0x0024be18..0x0024c0e4. The slots are the function's
// stack values (pressure words 0055a48c.. /255, the masked bits of 0055a44c,
// the sticks after 00369a10). The zoom words sp+fc8 / sp+fcc, the D-pad copies
// sp+fb0..fbc and the L1 copy sp+fd0 are never gated.
struct PlayerPadSlots {
    float select_f80{};      // 0055a44c & 0x100
    float left_f84{},right_f88{},down_f8c{};   // 0055a490 / 0055a48c / 0055a498 (Up has only sp+fb0)
    float r1_f90{},l1_f94{},circle_f98{},square_f9c{};   // 0055a4b0 / 4ac / 4a0 / 4a8
    float cross_fa0{},triangle_fa4{},r2_fa8{},l2_fac{};  // 0055a4a4 / 49c / 4b8 / 4b4
    float start_fd4{},r3_fd8{};                          // 0055a44c & 0x800 / & 0x400
    std::array<float,2> left_f70{},right_f60{};          // the left (1050/1054) and right (1048/104c) sticks
};
struct PlayerPadGateBytes {
    std::uint8_t input_00{};          // input +0 (the constructor 002504e0 stores 0 at 0x00250500)
    bool loadout_pending_4ffc29{};    // G + d9
    bool hud_busy_241ac0{};           // 00241ac0(*004ffc2c)
    std::uint8_t state_4ffba0{0x0f};  // G + 50
    bool game_screen_d4_one{};        // *(*004ffc2c + 6a4) + d4 == 1
    bool hud_6a8{},hud_6a9{},hud_6aa{};   // *004ffc2c + 6a8 / 6a9 / 6aa
    std::uint32_t selector_38{};      // input +38 before the shift (0x0024c088)
    float time_limit_48{};            // G + 48 (0x004ffb98)
    bool triangle_latch_ad{};         // input +ad
};
struct PlayerPadGateResult {
    bool disabled{};       // 0x0024be28..0x0024be58: input +0, 004ffc29, 00241ac0 or state 0x13
    bool hud_screen{};     // 0x0024bff0..0x0024c008: HUD +6a9, +6a8 or +6aa
};
// Applies both gates to the slots. Throws (naming the address) where the
// original leaves the input update: the time limit G+48 (0x0024bf50, 00369f90
// and 0012fa60 kind 2; LEVEL_END.md) and Triangle closing the start menu from
// the input update (0x0024c048: 002261c0, 00131470(G, 0)).
PlayerPadGateResult apply_player_pad_gates(PlayerPadSlots& slots,const PlayerPadGateBytes& bytes);
}
