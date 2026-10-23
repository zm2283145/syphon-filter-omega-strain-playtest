#include "tank.hpp"
#include "game_libm.hpp"
#include "player_targeting.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Tank: " + what); }
float check(float v) {
    if (!std::isfinite(v)) fail("nonfinite arithmetic");
    return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v;
}
float mul(float a, float b) { return check(a * b); }
float add(float a, float b) { return check(a + b); }
float sub(float a, float b) { return check(a - b); }
float fdiv(float a, float b) {
    if (b == 0.0F) fail("division by zero");
    return check(a / b);
}
// The interpreter's max.s / min.s (fd = fs unless ft is strictly greater / less).
float fmax_s(float s, float t) { return t > s ? t : s; }
float fmin_s(float s, float t) { return t < s ? t : s; }
constexpr float f32(std::uint32_t w) { return std::bit_cast<float>(w); }
constexpr float degree = f32(0x3c8efa35U);          // pi / 180 (00282d5c..)
constexpr float pi = f32(0x40490fdbU), minus_pi = f32(0xc0490fdbU), two_pi = f32(0x40c90fdbU);

template <class F> const F& need(const F& f, const char* what) {
    if (!f) fail(std::string("not hosted: ") + what);
    return f;
}

// 0013ee20 (transpose the 3 x 3), 0013edb0 (row 3's xyz all zero, c.eq.s),
// 0013ed90 (t = 0 - row 3, vsub.xyz) and 0013ed60 (row 3 = M0 t.x + M1 t.y +
// M2 t.z on the transposed rows; 00133ec0 / 00132020 / 00131e70 store it, w
// kept): the rigid inverse, as in target_angles (0022abc0).
TankMatrix rigid_inverse(const TankMatrix& frame) {
    TankMatrix m = frame;
    std::swap(m[0][1], m[1][0]);
    std::swap(m[0][2], m[2][0]);
    std::swap(m[1][2], m[2][1]);
    if (!(m[3][0] == 0.0F && m[3][1] == 0.0F && m[3][2] == 0.0F)) {
        const float tx = sub(0.0F, m[3][0]), ty = sub(0.0F, m[3][1]), tz = sub(0.0F, m[3][2]);
        RootVector t = m[3];
        for (std::size_t c = 0; c < 3; ++c) t[c] = add(add(mul(m[0][c], tx), mul(m[1][c], ty)), mul(m[2][c], tz));
        m[3] = t;
    }
    return m;
}
// vmulax / vmadday / vmaddaz / vmaddw.xyzw.
RootVector frame_point(const TankMatrix& m, const RootVector& v) {
    RootVector p{};
    for (std::size_t k = 0; k < 4; ++k)
        p[k] = add(add(add(mul(m[0][k], v[0]), mul(m[1][k], v[1])), mul(m[2][k], v[2])), mul(m[3][k], v[3]));
    return p;
}
// (x, y, 0, 0): vmul.xyz, then w = 1 * x^2 + 1 * y^2 + 1 * 0 (vmulax.w /
// vmadday.w / vmaddz.w with vf0.w), vsqrt.
float planar(const RootVector& p) {
    const float xx = mul(p[0], p[0]), yy = mul(p[1], p[1]), zz = mul(0.0F, 0.0F);
    return check(vu::sqrt(add(add(mul(1.0F, xx), mul(1.0F, yy)), mul(1.0F, zz))));
}
bool enemy_valid(const TankTurret& t, TankHost& h) {
    if (t.enemy_5c.object == 0) return false;
    return need(h.valid, "the enemy's +c (00285864 / 00286a00)")(t.enemy_5c);
}
}  // namespace

TankTurret tank_turret(std::uint32_t yaw, std::uint32_t pitch, std::uint32_t flash, std::uint32_t muzzle) {
    TankTurret t;
    t.yaw_0 = yaw;
    t.pitch_4 = pitch;
    t.flash_8 = flash;
    t.muzzle_c = muzzle != 0 ? muzzle : pitch;   // 00287968: t0, else a2
    t.pitch_lo_18 = f32(0xbd75c28fU);
    t.pitch_hi_1c = f32(0x3cf5c28fU);
    t.yaw_lo_20 = f32(0xc0000000U);
    t.yaw_hi_24 = f32(0xc0000000U);
    t.max_velocity_28 = f32(0x41200000U);
    t.accel_2c = f32(0x3f000000U);
    t.cone_50 = f32(0x3dcccccdU);
    return t;
}

