#pragma once
// 001992b0(actor, value, a2): the IR goggles switch, reached from the AI
// script native EquipGoggles(NPC, bool) 00149070 (a1 = (word != 0), the
// component's virtual +1f0 = 0014a7f0: actor = component +30, a2 = 0) and
// from the player's flashlight input (00199770 / 00199860, flashlight.hpp).
//   +33e5 == 2 (the switch's trigger has run): return, nothing written.
//   Otherwise, when (0013fb50(+3524) != 0, i.e. inventory +68 (the slot-6
//   item) != -1, or a2), +3314 exists and +33e5 == 0: the one-shot request
//   001cae20(+3314, "switch_on_ir" (0049e070, copied with a 47-byte bound),
//   kind 1, event) with an event built by 003c9e30(ev, 00542b60), vtable
//   004da930, +24 = 3, +28 = 0, +2c = the actor, +30 = 0, +34 = 0; then
//   003c9d60(ev, 0). When accepted: +33e5 = 1, and with 005721c8 set and a2
//   clear the replication message 004eee80 (003c9420) - not hosted (throws).
//   Finally +33e6 = value.
// The one-shot's end posts code 3 to the actor (+33e5 = 0, the receiver);
// its "switch_on_ir" phase reaching the trigger (00197e00 at 001984bc) calls
// 00199590(actor, +33e6) and sets +33e5 = 2.
// Interpreter goldens: tools/ee/gen_equip_goggles.py.
#include "actor_animation_receiver.hpp"
#include "secondary_component.hpp"
#include <cstdint>
#include <vector>

namespace omega {
struct ActorGogglesResult {
    bool requested{};    // 001cae20 was called
    bool accepted{};     // its result
};
// `slot6_item`: inventory +68 != -1 (0013fb50 returns a record); `secondary`:
// actor +3314 (null: no component); `actor`: the event's receiver +2c;
// `networked`: *005721c8.
ActorGogglesResult switch_actor_goggles(ActorAnimationReceiverState& state,std::uint8_t value,bool a2,bool slot6_item,
    SecondaryComponent* secondary,std::uint64_t actor,bool networked);

inline constexpr std::uint32_t goggles_switch_sound=0x004f0df0;   // 0020f860 descriptor (00199680)
inline constexpr std::uint32_t goggles_vision_loop=0x004f1fd0;    // 0020f990 / 0020eba0 descriptor (001b32dc / 001b3358)
// Checkpoint 132cp5: the static descriptor 004f1fd0 (sinit 004c8838..004c88cc): +0 = 1 (bank), +10 = "E_THERMAL_IM".
inline constexpr std::int32_t goggles_vision_loop_bank=1;
inline constexpr const char* goggles_vision_loop_name="E_THERMAL_IM";

struct GogglesEvent {
    enum class Kind {
        hud_ir,          // 00244a80(*004ffc2c, a = record +100 or 0, b = the IR byte)
        render_byte,     // 00142e00(inventory, 6, b): actor +24a2 + 00489d70[6] = b (slot 6 item != -1)
        switch_sound,    // 0020f860(request, a = 004f0df0, actor vcall +e4), +20 = 1, to 004ffb50 transport 0
        hud_mode,        // 002609e0(*004ffc30, a = mode 1..3 or 0, 0)
        hud_vision,      // 00241e30(*004ffc2c, a = (mode == 1))
        vision_flag,     // 0053b540 = a (mode == 1)
        loop_start,      // 0020f990(request, a = 004f1fd0, actor), +20 = 1, to 004ffb50 transport 0
        loop_stop,       // 0020eba0(request, a = 004f1fd0, actor), +20 = 1, to 004ffb50 transport 0
    } kind{};
    std::int32_t a{},b{};
    bool operator==(const GogglesEvent&) const=default;
};
// What 00199590 / 001b31b0 read besides the actor's bytes.
struct GogglesActorView {
    std::int32_t slot6_item{-1};          // inventory +68 (0013fb50 and 00142e00 test it against -1)
    std::int8_t record_100{};             // lb of the slot-6 record's +100 (read only when the item is not -1)
    bool controller_player{};             // +58 non-null and its vcall +54
    bool byte_14{};                       // actor +14
    std::uint8_t crosshair_346e{};        // +346e (0 outside the aim view, 00241ec0)
};
// 00199590(actor, ir): r = 0013fb50(+3524) (the slot-6 record, null for item
// -1; the database's record table is assumed loaded); +346f = ir ? (r ?
// lb r +100 : 0) : 0; for a player controller with +14 and +33e7:
// 00244a80(hud, r ? r +100 : 0, ir) and without r +33e6 = +33e7 = 0; then
// 00142e00(inv, 6, ir) and the switch sound at the actor's vcall +e4.
void switch_actor_ir(ActorAnimationReceiverState& state,std::uint8_t ir,const GogglesActorView& actor,
    std::vector<GogglesEvent>& events);
// 001b31b0(actor), each frame from the actor logic 001b1710 (001b2598,
// after the +352c decay): v = +346f if non-zero, else +346e; when the byte
// differs from +3470: +3470 = v, and for a player controller with +14 the
// HUD calls (mode = v for 1..3, else 0) and 0053b540 = (v == 1). Then mode
// 2 starts the vision loop sound once (+33b9 = 1) and any other mode stops
// it (+33b9 = 0).
void update_actor_vision(ActorAnimationReceiverState& state,const GogglesActorView& actor,std::vector<GogglesEvent>& events);
}
