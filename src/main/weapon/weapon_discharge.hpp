#pragma once
// The weapon discharge 0013ff40(inventory, target, subtarget, aim_view,
// continuation) (COMBAT.md §4) with the shotgun pellets 00140ea0: readiness,
// fire sound, shot frame, hit roll and target designation, random spread
// (game RNG, game sinf/cosf, the 00140e20 matrix and the VU row products),
// the hitscan request handed to the bullet pool (00277e20), muzzle flash,
// the AI sound event, cooldown, burst, ammo, the automatic reload action
// 0x3c and the out-of-ammo message. Every message, sound and effect is an
// event in call order; nothing is played. Calls that return values or run
// synchronously (shot frame, hit roll, target stance, bullet allocation and
// pellet linking, effect lookups) go through the host. Launched projectiles
// (record +d4 = 1/2, 0025d730/0025d4b0; checkpoint 132uk6) are events and a
// host launch instead of the bullet. Hold class 5 (00266200) goes to the
// host's air taser (throws when it is not hosted).
#include "animation_root.hpp"
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

using ShotFrame=std::array<RootVector,4>;   // rows 0..2 (row 1 forward), row 3 origin

// The 0xe8-byte request the discharge builds on its stack (sp+290).
struct ShotRequest {
    ShotFrame frame{};               // +0..+3c
    float power_40{};                // float(record +104); the pellets vary it
    std::uint64_t shooter_44{};
    std::uint64_t designated_48{};   // set with a target, outside the aim view, when +104 >= 200
    std::int32_t subtarget_d0{-1};
    bool rolled_d4{},aim_view_d5{},tracer_d6{};
    float range_d8{};                // float(record +3c)
    std::int32_t record_dc{};        // the weapon item (record pointer in the original)
    std::int32_t word_e0{-1};
    float value_e4{1.0F};
    bool operator==(const ShotRequest&) const=default;
};

struct WeaponDischargeEvent {
    enum class Kind {
        fire_sound,      // 001ca210(desc, flag, name, 0, 0) -> 0020f990 -> MSGGAME_PLAYSND to 004ffb50, transport 0
        shot_frame,      // controller virtual +64(controller, frame, second hand, aim view)
        hit_roll,        // 003ea4a0(controller)
        target_stance,   // target virtual +c8
        muzzle_effect,   // 00236a20(*3ebfb0(0x18), {flag, request, effect id}) when the service exists
        shake,           // 001b3400(actor, record +13a / 50, record +a4)
        bullet,          // 00277e20(request)
        pellet_link,     // 00275750(bullet a, bullet b)
        flash,           // actor +2df0/+2df1 = 1, 00384a70(actor +60, hand)
        ai_noise,        // 0016b4f0(msg, actor id, 4, 1.0, record +b8) to "AI Sound System" 004ee5c0, transport 0
        fire_notice,     // AI controller virtual +cc(controller, target, continuation) (0014f460)
        reload_action,   // actor action 0x3c kind 0 (vtable 004e0840), transport 0
        out_of_ammo,     // cOutOfAmmoMsg 004ea150 (vtable 004d91e0, +24 item) to the controller, transport 0
        shot_statistics, // 0013fe80(inventory, aim view): statistics 9 and, in the aim view, 11
        dry_fire_sound,  // 00141300: empty-weapon click (descriptor by item kind) to 004ffb50, transport 0
        muzzle_smoke,    // 00144080: 00232120(5, frame) from the controller +64 frame of the hand (aim view 0)
        loop_sound_stop, // 00144ba0: 001ca210(desc, 1, record name) -> 0020eba0 (vtable 004dadf0) to 004ffb50, transport 0
        weapon_value_message, // 00144080: message 00583968 (vtable 004e1140, +24 value, +20 2) to the controller, transport 0
        projectile,      // 0036b180(0, 0x130, "weapon.cc", 0xb12) and 0025d730(obj, inventory +88, &request, record)
        projectile_launch, // 0025d4b0(obj, &point, 0.02): object the target (its +e4) or 0 (the computed point)
    } kind{};
    std::string name;                // fire sound
    std::int32_t value{};            // sound flag / hand / effect flag / bullet index / item
    std::int32_t value2{};           // effect id / link partner
    bool flag{};                     // second hand / aim view / continuation / roll result
    float a{},b{};                   // shake strength and rate / noise radius
    std::uint8_t stance{};
    std::optional<ShotRequest> request;
    std::optional<ShotFrame> frame;  // muzzle smoke
    std::optional<RootVector> point; // projectile launch
    std::uint64_t object{};          // notice target / bullet / first linked bullet / launch target
    std::uint64_t object2{};         // second linked bullet
    bool operator==(const WeaponDischargeEvent&) const=default;
};