void tank_turret_limit(const TankTurret& t, float target, float dt, float lo, float hi, float& velocity, float& angle) {
    float d = sub(target, angle);                                   // 00287554
    if (d < minus_pi) d = add(d, two_pi);
    else if (!(d <= pi)) d = sub(d, two_pi);
    const float s = sub(mul(d, f32(0x40a00000U)), mul(velocity, f32(0x40a00000U)));   // 0048b3a8 / 0048b398 (5.0)
    const float s2 = mul(s, s);
    const float q = s <= 0.0F ? mul(f32(0xbf800000U), s2) : mul(f32(0x3f800000U), s2);
    const float kq = mul(f32(0x3f800000U), q);                      // 0048b3a0 (1.0)
    const float a = sub(kq, velocity);
    float limit = t.accel_2c;
    if (std::fabs(kq) < std::fabs(velocity)) limit = mul(limit, f32(0x40000000U));   // 0048b380 (2.0)
    const float clamped = fmin_s(fmax_s(a, -limit), limit);         // max.s / min.s
    velocity = add(velocity, mul(clamped, dt));
    const float v = fmin_s(fmax_s(velocity, -t.max_velocity_28), t.max_velocity_28);
    const float step = mul(v, dt);
    velocity = v;
    angle = add(angle, step);
    if (!(lo == hi)) {
        if (angle < lo) angle = lo;
        else if (!(angle <= hi)) angle = hi;
    }
    if (angle < minus_pi) angle = add(angle, two_pi);
    else if (!(angle <= pi)) angle = sub(angle, two_pi);
}

std::array<RootVector, 3> tank_euler_rows(float a, float b, float c) {
    const float ca = game_cosf(a), sa = game_sinf(a);               // 00140e80 / 00140e70
    const float cb = game_cosf(b), sb = game_sinf(b);
    const float cc = game_cosf(c), sc = game_sinf(c);
    const float s0 = mul(cb, ca);
    const float f2 = mul(sc, ca);
    const float f1 = mul(sc, sa);
    const float f13 = mul(-sc, cb);
    const float f18 = mul(-cb, sa);
    const float f4 = mul(cc, ca);
    const float f12 = sub(f4, mul(f1, sb));
    const float f17 = sub(f1, mul(f4, sb));
    const float f3 = mul(cc, sa);
    const float f14 = add(f3, mul(f2, sb));
    const float f0 = mul(f3, sb);
    const float f16 = mul(cc, cb);
    const float f15 = add(f2, f0);
    // 00140e20: (f12 f15 f18 0) (f13 f16 f19 0) (f14 f17 s0 0), f19 = sin b.
    return {RootVector{f12, f15, f18, 0.0F}, RootVector{f13, f16, sb, 0.0F}, RootVector{f14, f17, s0, 0.0F}};
}

