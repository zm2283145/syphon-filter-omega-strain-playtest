#pragma once
// The player's flashlight (flashlight.cc; research note FLASHLIGHT.md,
// milestones F1 and F2): the 0x1c-byte object at actor +33ec (constructor
// 0045bad0, created for roles 1 and 3), its set state 0045b610 on the native
// point pool, the toggles 00199770 / 00199860, the D-pad Right tap/hold block
// of the pad producer 0024b5d0, the per-frame update 0045ae70 with the beam
// probe 0022a8c0, and the stealth value block of 00133ab0.
//
// Interpreter-verified (goldens in tests/flashlight_*_goldens.hpp): set state,
// toggles, pad block, update, probe (with the LOS receiver and the candidate
// scan hooked) and the stealth block. The constructor, destructor 0045b990,
// the attach point 7 selection and the scan wiring are static readings.
//
// Host-side effects are returned as events (click sound, `_LIGH` byte,
// target-manager registration of the lit spot, goggle mode switch, HUD
// calls, NPC Sight awareness requests). Unsupported cases throw
// std::runtime_error.
#include "bullet_pool.hpp"
#include "level_lights.hpp"
#include "model_attach_points.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// Engine variables registered by 004d7c10 (each keeps its storage address as
// the variable; defaults are the values in the executable's data, the two bss
// ones are 0). TORONTO1 scripts write none of them; a host may.
struct FlashlightTuning {
    float dist{100.0F};          // FlashDist       004979b8
    float short_dist{2.5F};      // FlashShortDist  004979c0
    float hit_rad{1.5F};         // FlashHitRad     004979c8
    float adjust_y{1.0F};        // FlashAdjusty    004979d0
    float min_outer{9.0F};       // FlashMinOuter   004979d8
    float max_outer{25.0F};      // FlashMaxOuter   004979e0
    float min_inner{0.0F};       // FlashMinInner   005860c0 (bss)
    float max_inner{5.0F};       // FlashMaxInner   004979e8
    float min_rg{0.1F};          // FlashMinRG      004979f0
    float min_b{0.05F};          // FlashMinB       004979f8
    float vel{1.0F};             // FlashVel        00497a10
    float acc{200.0F};           // FlashAcc        00497a18
    float dazzle_size{8.0F};     // FlashDazzleSize 00497a20 (glare light range, vcall +34 0045bc00)
    float squeeze{0.0F};         // FlashSqueeze    005860c8 (bss)
};

inline constexpr std::uint32_t flashlight_click_descriptor=0x004f0c50;   // 0020f860 sound descriptor (name not resolved)
inline constexpr std::uint32_t flashlight_target_manager=0x004f77c0;     // 0022c650 / 0022c720
inline constexpr std::uint8_t flashlight_attach_index=7;                  // 00385240(&M, actor +60, 7); 00489d70[7]

// The object at actor +33ec. The point-pool entries are indices into
// LevelLights::point. The glare light's +40 (the glare factor, read by its
// vcall +20 0045bbe0) lies past the 0x40 point-pool stride: the original
// writes it over the first word of the next pool entry (its kind tag; past
// the pool for entry 23). It is kept here instead (`glare_40`).
struct Flashlight {
    bool on{};                            // +0
    std::uint64_t owner{};                // +4 the actor (0: none)
    std::optional<std::size_t> beam;      // +8 point light, type 4, aux 3
    std::optional<std::size_t> glare;     // +c point light, type 0x40, aux 0x10
    float length_10{-1.0F};               // smoothed beam length; <= 0: unset
    float speed_14{};                     // beam length speed
    std::uint64_t spot{};                 // +18 the lit-spot world object (host handle)
    RootVector spot_position{0.0F,0.0F,0.0F,1.0F};   // spot +50 translation (identity transform at construction)
    float glare_40{};                     // glare light +40
};

// 001b5fe0 (001b60d4..001b6180): the object exists for roles 1 and 3.
bool flashlight_created_for_role(std::int32_t role);
// 0045bad0(fl, actor): on 0, owner, no lights, length -1, speed 0. The spot
// is a 0x60-byte base world object the host creates (id from 0040c240 with
// category 0xe7, 003cf0e0 kind 0, then vcall +90 with the identity
// 0055c9f0) and passes as `spot`.
Flashlight make_flashlight(std::uint64_t owner,std::uint64_t spot);

