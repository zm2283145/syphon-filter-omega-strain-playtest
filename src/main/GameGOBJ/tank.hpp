#pragma once
// The script tank of BELARUS2 and UKRAINE (Tank, 0x78 bytes, vtable 004dcf90,
// ID category 0xf9; research/TANKS.md, research/SCRIPT_NATIVES_BELARUS2.md):
// game::CreateTank 002840d0 -> the constructor 00284100, the natives
// 00282ab0..002835a0 (registrations 004d28b0), the update vt+48 00285810 with
// the turret aim 00286990, the angle limiter 00287540, the target search
// 00287710, the fire 00286690, the enemy setter 00285590, the turret
// constructor 00287950 and the tread steps 00287ab0 / 00287a20. Single
// player only (the byte 005721c8 clear): every multiplayer branch is left out.
//
// The world calls are host boundaries (TankHost): the NIE (NIEventOBJ) nodes
// the tank drives, its targets, the bullet pool 00277e20, the turret sound and
// the treads' UV-scroll records. A host function that is not set throws naming
// the address that needs it. Arithmetic follows the original order (binary32,
// results that would be denormal flushed to zero, as the EE FPU and the
// interpreter); tests/tank_tests.cpp holds the interpreter goldens.
#include "animation_root.hpp"
#include "bullet_hit.hpp"
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace omega {

using TankMatrix = std::array<RootVector, 4>;   // rows 0..3 (row 3 the translation)

// A weak object reference as the turret stores it: the object (+5c) and its
// +c at the time (+60). Null: object 0, id -1.
struct TankObjectRef {
    std::uint64_t object{};
    std::int32_t id{-1};
    bool operator==(const TankObjectRef&) const = default;
};

// One entry of the actor traversal array *004ffd44 (00134100 / 001340c0 /
// 001340e0) as 00287710 reads it: the root bytes +2c / +2d / +2e and the +58
// controller with its virtual +54 (1 for a player's).
struct TankCandidate {
    std::uint64_t object{};
    std::array<std::uint8_t, 3> root{};
    bool controller_58{};
    bool controller_player_54{};
};

// The turret (0x70 bytes, constructor 00287950). The NIEs are population IDs.
struct TankTurret {
    std::uint32_t yaw_0{}, pitch_4{}, flash_8{};
    std::uint32_t muzzle_c{};             // 00287950's t0, or the pitch NIE when t0 is null
    float fire_timer_10{0.0F};
    std::int32_t shots_14{0};
    float pitch_lo_18{}, pitch_hi_1c{};   // radians
    float yaw_lo_20{}, yaw_hi_24{};       // radians; equal: the yaw is not clamped
    float max_velocity_28{}, accel_2c{};
    float yaw_30{0.0F}, yaw_velocity_34{0.0F}, pitch_38{0.0F}, pitch_velocity_3c{0.0F};
    // +40 the lead point: 00287950 zeroes x, y, z (+40..+48); w (+4c) is the
    // heap's previous content (0036b180 does not clear). PORT-ONLY: w starts 0.
    RootVector lead_40{};
    float cone_50{};                      // radians
    bool stopped_54{false};
    bool rotate_sound_55{false};
    TankObjectRef enemy_5c{};             // +5c / +60
    // +64 the enemy of the previous aim. 00287950 does not write it (heap
    // content until the first aim or Reset writes it). PORT-ONLY: starts null.
    std::uint64_t previous_64{};
    bool weapon_set{false};               // +68 non-null (SetWeapon: 00147840(*004ffd30, id))
    std::int32_t weapon_6c{0};
};

// 00287950(turret, yaw, pitch, flash, t0, tank): the defaults (bit patterns
// from the constructor: limits -0.06 / 0.03 and -2.0 / -2.0, velocity 10.0,
// acceleration 0.5, cone 0.1).
TankTurret tank_turret(std::uint32_t yaw, std::uint32_t pitch, std::uint32_t flash, std::uint32_t muzzle);

// A tread (0x2c bytes, 00283130 / 002832a0): +0 the NIE whose model carries
// the UV-scroll animators, +4 the NIE whose X rotation (+144) drives them.
struct TankTread {
    std::uint32_t model_0{}, wheel_4{};
    struct Record {                       // +8 + 12 i: the animator, its +c / +10 directions
        float u_14{}, v_18{};             // the animator's +14 / +18 as last written
        float dir_u_c{}, dir_v_10{};
    };
    std::vector<Record> records;          // +20 the count (at most 2)
    float last_24{};                      // +24 the wheel's last +144
    bool initialised_28{false};           // +28
};

// The fields of the weapon record (00147840(*004ffd30, id)) the fire reads.
struct TankWeapon {
    std::int32_t far_3c{};              // the hitscan length (cvt.s.w)
    std::int32_t tracer_interval_a0{};
    float rate_a4{};                    // shots per second
};

struct TankHost {
    // The NIE nodes (NIEventOBJ, vtable 004dc050) by population ID.
    std::function<TankMatrix(std::uint32_t nie)> world;               // +50 component +10 (vt+58 refresh)
    std::function<std::uint32_t(std::uint32_t nie)> parent;           // +94
    // +f0 rows 0..2 (x, y, z, w) then vt+120 (0023a2d0 -> 0023dda0).
    std::function<void(std::uint32_t nie, const std::array<RootVector, 3>& rows)> set_local_rows;
    std::function<float(std::uint32_t nie)> rotation_144;             // +144 (the X rotation track value)
    std::function<void(std::uint32_t nie, float speed)> set_speed;    // 0023e3d0 (+134 over the +9c subtree)
    std::function<void(std::uint32_t nie)> play;                      // vt+114 0023e820
    std::function<void(std::uint32_t nie)> stop;                      // vt+118(1) 0023e790
    std::function<void(std::uint32_t nie)> reset;                     // vt+11c 0023e720
    // Objects.
    std::function<std::int32_t(std::uint64_t object)> object_id;      // +c
    std::function<bool(const TankObjectRef&)> valid;                  // +5c non-null and its +c == +60
    std::function<RootVector(std::uint64_t object)> eye_c0;           // vt+c0(out, object, -1)
    std::function<std::vector<TankCandidate>()> actors;               // *004ffd44
    // World services.
    std::function<void(std::uint32_t body)> register_target;          // 0022c650(004f77c0, body)
    std::function<void(const ShotRequest&)> bullet;                   // 00277e20(request)
    std::function<TankWeapon(std::int32_t id)> weapon;                // 00147840(*004ffd30, id)
    // 001ca210(desc, 0, "O_TANK_TURRET" 004a93d8, 0, 0), then 0020f990 (start)
    // / 0020eba0 (stop) with the yaw NIE as the owner, byte +20 = 1, sent to
    // 004ffb50 with transport 0 (003c9420(msg, 004ffb50, 0)).
    std::function<void(std::uint32_t yaw, bool start)> turret_sound;
    // 00287ab0: the model NIE's vt+124 (0023c180: +140 for kinds 1 / 3 / 6),
    // its +68 and the word there (false: 0, no records); then 00376ff0(+68,
    // buf, 2): the first two non-null animators of its mesh records, read as
    // their +14 / +18 floats.
    std::function<bool(std::uint32_t nie)> tread_model;
    std::function<std::vector<std::array<float, 2>>(std::uint32_t nie)> tread_records;
    // 00285810 / 00287a20: animator `index` +14 = u, +18 = v.
    std::function<void(std::uint32_t nie, std::size_t index, float u, float v)> tread_scroll;
    // NetCreate 00282c00: cNetCreateTankMsg (003c9e30 key 00506248, vtable
    // 004dcf70; +24 = 0x40-byte payload header, +28 = 4, +44 the tank, +48 its
    // +c) queued to the body with transport 0 (003c9420(msg, body, 0)).
    std::function<void(std::uint32_t body, std::int32_t tank_id)> net_create;
};

class Tank {
public:
    // 00284100 (after 0040c240(&id, 0xf9) and 003cf0e0): +60 the body NIE,
    // +64 / +68 / +6c null, +70 = 1, 0022c650(004f77c0, body), 0040c9d0, vt+5c(1)
    // (003ce980: +2d = 1, no controller), +74 = 0. `handle` is the host's
    // object for the tank (a bullet's shooter).
    Tank(std::uint32_t id, std::uint32_t body, std::uint64_t handle, TankHost& host);

    // Natives (object:Tank, registrations 004d28b0).
    void set_turret(std::uint32_t yaw, std::uint32_t pitch, std::uint32_t flash,
                    std::optional<std::uint32_t> muzzle);                  // 00283410 / 002835a0
    void set_left_tread(std::uint32_t model, std::uint32_t wheel, TankHost& host);    // 002832a0 (+64)
    void set_right_tread(std::uint32_t model, std::uint32_t wheel, TankHost& host);   // 00283130 (+68)
    void set_pitch_limit(float lo, float hi);                              // 00282f30
    void set_rot_limit(float lo, float hi);                                // 00282f90
    void set_turret_accel(float accel);                                    // 00282d90
    void set_firing_cone(float degrees);                                   // 00282d40
    void set_weapon(std::int32_t id);                                      // 00282dc0
    void move(float speed, TankHost& host);                                // 00282e10
    void aim_at(const TankObjectRef& enemy);                               // 002830f0 -> 00285590
    void clear_enemy() { set_enemy({}); }                                  // 00282ff0 -> 00285590(0)
    void resume_aiming();                                                  // 00283020
    void stop_firing();                                                    // 00282af0
    void reset(TankHost& host);                                            // 00282bb0
    void net_create(TankHost& host) const;                                 // 00282c00

    // 00285810 (vt+48, the traversal 00133420 runs it while +2d / +2c / !+2e).
    void update(float dt, TankHost& host);

    std::uint32_t id() const { return id_; }
    std::uint32_t body() const { return body_; }
    std::uint64_t handle() const { return handle_; }
    const std::optional<TankTurret>& turret() const { return turret_; }
    std::optional<TankTurret>& turret() { return turret_; }
    const std::optional<TankTread>& left_tread() const { return left_; }
    const std::optional<TankTread>& right_tread() const { return right_; }
    bool auto_target_70() const { return auto_70_; }
    float retarget_74() const { return retarget_74_; }
    void set_auto_target_70(bool v) { auto_70_ = v; }       // tests
    void set_retarget_74(float v) { retarget_74_ = v; }     // tests

    // 00285590(tank, enemy): turret +54 = 0, +5c = enemy, +60 = its +c (-1
    // for null); then +70 = 0 (with or without a turret).
    void set_enemy(const TankObjectRef& enemy);

private:
    std::uint32_t id_{}, body_{};
    std::uint64_t handle_{};
    std::optional<TankTread> left_, right_;   // +64 / +68
    std::optional<TankTurret> turret_;        // +6c
    bool auto_70_{true};
    float retarget_74_{0.0F};
};

// The pure steps (interpreter goldens).
// 00287540(turret, target f12, dt f13, lo f14, hi f15, &velocity a1, &angle a2).
void tank_turret_limit(const TankTurret& turret, float target, float dt, float lo, float hi, float& velocity,
                       float& angle);
// 00140e70 / 00140e80 (sinf / cosf) of a, b, c and 00140e20: rows 0..2 (w 0).
std::array<RootVector, 3> tank_euler_rows(float a, float b, float c);
// 00286990: the aim; true when the enemy is within the cone of the pitch NIE.
bool tank_turret_aim(TankTurret& turret, float dt, TankHost& host);
// 00287710: the actor nearest in yaw (|yaw| < 1000) within the pitch window
// (+18 < pitch + 0.3, pitch - 0.3 < +1c) among the present actors with a
// player controller (+58 vt+54); 0 when none.
std::uint64_t tank_target_search(const TankTurret& turret, TankHost& host);
// 00286690(turret): +10 = 1 / weapon +a4, +14 += 1, the hitscan request
// (range 500.0, origin the muzzle NIE's world) and the flash (3.0, Reset, Play).
void tank_turret_fire(TankTurret& turret, std::uint64_t tank, TankHost& host);
// Checkpoint 132uk6: the tank as the shooter of its bullets (00188780 / 00187360,
// BulletObjectShooter). +14 = 1: the base 003cb090 (sb 1 at 003cb0e0); the
// constructor clears it (00284234) only with 005721c8 set (multiplayer). +58 = 0:
// 003cf0e0 (003cf164); vt+5c 003ce980 sets no controller. vt+e4 (004dcf90 +e4) is
// 003cd260, which with +50 null (003cf0e0 003cf15c; the tank never sets it)
// answers vf0 (003cd3e8): (0, 0, 0, 1).
BulletObjectShooter tank_bullet_shooter();
// 00287ab0: the tread's records on first use; returns +28.
bool tank_tread_init(TankTread& tread, TankHost& host);
// 00287a20 / the inline copy in 00285810: the wheel's +144 delta times 1.0
// (0048b3d0) into every record's +14 / +18 by its directions.
void tank_tread_update(TankTread& tread, TankHost& host);

// The level's tanks in creation order (0040c9d0 registers each after the
// level's objects; the traversal 00133420 reaches them in that order).
class MissionTanks {
public:
    Tank& create(std::uint32_t id, std::uint32_t body, std::uint64_t handle, TankHost& host);
    Tank* find(std::uint32_t id);
    Tank& at(std::uint32_t id);    // throws when the ID is not a tank
    const Tank* find_handle(std::uint64_t handle) const;
    std::vector<Tank>& tanks() { return tanks_; }
    const std::vector<Tank>& tanks() const { return tanks_; }
    // 00133420 for each tank (vt+48 00285810).
    void update(float dt, TankHost& host);

private:
    std::vector<Tank> tanks_;
};
}