bool tank_turret_aim(TankTurret& t, float dt, TankHost& h) {
    float pitch_target = 0.0F, yaw_target = 0.0F;
    const bool valid = enemy_valid(t, h);
    if (valid) {
        // 00286a38: the yaw NIE's parent (+94) world, row 3 the yaw NIE's own (+40).
        TankMatrix m = need(h.world, "the NIE world (+50 component, 00286a40)")(
            need(h.parent, "the NIE parent +94 (00286a3c)")(t.yaw_0));
        m[3] = h.world(t.yaw_0)[3];
        const RootVector eye = need(h.eye_c0, "the enemy's virtual +c0 (00286b68)")(t.enemy_5c.object);
        RootVector p = frame_point(rigid_inverse(m), eye);
        float horizontal = planar(p);
        if (t.enemy_5c.object == t.previous_64 && horizontal > f32(0x41200000U)) {   // 0048b390 (10.0)
            RootVector d{};
            for (std::size_t k = 0; k < 4; ++k) d[k] = sub(p[k], t.lead_40[k]);   // vsub.xyzw
            t.lead_40 = p;
            for (std::size_t k = 0; k < 4; ++k) p[k] = add(p[k], mul(d[k], f32(0x420c0000U)));   // 0048b388 (35.0)
            horizontal = planar(p);
        } else {
            t.lead_40 = p;
        }
        pitch_target = game_atan2f(p[2], horizontal);              // 00121ab8
        yaw_target = game_atan2f(-p[0], p[1]);
    }
    t.previous_64 = valid ? t.enemy_5c.object : 0;
    tank_turret_limit(t, pitch_target, dt, t.pitch_lo_18, t.pitch_hi_1c, t.pitch_velocity_3c, t.pitch_38);
    need(h.set_local_rows, "the NIE +f0 rows and vt+120 (00286eb0)")(t.pitch_4, tank_euler_rows(0.0F, t.pitch_38, 0.0F));
    tank_turret_limit(t, yaw_target, dt, t.yaw_lo_20, t.yaw_hi_24, t.yaw_velocity_34, t.yaw_30);
    h.set_local_rows(t.yaw_0, tank_euler_rows(0.0F, 0.0F, t.yaw_30));
    if (t.yaw_lo_20 == t.yaw_hi_24) {                               // 00287074
        if (!(std::fabs(t.yaw_velocity_34) <= f32(0x3dcccccdU))) {  // 0048b3e0 (0.1)
            if (!t.rotate_sound_55) {
                t.rotate_sound_55 = true;
                need(h.turret_sound, "the turret sound 001ca210 / 0020f990 (002870c0)")(t.yaw_0, true);
            }
        } else if (t.rotate_sound_55) {
            t.rotate_sound_55 = false;
            need(h.turret_sound, "the turret sound 001ca210 / 0020eba0 (00287168)")(t.yaw_0, false);
        }
    }
    if (!enemy_valid(t, h)) return false;                           // 002871e8
    const RootVector eye = h.eye_c0(t.enemy_5c.object);             // 00287338
    const RootVector p = frame_point(rigid_inverse(h.world(t.pitch_4)), eye);
    const float yaw = game_atan2f(-p[0], p[1]);
    if (!(std::fabs(yaw) < t.cone_50)) return false;
    const float pitch = game_atan2f(p[2], p[1]);
    return std::fabs(pitch) < t.cone_50;
}

std::uint64_t tank_target_search(const TankTurret& t, TankHost& h) {
    const TankMatrix frame = need(h.world, "the NIE world (+50 component, 00287730)")(t.yaw_0);
    float best = f32(0x447a0000U);                                  // 1000.0
    std::uint64_t chosen = 0;
    for (const auto& c : need(h.actors, "the actor array *004ffd44 (00287780)")()) {
        if (!(c.root[1] != 0 && c.root[0] != 0 && c.root[2] == 0)) continue;   // +2d, +2c, !+2e
        if (!(c.controller_58 && c.controller_player_54)) continue;              // +58 vt+54
        const RootVector eye = need(h.eye_c0, "the actor's virtual +c0 (00287844)")(c.object);
        const auto a = target_angles(frame, eye);                   // 0022abc0
        const float window = f32(0x3e99999aU);                      // 0.3
        if (add(window, a.angles[0]) <= t.pitch_lo_18) continue;
        if (!(sub(a.angles[0], window) < t.pitch_hi_1c)) continue;
        const float yaw = std::fabs(a.angles[2]);
        if (yaw < best) {
            best = yaw;
            chosen = c.object;
        }
    }
    return chosen;
}

void tank_turret_fire(TankTurret& t, std::uint64_t tank, TankHost& h) {
    if (!t.weapon_set) fail("00286690 fires without a weapon record (+68 null: SetWeapon was not called)");
    const TankWeapon record = need(h.weapon, "the weapon record 00147840 (+68)")(t.weapon_6c);
    t.fire_timer_10 = fdiv(f32(0x3f800000U), record.rate_a4);     // 1 / +a4
    t.shots_14 += 1;
    ShotRequest r;                                                  // sp+60
    const std::int32_t interval = record.tracer_interval_a0;
    r.tracer_d6 = interval != 0 && t.shots_14 % interval == 0;      // sp+136
    r.range_d8 = game_int_to_float(record.far_3c);               // sp+138: cvt.s.w(+3c)
    r.power_40 = f32(0x43fa0000U);                                  // sp+a0: 500.0
    r.rolled_d4 = false;                                            // sp+134
    r.shooter_44 = tank;                                            // sp+a4: turret +58
    r.designated_48 = 0;                                            // sp+a8
    r.record_dc = t.weapon_6c;                                      // sp+13c: turret +68
    r.word_e0 = -1;                                                 // sp+140
    // sp+130 (+d0) and sp+135 (+d5) are not written (stack content): read
    // only for a designated target and a player shooter. PORT-ONLY: -1 / 0.
    r.subtarget_d0 = -1;
    r.aim_view_d5 = false;
    r.frame = need(h.world, "the muzzle NIE world (+50 component, 0028678c)")(t.muzzle_c);
    need(h.bullet, "the bullet pool 00277e20 (002867dc)")(r);
    need(h.set_speed, "0023e3d0 (002867ec)")(t.flash_8, f32(0x40400000U));   // 3.0
    need(h.reset, "the NIE vt+11c (00286800)")(t.flash_8);
    need(h.play, "the NIE vt+114 (00286814)")(t.flash_8);
}