// What 0045b610 reads from the owner actor.
struct FlashlightOwner {
    RootVector position_e4{};             // actor vcall +e4 (click sound position)
    std::int32_t slot7_item{-1};          // inventory +78 (00142e00 writes the byte unless it is -1)
};

struct FlashlightEvent {
    enum class Kind {
        // 0045b610: 0020f860(request, 004f0c50, position) with byte +20 = 1,
        // sent to 004ffb50 with transport 0 (003c9420).
        click_sound,
        // 00142e00(inventory, 7, value): actor +24a2 + 00489d70[7] = value
        // (render +2442 + 7): the slot-object draw shows the `_LIGH` sub-mesh
        // while it is set.
        light_byte,
        spot_registered,        // 0022c650(004f77c0, spot)
        spot_unregistered,      // 0022c720(004f77c0, spot)
        spot_deleted,           // 0045b990: spot vcall +8(-1)
        set_state_call,         // 00199770 / 00199860 call 0045b610(fl, value)
        goggle_mode,            // 001992b0(actor, value, 0): goggle mode switch (not native)
        hud_flashlight,         // 00244a80(*004ffc2c, 0, value): icon animation 210 <-> 220
        hud_goggles,            // 00244b20(*004ffc2c, value)
    } kind{};
    std::int32_t value{};
    std::uint64_t object{};
    RootVector position{};
    bool operator==(const FlashlightEvent&) const=default;
};

// 0045b610(fl, on). `owner` must be given exactly when fl.owner != 0. When
// `on` equals the state nothing happens. Switching on takes the first free
// point-pool entry for the beam (00373e70; kind tag 2 at +0 and +1 = 0 are
// not modelled by LevelLight) and the next one for the glare (00179540), sets
// length -1, registers the spot when the beam was obtained and enables both
// (vcall +2c 00144b80). Switching off unregisters the spot and releases the
// beam (00144a60) only when there is a beam, releases the glare (0045b8e0),
// and clears both. A released entry gets id -1, disabled, vtable 004d9280;
// its other fields stay. The manager's light-table removal (00373d70) has no
// counterpart: rebuild_level_light_table skips disabled entries. The dirty
// byte 00535d62 is LevelLights::dirty_42 (Checkpoint 132f7): an enable or
// disable that changes the state, a release and a position move raise it.
void set_flashlight_state(Flashlight& fl,LevelLights& lights,bool on,const std::optional<FlashlightOwner>& owner,
    std::vector<FlashlightEvent>& events);
// 0045b990(fl, 1): unregister the spot, release both lights, delete the spot.
void destroy_flashlight(Flashlight& fl,LevelLights& lights,std::vector<FlashlightEvent>& events);
// 00199700 / 00199740: the spot object while on (else 0) / the on flag.
std::uint64_t flashlight_lit_spot(const Flashlight* fl);
bool flashlight_is_on(const Flashlight* fl);

// The actor bytes and inventory values the toggles read.
struct FlashlightGoggles {
    bool locked_33e5{};
    std::uint8_t mode_33e6{};
    bool on_33e7{};
};
struct FlashlightToggleActor {
    FlashlightOwner owner;                // for 0045b610
    std::int32_t slot6_item{-1};          // inventory +68: 0013fb50 returns the record unless -1
    std::int8_t slot6_record_100{};       // that record's +100 (lb)
    bool controller_player{};             // actor +58 non-null and vcall +54
    bool local_14{};                      // actor +14
};
// The inventory values above from the native inventory and database
// (00147840 never fails: an unknown id selects record 0).
FlashlightToggleActor flashlight_toggle_actor(const WeaponInventory& inventory,const WeaponDatabase& database,
    const RootVector& position_e4,bool controller_player,bool local_14);
// 00199770 (tap): with the goggles on, 001992b0(actor, mode == 0, 0) only.
// Otherwise ok = slot-7 item >= 0, 0045b610(fl, ok && !on) and, for a local
// player with ok, 00244a80(hud, 0, new).
void toggle_flashlight(Flashlight& fl,FlashlightGoggles& goggles,LevelLights& lights,const FlashlightToggleActor& actor,
    std::vector<FlashlightEvent>& events);