struct WeaponDischargeHost {
    std::function<std::uint32_t()> random;                         // 00127dd0
    std::function<ShotFrame(bool second_hand,bool aim_view)> shot_frame;   // controller +64
    std::function<bool()> hit_roll;                                // 003ea4a0(controller) (weapon_hit_roll)
    std::function<std::uint8_t()> target_stance;                   // target virtual +c8
    // 00277e20: hand the request to the bullet pool; returns a non-zero bullet
    // handle, or 0 when all 50 records are in use.
    std::function<std::uint64_t(const ShotRequest&)> bullet;
    std::function<void(std::uint64_t,std::uint64_t)> link_bullets;   // 00275750
    // *00236f20(index): the effect id the muzzle effect carries.
    std::function<std::int32_t(std::int32_t index)> effect_id;
    // 003ebfb0(0x18) is non-null: the muzzle effect is requested (asked
    // only when a shot requests the effect).
    std::function<bool()> effect_service;
    // record +13c (the attached model, a runtime resource) byte +411 != -1.
    std::function<bool(std::int32_t model)> model_flag_411;
    // 001960c0's throw starts (actor_throw.hpp): 00197560(actor, &request +0,
    // &request +10, request +20) for hold class 1 and 00197140 for hold class 7.
    // Empty: a throw request throws.
    std::function<void(std::int32_t hold_class)> start_throw;
    // 001960c0's melee attack (actor_melee.hpp): 001414d0(inventory, &sp+9f)
    // then 00196480(actor, target, sp+9f). Empty: a melee request throws.
    std::function<void()> melee_attack;
    // 00384a70(actor +60, hand) after each hand's flash (0x1409c8): the shell eject, which draws
    // 00127dd0 when the hand's model has a shell record (+404). Empty: not hosted (no draw).
    std::function<void(bool second_hand)> eject_shell;
    // Checkpoint 132uk6, the launched projectile (record +d4 1 / 2): the
    // target's virtual +e4 (outside the aim view), and the projectile itself,
    // 0025d730(new, inventory +88, &request, record) then 0025d4b0(obj, &point,
    // arc). Empty: a launched projectile throws.
    std::function<RootVector()> projectile_target;
    std::function<void(const ShotRequest& request,const RootVector& point,float arc)> launch_projectile;
    // Checkpoint 132ia1c, hold class 5: 00266200(inventory +0, target, aim
    // view, subtarget) (air_taser_entry); returns its result. Empty: throws.
    std::function<bool(std::uint64_t target,bool aim_view,std::int32_t subtarget)> air_taser;
    std::uint64_t actor{};           // inventory +88
    std::int32_t actor_id{};         // actor +c
    bool player{};                   // controller +54
    bool ai{};                       // controller +58
    bool byte_14{true};              // actor +14
};
struct WeaponDischargeInput {
    std::uint64_t target{};          // 0 none
    std::int32_t target_id{-1};      // target +c
    std::int32_t subtarget{-1};
    bool aim_view{};
    bool continuation{};
};
// 0013ff40 (single player). Returns 1 when a shot was made (the return value
// of the original); the shot statistics follow it as events.
bool discharge_weapon(WeaponInventory& inventory,const WeaponDatabase& database,const WeaponDischargeInput& input,
    const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events);

// 0013ff40 0x00140740..0x001409c8 (checkpoint 132uk6), after the spread
// rotation: record +d4 1 or 2 is a launched projectile. 0025d730 gets the
// rotated request; the launch point is the target's +e4 (a target outside the
// aim view) or the shot frame of hand 0 (controller +64, a new unrotated
// frame) times (0, range, 0, 1) with the range inventory +f4 in the aim view,
// float(record +3c) outside it; then 0025d4b0(obj, &point, *00489c50 = 0.02).
// The shake, the bullets, the pellets and the second hand's bullet and muzzle
// effect are skipped (the flash follows). Returns false (no events) for any
// other +d4: the hitscan path 001408d0.
bool discharge_launched_projectile(const WeaponInventory& inventory,const WeaponRecord& record,const ShotRequest& request,
    const WeaponDischargeInput& input,const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events);

// 00140e20 with the angles (a, b, c) of the discharge / pellets: rows
// r0 = (cb cc + sb sc sa, ca sc, -sb cc + cb sc sa), r1 = (-cb sc + sb cc sa,
// ca cc, sb sc + cb cc sa), r2 = (sb ca, -sa, cb ca) in the original operation
// order (game sinf/cosf), applied to rows 0..2 of `frame` as the VU row
// products (xyz; w becomes 0). Row 3 is kept.
void rotate_shot_frame(ShotFrame& frame,const ShotFrame& source,float a,float b,float c);

}