BulletObjectShooter tank_bullet_shooter() {
    BulletObjectShooter s;
    s.owned_14 = true;                                              // 003cb0e0 (single player: 00284234 not reached)
    s.position_e4 = {0.0F, 0.0F, 0.0F, 1.0F};                       // 003cd260 with +50 null: vf0 (003cd3ec)
    return s;
}

bool tank_tread_init(TankTread& tread, TankHost& h) {
    if (!need(h.tread_model, "the tread model's vt+124 / +68 (00287ac8)")(tread.model_0)) {
        tread.initialised_28 = false;                               // 00287bf0
        return false;
    }
    if (!tread.initialised_28) {
        auto records = need(h.tread_records, "the UV-scroll records 00376ff0 (00287af4)")(tread.model_0);
        if (records.size() > 2) fail("00376ff0 answered more than the two records asked for");
        tread.records.clear();
        tread.initialised_28 = true;
        for (const auto& r : records) {
            TankTread::Record x;
            x.u_14 = r[0];
            x.v_18 = r[1];
            const float nu = -r[0];
            x.dir_u_c = nu < f32(0xbdcccccdU) ? -1.0F : nu <= f32(0x3dcccccdU) ? 0.0F : 1.0F;
            const float v = r[1];
            x.dir_v_10 = v < f32(0xbdcccccdU) ? -1.0F : v <= f32(0x3dcccccdU) ? 0.0F : 1.0F;
            tread.records.push_back(x);
        }
    }
    return tread.initialised_28;
}

void tank_tread_update(TankTread& tread, TankHost& h) {
    if (!tank_tread_init(tread, h)) return;
    const float r = need(h.rotation_144, "the wheel NIE's +144 (002859ac)")(tread.wheel_4);
    float d = sub(r, tread.last_24);
    tread.last_24 = r;
    d = mul(d, f32(0x3f800000U));                                   // 0048b3d0 (1.0)
    for (std::size_t i = 0; i < tread.records.size(); ++i) {
        auto& x = tread.records[i];
        x.u_14 = mul(d, x.dir_u_c);
        x.v_18 = mul(d, x.dir_v_10);
        need(h.tread_scroll, "the UV-scroll animator +14 / +18 (002859e8)")(tread.model_0, i, x.u_14, x.v_18);
    }
}

Tank::Tank(std::uint32_t id, std::uint32_t body, std::uint64_t handle, TankHost& host)
    : id_(id), body_(body), handle_(handle) {
    need(host.register_target, "0022c650 (002841d8)")(body);
}