// 00199860 (hold): goggles on: unless locked, 001992b0(actor, 0, 0) when
// mode == 1, then off. Goggles off: with a slot-6 record, on, and the
// flashlight off through 0045b610 when it is on. On a change, for a local
// player, 00244b20(hud, on ? record +100 : 0).
void toggle_goggles(Flashlight& fl,FlashlightGoggles& goggles,LevelLights& lights,const FlashlightToggleActor& actor,
    std::vector<FlashlightEvent>& events);

// D-pad Right in the pad producer 0024b5d0 (0024c980..0024ca44, action
// value 2 at sp+f88) and the tail's copy of the action byte (0024fa98..
// 0024fad4: input +a4 + 2 = value != 0).
struct FlashlightPadState {
    bool previous_a6{};                   // input +a6
    float timer_104{};                    // input +104
};
struct FlashlightPadGates {
    bool hud_6a9{},hud_6a8{},hud_6aa{};   // *004ffc2c bytes (s7, s6, sp+1d0)
    bool priority_bit7{};                 // 00250350(input +38, 7): bit 7 of the current priority set +3c (PlayerButtonState::current_bits_3c)
};
enum class FlashlightPadRequest : std::uint8_t { none, flashlight, goggles };
// Any gate skips the block. Pressed (value != 0) after a release: timer 0.
// Pressed and held with timer < 0.5 (0048ae88): timer += dt, and goggles
// (00199860) when the new timer is above 0.5. Released after a press with
// timer < 0.5: flashlight (00199770). The caller runs the returned toggle.
FlashlightPadRequest update_flashlight_pad(FlashlightPadState& state,float action_value,float dt,const FlashlightPadGates& gates);

// Attach point 7 for the update (001b1d74: 00385240(&M, actor +60, 7)).
// Slot 7 must be set: the hard-mesh setup copies slot 0 into it
// (hard_mesh_attach_points); the default frames of 0038e100 (node 5) are not
// ported and throw. The world matrix is then
// weapon_muzzle_frame(shot_frame_node_world(node(point.node), component),
// point, attached item +130); row 1 is the beam axis.
const WeaponAttachPoint& flashlight_attach_point(const ModelAttachSlots& slots);

// The probe request 0045ae70 builds for 0022a8c0 (its stack +b0).
struct FlashlightProbeRequest {
    std::uint64_t owner{};                // passed as a0 (not stored at +0 until 0022a8c0)
    ShotFrame frame{};                    // +4: M with row 3 replaced by the lens point
    float range{};                        // +14: FlashDist, or FlashShortDist in the short state
    float hit_radius{};                   // +28: FlashHitRad
    // Constant fields: +8 0, +c 0, +10 -1, +1c 0, +20 1 (skip damage), +24 0,
    // +2c 1, +2d 1; +d is not written.
    // a2 of 0022a8c0, stored at the LOS request's +58: 0 from the flashlight
    // (0045ae70), 0x10 from the aim-view lock 00249b10.
    std::uint32_t los_mask{};
};
// 0022a8c0's LOS request (class 005435a0, vtable 004d9180) sent through the
// receiver 003caa50(004ffb50, 004ea0b0) and answered synchronously (+5c 0):
// origin +30 = frame row 3, displacement +40 = row 1 * range (xyzw), +50 the
// owner, +54 0, +58 the mask a2, +3c 1.0, +4c 0, +24 / +28 0. Before the call the
// provider's region cache (*004ea0b8 +50 / +54) is set to the owner's region
// object (actor +2fe0 -> +50 / +54 weak handle, 0014d410) when it resolves.
// The LOS world service is not native (LOS_QUERY_NATIVE.md): the host answers
// with the hit point or nullopt.
struct FlashlightLosRequest {
    RootVector origin{},displacement{};
    std::uint64_t owner{};
    std::uint32_t mask{};                 // +58
};
struct FlashlightProbeHost {
    std::function<std::optional<RootVector>(const FlashlightLosRequest&)> los;
    // 0022ba30 on the probe (skip damage). flashlight_candidate_scan wraps the
    // native scan_bullet_candidates; tests may hook it like the interpreter.
    std::function<void(BulletHitRequest&)> scan;
    // 001437e0(owner inventory): the held weapon's record, null with nothing
    // held (+84 == 6). Stored at +18; with skip damage only the hit entry's
    // damage type reads it, which cannot change its mode-0 test.
    const WeaponRecord* held_record{};
};
// 0022ba30 through the native candidate scan (bullet index 0 in the events).
std::function<void(BulletHitRequest&)> flashlight_candidate_scan(const BulletPoolHost& host,std::vector<BulletEvent>& events);
// The BulletHitRequest the probe passes to 0022ba30 (+14 = the LOS distance or the range).
BulletHitRequest flashlight_scan_request(const FlashlightProbeRequest& probe,float step,const WeaponRecord* held_record);
// 0022a8c0: +14 = |hit - origin| after a LOS hit; then the scan; a non-zero
// category +30 makes +14 the scan distance +34. Returns +14.
float flashlight_beam_probe(const FlashlightProbeRequest& probe,const FlashlightProbeHost& host);
// The same with the request's scan results the aim-view lock 00249b10 reads
// back (its +30 category and +38 victim, which 0022ba30 always writes).
struct FlashlightProbeResult {
    float step{};                         // +14 (the return value)
    std::int32_t category_30{};
    std::uint64_t victim_38{};
};
FlashlightProbeResult flashlight_beam_probe_result(const FlashlightProbeRequest& probe,const FlashlightProbeHost& host);

