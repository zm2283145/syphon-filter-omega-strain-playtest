#pragma once
// Cross and Triangle senders of the input helper 0024b5d0 (00249f40 and
// 0024a1c0 with the crouch helpers 0024a5a0/0024a6e0/0024a7a0/0024a820),
// its Circle send (0x0024cef0, action 8) and
// the button tail 0x0024f810..0x0024fa34 that calls them and latches the
// passive edge slots 7/8.
#include <cstdint>
#include <functional>

namespace omega {
// Input-state fields read or written by the two senders.
struct PlayerButtonState {
    bool cross_held_ac{};        // +0xac: Cross held last frame
    bool triangle_held_ad{};     // +0xad: Triangle held last frame
    bool triangle_blocked_bd{};  // +0xbd: Triangle swallowed until released
    bool byte_bc{};              // +0xbc: set by the Triangle object interaction
    bool zone_24{};              // +0x24: inside a code-9 (crawl) zone
    bool zone_previous_25{};     // +0x25: +0x24 copied at 0x0024fa50
    std::uint32_t current_bits_3c{}; // current priority set +0x38 bits (bit 4 crouched)
    std::int32_t pending_top_40{-1}; // pending priority set +0x40
    std::uint32_t pending_bits_44{};
    bool alternate_138{};        // +0x138: Triangle edge sends 0x2d instead of 0x2c
    bool interaction_12c{};      // +0x12c
    std::int32_t word_c8{},word_cc{}; // +0xc8/+0xcc
    bool cross_latched_2d{};     // +0x2d: Cross swallowed (set in the +0x128 path only)
    // Running-jump zone (hotbox codes 15/16, 0024b590/0024b580): +0x26 active,
    // +0x27 side (1 for code 15, 2 for 16), +0x28 the target's +0x0c key.
    bool jump_zone_26{};
    std::uint8_t jump_side_27{};
    std::uint32_t jump_target_28{};
    std::int32_t zone_count_134{};   // component +0x184 (input +0x134): hotbox code 13 nesting
    bool pickup_16c{};           // component +0x1bc (input +0x16c): set by an object-zone enter (001774c8), read by 001762c0
    bool operator==(const PlayerButtonState&) const=default;
};
// Actor fields read by the senders, at each original read point.
struct PlayerButtonActorView {
    std::int32_t mode_320c{};
    bool crouched_3204_bit4{};   // 0024a6e0
    bool physical_present{};     // +3310
    bool physical_3d2{};         // no headroom (0024a6e0)
    bool physical_dc{};          // 0024a330
    std::uint8_t life_state{};   // actor virtual +108 (00187... 0 alive, 2/3 dying/dead)
    // Actor +4c equals the class word at 0049d010 (the human class that
    // 001b6ab0 passes to 00393760); the zone helpers 0024a660/0024a600 act
    // only for such actors.
    bool human{};
};
struct PlayerButtonHost {
    std::function<PlayerButtonActorView()> actor;
    // 003c9420 immediate delivery of a pad action (constructor 003c9e30,
    // table 004e0840). alternate: the 0x28 message of type 004d9320 sent in
    // actor mode 0x14 (byte 0049aec8 at +0x2c).
    std::function<void(std::int32_t action,bool alternate)> send;
    // 001714c0(*(0050fbd0), actor, 1, 0): an object interaction takes the
    // Triangle press. Called only on the edge in modes 0/0x11/0x12.
    std::function<bool()> interaction;
    // 0040c540(key) is non-null (a registered object).
    std::function<bool(std::uint32_t key)> object_exists;
    // 0021b790(msg, 4, key, actor +c, 0) with byte +20 = 1 delivered at once
    // (003c9420(msg, object, 1)) to the object with this key; its receiver
    // reads and clears the component's +1bc / +17c (the state's pickup_16c /
    // interaction_12c).
    std::function<void(std::uint32_t key,PlayerButtonState&)> trigger_object;
    // Hotbox 0x12 exit: 002473e0(input, object) is true when input +128 (the
    // 00253070 pickup menu) is set and its +0 (the backpack) is this object;
    // then 00247320 closes the menu (252fb0(menu, -1), free, +128 = 0). The
    // host does both tests; unset when no menu can exist.
    std::function<void(std::uint32_t key)> close_menu_for;
};
// 00249f40. pressure is the Cross word /255 (0055a4a4). Held means
// !(pressure <= 0). The press edge, or a held press inside a newly entered
// code-9 zone, runs the crouch toggle 0024a5a0 (stand: action 10 unless
// crouched without headroom; crouch: action 9 in a zone), which swallows the
// press. Then: mode 0x10 sends 0x1c/0x1d, held sends 0x10, and action 1
// (held outside a zone) or 2 is always sent.
void update_player_cross(PlayerButtonState&,float pressure,const PlayerButtonHost&);
// 0024a1c0. pressure is the Triangle word /255 (0055a49c). The edge sends
// 0x2c (0x2d with +0x138); in actor mode 0x14 it sends 0x28 and swallows the
// press. Otherwise the edge may be taken by an object interaction (unless
// physical +dc, life state 2/3, or mode not 0/0x11/0x12), then the crouch
// toggle as for Cross. A remaining held press sends 0x0f and 0x1b.
void update_player_triangle(PlayerButtonState&,float pressure,const PlayerButtonHost&);

// The L1 handler 0024a850(input, actor, camera, value) (the manual aim view,
// selector +38 = 8, is pending bit 8).
struct PlayerAimEntryInput {
    std::int32_t selector_38{};       // input +38 (this frame's selector)
    std::int32_t previous_30{};       // input +30 (the previous frame's selector)
    float value{};                    // f12: 1.0 when L1 asks for the aim view, else 0.0
    bool multiplayer{};               // byte 005721c8
};
struct PlayerAimEntryHost {
    // 0017a610(camera): camera +9c4 (the boom curve +910's remaining time,
    // CameraRigState::boom_910.remaining) != 0.0. Read only with +38 = 8.
    std::function<bool()> camera_moving;
    // Actor byte +2e00: 2 while the aim view holds a resting camera, 0 when it
    // is left (its only writers; no reader is known).
    std::function<void(std::uint8_t)> set_byte_2e00;
};
// 0024a850. With +38 = 8 and a resting camera, actor +2e00 = 2. A zero value
// or current bit 11 clears pending bit 8, otherwise it is set; +40 becomes the
// highest set pending bit of 11..0 (or -1). Leaving the aim view (+30 = 8,
// +38 != 8) sets +2e00 = 0 and in multiplayer calls 00249460 (not hosted:
// throws).
void update_player_aim_entry(PlayerButtonState&,const PlayerAimEntryInput&,const PlayerAimEntryHost&);
// 0x0024cef0..0x0024cf8c (after the targeting stage 0x0024cec0, before the
// selector-8 sends 0x0024cf90): the Circle send. With an actor (s2) and none
// of the current priority bits 6 (the ladder), 8 (the aim view) and 1 set
// (00250350(input +38, k)), a Circle word sp+f98 (0055a4a0 / 255 after the
// game-state gates) other than 0.0 builds a message (003c9e30 with base
// 0055d480, table 004e0840, +24 = 8, +28 = 0) and delivers it to the actor at
// once (003c9420 transport 1), then destroys it (003c9d60(msg, 0)): actor
// action 8, the directional roll 001aa468. There is no edge test: the send
// repeats every frame Circle is held. Returns whether action 8 is sent.
bool player_circle_roll_request(bool actor,std::uint32_t current_bits_3c,float circle);
// 0x0024f838..0x0024f8c4: the caller's value. L1 (sp+f94, not 0.0) asks for
// the aim view unless input +128 is set or R1 (sp+f90, not 0.0) is held
// outside it (current bit 8 clear, 00250350); the result is cvt.s.w of 0/1.
float player_aim_entry_value(float l1,float r1,bool object_128,std::uint32_t current_bits_3c);

struct PlayerButtonTailInput {
    float triangle{},cross{};      // pressure words /255 after the game-state gates
    std::uint32_t actor_flags_3248{};
    // 0024a850's caller inputs (see player_aim_entry_value).
    float l1{},r1{};
    std::int32_t selector_38{},previous_selector_30{};
    // Input +128: the pickup menu 00253070 (created by the backpack receiver
    // 001762c0, or the drop path 0x0024c968); with it L1 does not ask for the
    // aim view (0x0024f858).
    bool object_128{};
    bool multiplayer{};
};
struct PlayerButtonTailHost {
    PlayerButtonHost buttons;
    // 0024fbc0(physical, slot, 1): passive latch of an edge slot.
    std::function<void(std::size_t slot)> latch_passive;
    PlayerAimEntryHost aim;
};
// 0x0024f818..0x0024fa34 for a supplied actor. Mode 2 without +3248 bit 3
// skips 0024a850 and Triangle; otherwise 0024a850 runs with the caller's
// value, then current priority bits 8 or 1 skip Triangle; Cross runs unless
// +0x2d (the menu took Cross; +ac is then the held state, as the end latch
// 0x0024faa8 stores it); with a physical component slots 7 and 8 are latched
// passive.
void run_player_button_tail(PlayerButtonState&,const PlayerButtonTailInput&,const PlayerButtonTailHost&);

// A hotbox event (class 004ee698) as the actor receives it: +0x28 code,
// +0x24 payload (0 enter, 2 exit), +0x2c word.
struct PlayerHotboxEvent {
    std::uint32_t code{};
    std::uint8_t payload{};
    std::uint32_t word_2c{};
};
// 001b3cb0 (table 0049e8c0): the actor forwards codes 8, 9, 0x0c..0x10 and
// 0x12 synchronously to its +58 component (virtual +0x0c); 0x0a, 0x0b and 0x11
// are dropped.
bool actor_forwards_hotbox_code(std::uint32_t code);
// 00177380 (table 0049cb60) on the player component for a forwarded code:
// 9 enter -> 0024b5c0 (+0x24 = 1, then 0024a660: with current priority bit 4
// and a human actor, crouch, action 9), 9 exit -> 0024b5b0 (+0x24 = 0, then
// 0024a600: for a human actor, stand, action 10 unless crouched (+3204 bit 4)
// with physical +3d2); 0x0d enter/exit -> +0x134 += 1 / -= 1; 0x0f/0x10 enter
// -> 0024b590 (+0x26 = 1, +0x27 = 1 for 0x0f else 2, +0x28 = +0x2c), exit
// -> 0024b580 (+0x26 = 0); 8, 0x0c and 0x0e have no effect; 0x12 (object
// zone, 001774c8): the object 0040c540(+0x2c); none: nothing. Exit: 002473e0
// compares input +128's backpack (+0) with it and 00247320 closes the menu
// (host.close_menu_for). Enter with the component's +14 set
// (1 from 003cb4e0; no other writer in single player): +17c = 0, +1bc = 1,
// then the trigger message to the object (host.trigger_object; absent:
// throws). Other payloads have no effect. Actions
// 9/10 are sent with event byte 1 and transport 1 (host.send, immediate).
void receive_player_hotbox_event(PlayerButtonState&,const PlayerHotboxEvent&,const PlayerButtonHost&);
}