void Tank::set_turret(std::uint32_t yaw, std::uint32_t pitch, std::uint32_t flash, std::optional<std::uint32_t> muzzle) {
    turret_ = tank_turret(yaw, pitch, flash, muzzle.value_or(0));  // the old one is freed (0036b130)
}
namespace {
// 00283260 / 002833d0 (and 00287c10): +4 = the wheel, +0 = the model, +24 =
// the wheel's +144, +28 = 0 (the old tread is freed, 0036b130).
TankTread make_tread(std::uint32_t model, std::uint32_t wheel, TankHost& host) {
    TankTread t;
    t.model_0 = model;
    t.wheel_4 = wheel;
    t.last_24 = need(host.rotation_144, "the wheel NIE's +144 (0028326c)")(wheel);
    return t;
}
}  // namespace
void Tank::set_left_tread(std::uint32_t model, std::uint32_t wheel, TankHost& host) { left_ = make_tread(model, wheel, host); }
void Tank::set_right_tread(std::uint32_t model, std::uint32_t wheel, TankHost& host) { right_ = make_tread(model, wheel, host); }
void Tank::set_pitch_limit(float lo, float hi) {
    if (!turret_) return;
    turret_->pitch_lo_18 = mul(degree, lo);
    turret_->pitch_hi_1c = mul(degree, hi);
}
void Tank::set_rot_limit(float lo, float hi) {
    if (!turret_) return;
    turret_->yaw_lo_20 = mul(degree, lo);
    turret_->yaw_hi_24 = mul(degree, hi);
}
void Tank::set_turret_accel(float accel) {
    if (turret_) turret_->accel_2c = accel;
}
void Tank::set_firing_cone(float degrees) {
    if (turret_) turret_->cone_50 = mul(degree, degrees);
}
void Tank::set_weapon(std::int32_t id) {
    if (!turret_) return;
    turret_->weapon_6c = id;
    turret_->weapon_set = true;                                     // +68 = 00147840(*004ffd30, id)
}
void Tank::move(float speed, TankHost& host) {
    need(host.set_speed, "0023e3d0 (00282e3c)")(body_, speed);
    need(host.play, "the NIE vt+114 (00282e4c)")(body_);
    if (turret_) need(host.stop, "the NIE vt+118 (00282e6c)")(turret_->yaw_0);
}
void Tank::set_enemy(const TankObjectRef& enemy) {
    if (turret_) {
        turret_->stopped_54 = false;
        turret_->enemy_5c = enemy.object ? enemy : TankObjectRef{};
    }
    auto_70_ = false;
}
void Tank::aim_at(const TankObjectRef& enemy) { set_enemy(enemy); }
void Tank::resume_aiming() {
    if (turret_) turret_->stopped_54 = false;
    auto_70_ = true;
}
void Tank::stop_firing() {
    if (turret_) turret_->stopped_54 = true;
}
void Tank::reset(TankHost& host) {
    need(host.reset, "the NIE vt+11c (00282bcc)")(body_);
    if (turret_) turret_->previous_64 = 0;
}
void Tank::net_create(TankHost& host) const {
    need(host.net_create, "the cNetCreateTankMsg queue 003c9420 (00282c5c)")(body_, static_cast<std::int32_t>(id_));
}

void Tank::update(float dt, TankHost& host) {
    if (turret_) {
        auto& t = *turret_;
        if (!(t.fire_timer_10 <= 0.0F)) t.fire_timer_10 = sub(t.fire_timer_10, dt);
        const bool aligned = tank_turret_aim(t, dt, host);
        if (enemy_valid(t, host) && !t.stopped_54 && aligned && t.fire_timer_10 <= 0.0F)
            tank_turret_fire(t, handle_, host);            // 00285918 (solo)
        if (!(retarget_74_ <= 0.0F)) retarget_74_ = sub(retarget_74_, dt);
        else if (auto_70_) {
            retarget_74_ = f32(0x40400000U);                        // 0048b3d8 (3.0)
            const std::uint64_t found = tank_target_search(t, host);
            set_enemy(found ? TankObjectRef{found, need(host.object_id, "the target's +c (002855c4)")(found)}
                            : TankObjectRef{});
            auto_70_ = true;
        }
    }
    if (left_) tank_tread_update(*left_, host);
    if (right_) tank_tread_update(*right_, host);
}

Tank& MissionTanks::create(std::uint32_t id, std::uint32_t body, std::uint64_t handle, TankHost& host) {
    if (find(id)) fail("a second tank with the ID " + std::to_string(id));
    tanks_.emplace_back(id, body, handle, host);
    return tanks_.back();
}
Tank* MissionTanks::find(std::uint32_t id) {
    for (auto& t : tanks_)
        if (t.id() == id) return &t;
    return nullptr;
}
Tank& MissionTanks::at(std::uint32_t id) {
    if (auto* t = find(id)) return *t;
    fail("no tank with the ID " + std::to_string(id));
}
const Tank* MissionTanks::find_handle(std::uint64_t handle) const {
    for (const auto& t : tanks_)
        if (t.handle() == handle) return &t;
    return nullptr;
}
void MissionTanks::update(float dt, TankHost& host) {
    for (auto& t : tanks_) t.update(dt, host);
}
}  // namespace omega