// One valid entry of the AI think list 004ea190, in list order.
struct FlashlightThinkEntry {
    std::uint64_t object{};
    RootVector position_e4{};             // object vcall +e4
    bool controller_58{};                 // object +58 non-null
    bool handle_valid{true};              // weak handle {object, serial} resolves (object +c == serial)
};
// 0016c9d0(controller +1ac, owner, 6 = Sight, 0.0): recorded for the host
// (NPC perception is not native, FLASHLIGHT.md F4).
struct FlashlightSightRequest {
    std::size_t entry{};                  // index into FlashlightFrameInput::think_list
    std::uint64_t object{};
    std::uint64_t source{};               // the owner
    std::int32_t cause{6};
    float value{0.0F};
    bool operator==(const FlashlightSightRequest&) const=default;
};
struct FlashlightFrameInput {
    ShotFrame attach{};                   // M: attach point 7 world matrix, rows (2, 0, 1, 3)
    float dt{};
    // Translation (+40) of the +50 transform of the first object of list
    // 004ffc14 (the game camera). Required when the glare light exists.
    std::optional<RootVector> camera_position;
    // actor +3204 bit 4 (crouched) with the physical component +3310 present
    // and its byte +3d2 set (no headroom, 001ec580): range FlashShortDist.
    bool short_range{};
    // 0022a8c0 (flashlight_beam_probe with a FlashlightProbeHost, or a hook).
    std::function<float(const FlashlightProbeRequest&)> probe;
    std::vector<FlashlightThinkEntry> think_list;
};
struct FlashlightFrameResult {
    std::optional<FlashlightProbeRequest> probe;
    float hit{};                          // the probe result
    float target{};                       // T = min(max(hit - FlashAdjusty, 0), FlashDist)
    RootVector end{};                     // E: beam light and spot position
    std::vector<FlashlightSightRequest> sight;
};
// 0045ae70(fl, M, dt) for a flashlight that is on (the actor logic 001b1710
// calls it only then). Nothing happens without a beam light. Writes the glare
// factor and position (the camera test), the beam light position, colour,
// weight, inner and outer distances into the point pool, the spot position,
// the length spring, and lists the Sight requests. Throws without an owner
// (the original reads actor +3524 / +3204 through it), without a probe or a
// camera position when needed, for a stale think-list handle (the original
// calls through a null object), for zero-length normalizations and for
// non-finite values.
FlashlightFrameResult update_flashlight(Flashlight& fl,LevelLights& lights,const FlashlightTuning& tuning,
    const FlashlightFrameInput& input);

// The stealth block of 00133ab0 (00133c38..00133da8) for one actor: with the
// lighting colour c at the actor (00537d80, mask 0x17 gather), L = |c / 128|,
// s = trunc(100 (1 - L)); thr = DarknessDecreaser (00489bb0, default 65) for a
// player (controller +54), NpcDarknessDecreaser (00489bb8, 90) otherwise;
// s = s < thr ? 0 : trunc((100 / (100 - thr)) (s - thr)); 0 while the actor's
// flashlight is on (00199740). The result is actor +44.
std::int32_t actor_stealth_value(const RootVector& colour,bool player,bool flashlight_on,
    std::int32_t darkness_decreaser=65,std::int32_t npc_darkness_decreaser=90);

}
