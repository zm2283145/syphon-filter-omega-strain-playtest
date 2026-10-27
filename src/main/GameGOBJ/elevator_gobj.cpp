#include "elevator_gobj.hpp"
#include "hotbox_world.hpp"
#include "path_motion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
// cop1 results flush denormals to zero (tools/ee/ee.py ftz).
float ftz(float value) {
    if (value != 0.F && std::fabs(value) < std::numeric_limits<float>::min()) return std::copysign(0.F, value);
    return value;
}
// div.s as tools/ee/ee.py runs it: a zero divisor gives the largest float
// signed as S * T (positive for a zero S).
float ee_div(float s, float t) {
    if (t != 0.F) return ftz(s / t);
    const double sign = s != 0.F ? static_cast<double>(s) * static_cast<double>(t) : 1.0;
    return std::copysign(std::numeric_limits<float>::max(), static_cast<float>(sign));
}
std::array<float, 16> placement_matrix(const std::array<float, 9>& basis, const Vec3& position) {
    return {basis[0], basis[1], basis[2], 0.F, basis[3], basis[4], basis[5], 0.F,
            basis[6], basis[7], basis[8], 0.F, position.x, position.y, position.z, 1.F};
}
// The VU row product vmulax / vmadday / vmaddaz / vmaddw (00217250.., 00218520..):
// row i = ((a_i.x * B0 + a_i.y * B1) + a_i.z * B2) + a_i.w * B3, every product
// and sum flushed.
std::array<float, 16> vu_matrix_product(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> out{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t c = 0; c < 4; ++c) {
            float acc = ftz(a[i * 4] * b[c]);
            acc = ftz(acc + ftz(a[i * 4 + 1] * b[4 + c]));
            acc = ftz(acc + ftz(a[i * 4 + 2] * b[8 + c]));
            out[i * 4 + c] = ftz(acc + ftz(a[i * 4 + 3] * b[12 + c]));
        }
    return out;
}
std::size_t sound_slot(LiftSound sound) {
    switch (sound) {
    case LiftSound::Ding: return 0;
    case LiftSound::Loop: return 1;
    case LiftSound::Start: return 2;
    case LiftSound::Stop: return 3;
    }
    throw std::invalid_argument("Unknown lift sound descriptor");
}
// A record reference (class, serial) as the 0040c480 key (class << 16 | serial).
std::vector<std::uint32_t> reference_keys(std::uint32_t id, const std::vector<std::array<std::uint32_t, 2>>& refs) {
    std::vector<std::uint32_t> keys;
    for (const auto& r : refs) {
        if (r[0] >= 0x100 || r[1] >= 0x10000)
            throw std::runtime_error("ElevatorGOBJ " + std::to_string(id) + ": reference (" + std::to_string(r[0]) + ", " +
                                     std::to_string(r[1]) + ") is not a 0040c480 key");
        keys.push_back((r[0] << 16) | r[1]);
    }
    return keys;
}
}

ElevatorGOBJ::ElevatorGOBJ(const PopulationObject& object, std::size_t recipients)
    : ElevatorGOBJ(object.motion.points, object.motion.rotations,
                   // 00219820 stores the file speed doubled in +d4; 00215700 halves it (00215adc div.s 2.0).
                   ee_div(object.motion.speed * 2.F, 2.F), recipients) {
    if (object.kind != 3) throw std::runtime_error("ElevatorGOBJ requires a kind-3 population object");
    // 00215700 -> the GameGOBJ base 0021add0 hands a record's script name to 003d9d30 (0021affc), which binds the
    // script type's member storage to the object's +18 holder (TOKYO's ScriptElevator): the script host does it
    // (MissionScriptHost::bind_entity_script, checkpoint 132el6).
    script_ = object.script;
    // The GameGOBJ base 0021b388..0021b440: +70 = the reference count, each one
    // 0040c480(ID) into +7c (null kept); 00215b88: the next record word sizes
    // +e8 (1bac40) and each extra is 0040c480(ID) into +f0. The caller's door
    // view resolves the IDs (and throws for one the level does not hold).
    set_doors(reference_keys(object.id, object.references), reference_keys(object.id, object.extra_references));
    matrix_ = placement_matrix(object.basis, object.position);
}

ElevatorGOBJ::ElevatorGOBJ(std::vector<Vec3> points, std::vector<std::array<float, 4>> rotations, float speed,
                           std::size_t recipients)
    : points_(std::move(points)), rotations_(std::move(rotations)), speed_(speed), recipients_(recipients) {
    if (points_.size() != rotations_.size() || points_.size() > 65535)
        throw std::runtime_error("ElevatorGOBJ requires matching point and rotation arrays");
    if (!std::isfinite(speed_)) throw std::runtime_error("Nonfinite lift speed");
    for (const auto& p : points_)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            throw std::runtime_error("Nonfinite lift path point");
    for (const auto& q : rotations_)
        for (const float v : q)
            if (!std::isfinite(v)) throw std::runtime_error("Nonfinite lift path quaternion");
    // 00219820: +b4 holds one length per point; the first n-1 are the point
    // distances (<= 0.01 as a double becomes 1.0), the last is 0.0.
    for (std::size_t i = 1; i < points_.size(); ++i) {
        const Vec3 a = points_[i - 1], b = points_[i];
        const float x = b.x - a.x, y = b.y - a.y, z = b.z - a.z;
        float distance = std::sqrt((x * x + y * y) + z * z);
        if (double(distance) <= 0.01) distance = 1.F;
        lengths_.push_back(distance);
    }
    if (!points_.empty()) lengths_.push_back(0.F);
    // 00215cc0: one lock per point, cleared; 00215d10: floor names "Floor %d" (i + 1).
    locks_.assign(points_.size(), 0);
    for (std::size_t i = 0; i < points_.size(); ++i) floor_names_.push_back("Floor " + std::to_string(i + 1));
    // 00215ab0..00215b54: the four descriptors 001ca010(d, 0, name, lift, 0).
    sounds_ = {"O_PASSENG_BELL", "O_PASSENG_MOVE", "O_PASSENG_START", "O_PASSENG_STOP"};
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    matrix_ = placement_matrix(identity, points_.empty() ? Vec3{} : points_.front());
}

const std::string& ElevatorGOBJ::sound(LiftSound sound) const { return sounds_[sound_slot(sound)]; }

Vec3 ElevatorGOBJ::point(std::int32_t index) const {
    if (index < 0 || index >= static_cast<std::int32_t>(points_.size()))
        throw std::runtime_error("Lift path read outside the point array (00216c60)");
    return points_[static_cast<std::size_t>(index)];
}

void ElevatorGOBJ::notice(LiftEvent::Kind kind, std::int32_t a, std::int32_t b) {
    LiftEvent event;
    event.kind = kind;
    event.a = a;
    event.b = b;
    events_.push_back(event);
}

void ElevatorGOBJ::sound_event(LiftEvent::Kind kind, LiftSound sound) {
    LiftEvent event;
    event.kind = kind;
    event.a = static_cast<std::int32_t>(sound);
    events_.push_back(event);
}

void ElevatorGOBJ::set_doors(std::vector<std::uint32_t> links, std::vector<std::uint32_t> extras) {
    links_ = std::move(links);
    extras_ = std::move(extras);
}

LiftDoor ElevatorGOBJ::door(std::uint32_t id) const {
    if (!view_) throw std::runtime_error("ElevatorGOBJ: no door view for link / extra " + std::to_string(id));
    return view_(id);
}

// 00214080 (open) / 00214240 (closed): every link that is a GameGOBJ (+4c ==
// *004f5468) of kind 2 (+68) is stopped (+dc) at its last (+98 - 1) / first
// waypoint; others are skipped, so no links answer 1.
bool ElevatorGOBJ::links_at(bool open) const {
    for (const auto id : links_) {
        const LiftDoor d = door(id);
        if (!d.game_gobj || d.kind != 2) continue;
        if (!d.stopped || d.index != (open ? d.points - 1 : 0)) return false;
    }
    return true;
}

// 00213fe0 (open) / 002141a0 (closed): the floor's two extras (t from 2f
// while t < +ec and t <= 2f + 1, f = the index); kind-2 ones only; a negative
// index answers 0.
bool ElevatorGOBJ::extras_at(bool open) const {
    const std::int32_t f = state_.index;
    if (f < 0) return false;
    const std::int64_t count = static_cast<std::int64_t>(extras_.size());
    for (std::int64_t t = 2LL * f; t < count && t <= 2LL * f + 1; ++t) {
        const LiftDoor d = door(extras_[static_cast<std::size_t>(t)]);
        if (d.kind != 2) continue;
        if (!d.stopped || d.index != (open ? d.points - 1 : 0)) return false;
    }
    return true;
}

// 00213fa0: 00214080 || 00213fe0.
bool ElevatorGOBJ::doors_open() const { return links_at(true) || extras_at(true); }
// 00214160: 00214240 && 002141a0.
bool ElevatorGOBJ::doors_closed() const { return links_at(false) && extras_at(false); }

void ElevatorGOBJ::door_trigger(std::int32_t which, std::size_t index, std::uint32_t id) {
    LiftEvent event;
    event.kind = LiftEvent::Kind::DoorTrigger;
    event.a = which;
    event.b = static_cast<std::int32_t>(index);
    event.object = id;
    events_.push_back(event);
}

// 00214470: stop the loop (+2e0, 0020eba0); 1 when there are links (+70 > 0);
// each GameGOBJ kind-2 link gets a trigger (0x4e0000 - 0x50f0 = 004daf10).
bool ElevatorGOBJ::trigger_links() {
    sound_event(LiftEvent::Kind::StopSound, LiftSound::Loop);
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const LiftDoor d = door(links_[i]);
        if (d.game_gobj && d.kind == 2) door_trigger(0, i, links_[i]);
    }
    return !links_.empty();
}

// 00214310 (its callers test the index first): 1 when the floor has an extra;
// each kind-2 extra gets the same trigger.
bool ElevatorGOBJ::trigger_extras() {
    const std::int32_t f = state_.index;
    bool any = false;
    const std::int64_t count = static_cast<std::int64_t>(extras_.size());
    for (std::int64_t t = 2LL * f; t < count && t <= 2LL * f + 1; ++t) {
        any = true;
        const auto id = extras_[static_cast<std::size_t>(t)];
        if (door(id).kind == 2) door_trigger(1, static_cast<std::size_t>(t), id);
    }
    return any;
}

// The handle test (00218240..00218298, 00256578..002565c8): a null object is
// dead; otherwise the object's +c must still be the handle's id.
bool ElevatorGOBJ::rider_live(const LiftRider& rider) const {
    if (rider.object == 0) return false;
    if (!rider_view_) throw std::runtime_error("ElevatorGOBJ: no rider view for rider " + std::to_string(rider.object));
    return rider_view_(rider);
}

void ElevatorGOBJ::attach_rider(const LiftRider& rider) {
    // 00256500: the resolved objects compared (a dead entry resolves to null).
    const std::uint32_t needle = rider_live(rider) ? rider.object : 0;
    for (const auto& r : riders_)
        if ((rider_live(r) ? r.object : 0) == needle) return;
    riders_.push_back(rider);   // 002564a0 -> 00258e10(+84, end, 1, &handle)
}

// 00218200(M, s3) with M = 0013eca0(the old root) x the new root.
void ElevatorGOBJ::carry(const std::array<float, 16>& old, bool teleport) {
    if (riders_.empty() && links_.empty()) return;
    carry_pass(vu_matrix_product(hotbox_rigid_inverse(old), matrix_), teleport);
}

// 00218200: the rider pass (+84: count +88, data +8c, s2 = the count read
// once, s1 the index) then each link's root (its +50 component +10..+4f)
// times M, to its vt+94 (s3 = 0) or as a 004d9360 message (s3 = 1).
void ElevatorGOBJ::carry_pass(const std::array<float, 16>& m, bool teleport) {
    std::int32_t count = static_cast<std::int32_t>(riders_.size());
    for (std::int32_t i = 0; i < count; ++i) {
        const LiftRider r = riders_[static_cast<std::size_t>(i)];
        if (rider_live(r)) {
            LiftEvent event;
            event.kind = LiftEvent::Kind::RiderCarry;
            event.object = r.object;
            event.a = static_cast<std::int32_t>(r.id);
            event.b = teleport ? 1 : 0;
            if (teleport) {
                // 002182b0..00218384: 0014b160(msg + 30, 0014b210() = 0055c9f0, 0015a400(M + 0x30) with w = 0).
                event.matrix = {1.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, m[12], m[13], m[14], 0.F};
            } else {
                // 00218390..002183f8: the rider's vt+a8(M + 0x30).
                for (std::size_t k = 12; k < 16; ++k) event.matrix[k] = m[k];
            }
            events_.push_back(event);
            continue;
        }
        // 00218408..0021847c: a dead handle is removed (each later entry copied down, 002186d0 /
        // 002186b0, then the count 002186a0), the loop's count and index step back.
        riders_.erase(riders_.begin() + i);
        --count;
        --i;
    }
    for (std::size_t i = 0; i < links_.size(); ++i) {
        LiftEvent event;
        event.kind = LiftEvent::Kind::Carry;
        event.a = static_cast<std::int32_t>(i);
        event.b = teleport ? 1 : 0;
        event.object = links_[i];
        event.matrix = vu_matrix_product(door(links_[i]).matrix, m);
        events_.push_back(event);
    }
}

void ElevatorGOBJ::pose(int which, const std::array<float, 9>& basis, const Vec3& position) {
    const auto old = matrix_;
    matrix_ = placement_matrix(basis, position);
    LiftEvent event;
    event.kind = LiftEvent::Kind::Pose;
    event.a = which;
    event.matrix = matrix_;
    events_.push_back(event);
    // 00218200 follows every pose write: s3 = 0 after vt+94 (00218188), 1
    // after vt+ac (the endpoints and the stop target 002172d0).
    carry(old, which == 1);
}

// 00216c60 for a lift (mode +d8 = 4, delay +c8 = 0).
void ElevatorGOBJ::path_update(float timestep) {
    auto& s = state_;
    // 00216c84: stopped (+dc) or no points (+98) -> only 003ce650.
    if (s.stopped || points_.empty()) return;
    const std::int32_t n = static_cast<std::int32_t>(points_.size());
    const auto clamp = [n](std::int32_t i) { return std::clamp(i, 0, n - 1); };
    float length;
    if (s.direction == 1) {
        length = lengths_[static_cast<std::size_t>(clamp(s.index))];
    } else {
        if (s.index == -1) s.index = 0;
        if (s.direction == -1 && s.index == 0) s.direction = 0;
        if (s.index == n && s.direction == 1) s.direction = 0;
        if (s.index >= n) s.index = n - 1;
        length = lengths_[static_cast<std::size_t>(clamp(s.index + s.direction))];
    }
    float progress = ftz(s.progress + ee_div(ftz(speed_ * timestep), length));
    if (!(progress <= 1.F)) progress = 1.F;
    s.progress = progress;
    const Vec3 p0 = point(clamp(s.index)), p1 = point(clamp(s.index + s.direction));
    if (progress == 1.F) {
        s.index += s.direction;
        notice(LiftEvent::Kind::PathedNotice, 2, s.index);
        if (s.index == s.stop_target) {
            // 002170a8: the +cc point (218ea0 -> 00218fd0, clamped) and rotation through vt+ac and
            // 00218200(M, 1); stopped, PathedNotice(0, index), +cc = -1, +e0 = 0.
            const std::int32_t c = clamp(s.stop_target);
            pose(1, quaternion_basis(rotations_[static_cast<std::size_t>(c)]), point(c));
            s.stopped = true;
            notice(LiftEvent::Kind::PathedNotice, 0, s.index);
            s.stop_target = -1;
            s.progress = 0.F;
            return;
        }
        s.progress = 0.F;
        const std::int32_t r = s.index >= n ? n - 1 : s.index;
        // Mode 4 (00217110..): the endpoint rotation and point written through vt+ac.
        pose(1, quaternion_basis(rotations_[static_cast<std::size_t>(clamp(r))]), point(s.index));
        s.stopped = true;
        notice(LiftEvent::Kind::PathedNotice, 0, s.index);
        s.progress = 1.F;
        return;
    }
    const Vec3 delta{(p1.x - p0.x) * progress, (p1.y - p0.y) * progress, (p1.z - p0.z) * progress};
    const Vec3 position{p0.x + delta.x, p0.y + delta.y, p0.z + delta.z};
    // 00218c80(to = index + direction, from = index, progress) -> 00190750.
    const auto q = path_nlerp(rotations_[static_cast<std::size_t>(clamp(s.index))],
                              rotations_[static_cast<std::size_t>(clamp(s.index + s.direction))], progress);
    pose(0, quaternion_basis(q), position);
}

void ElevatorGOBJ::update(float timestep) {
    if (!std::isfinite(timestep)) throw std::runtime_error("Nonfinite lift timestep");
    // 002135c0: (solo: the replication block is skipped), vt+88 = 00213ac0
    // (unparented: no effect), then 00216c60.
    path_update(timestep);
    auto& s = state_;
    if (s.busy == 0) {
        if (s.destination != s.index) s.busy = 1;
        return;
    }
    if (s.busy != 1 || !s.stopped) return;
    const bool linked = !links_.empty();
    if (s.destination != s.index) {
        // 00213804: links with doors open close them first; move once they are closed (or without links).
        if (linked && doors_open()) {
            close();
            return;
        }
        if (!doors_closed() && linked) return;
        sound_event(LiftEvent::Kind::PlaySound, LiftSound::Start);
        sound_event(LiftEvent::Kind::PlaySound, LiftSound::Loop);
        s.stopped = false;
        s.progress = 0.F;
        return;
    }
    // 00213918: without links arrive (00213b20); with links and the doors closed, ElevatorNotice(0, 0) and
    // open (00213dc0); finish once they are open (or without links), else wait.
    if (!linked) arrive();
    if (linked && doors_closed()) {
        notice(LiftEvent::Kind::ElevatorNotice, 0, 0);
        open();
        return;
    }
    if (!doors_open() && linked) return;
    s.busy = 0;
    enable_recipients();
    std::int32_t next = -1;
    if (s.queue_count > 0) {
        --s.queue_count;
        next = s.queue[static_cast<std::size_t>(s.queue_count)];
    }
    if (next != -1) seek(next, true);
}

// 00213b20.
void ElevatorGOBJ::arrive() {
    sound_event(LiftEvent::Kind::StopSound, LiftSound::Loop);
    sound_event(LiftEvent::Kind::PlaySound, LiftSound::Ding);
    notice(LiftEvent::Kind::ElevatorNotice, 0, 0);
    enable_recipients();
}

// 002154b0 / 00215430: each registered recipient's vt+64(-1) / vt+68(-1).
void ElevatorGOBJ::enable_recipients() {
    for (std::size_t i = 0; i < recipients_; ++i) notice(LiftEvent::Kind::EnableRecipient, static_cast<std::int32_t>(i), 0);
}
void ElevatorGOBJ::disable_recipients() {
    for (std::size_t i = 0; i < recipients_; ++i) notice(LiftEvent::Kind::DisableRecipient, static_cast<std::int32_t>(i), 0);
}

// 00214c90(floor, ui, 0), solo: a negative floor with ui opens the HUD's floor chooser 00214bd0 at once.
void ElevatorGOBJ::seek(std::int32_t floor, bool ui) {
    if (ui && floor < 0) {
        notice(LiftEvent::Kind::ChooseFloor, state_.index, 0);
        return;
    }
    if (floor < 0) return;
    auto& s = state_;
    if (floor >= static_cast<std::int32_t>(locks_.size()) || locks_[static_cast<std::size_t>(floor)] != 0) {
        notice(LiftEvent::Kind::ElevatorNotice, 4, floor);
        return;
    }
    s.destination = floor;
    if (s.index < floor || s.index > floor) {
        notice(LiftEvent::Kind::ElevatorNotice, 1, floor);
        s.direction = s.index < floor ? 1 : -1;
        disable_recipients();
        return;
    }
    s.direction = 0;
    open();
}

bool ElevatorGOBJ::receive_trigger(std::int32_t action, std::int32_t floor, LiftAgent agent, std::uint32_t agent_key) {
    auto& s = state_;
    switch (action) {
    case 10:
        // 00214920: the player opens the floor chooser (00214c90(-1, 1, 0)); another agent's trigger
        // (+24 = 1, +28 = -1, +2c = 0, +30 = its +c) goes to 003fb020(lift +c, msg, 0, 0).
        if (agent == LiftAgent::Player) seek(-1, true);
        else if (agent == LiftAgent::Other) {
            LiftEvent event;
            event.kind = LiftEvent::Kind::NetworkTrigger;
            event.object = agent_key;
            event.a = static_cast<std::int32_t>(agent_key);
            events_.push_back(event);
        }
        return true;
    case 6: set_lock(floor, 0); return true;  // 00214908 -> 002156c0(f, 0)
    case 5: set_lock(floor, 1); return true;  // 002148f0 -> 002156c0(f, 1)
    case 1:
    case 4:
        // 00214828 (solo).
        if (floor < 0 || s.busy == 0) {
            seek(floor, true);
            if (floor >= 0) s.busy = 1;
            return true;
        }
        if (s.destination == floor || floor == -1 || s.queue_count >= 5) return true;
        for (std::int32_t k = 0; k < s.queue_count; ++k)
            if (s.queue[static_cast<std::size_t>(k)] == floor) return true;
        s.queue[static_cast<std::size_t>(s.queue_count++)] = floor;
        return true;
    default:
        notice(LiftEvent::Kind::Forward, 0, 0);
        return false;
    }
}

void ElevatorGOBJ::receive_other() {
    notice(LiftEvent::Kind::Forward, 0, 0);
}

void ElevatorGOBJ::receive_forwarded_trigger() {
    state_.stopped = false;                              // 0021909c: +dc = 0
    notice(LiftEvent::Kind::PathedNotice, 1, -1);        // 002190b0..002190e0
    state_.progress = 0.F;                               // 002190f4: +e0 = 0
}

// 00214aa0: +790 (the network lift, 002136f0) stays 0 solo, so 002ec460 is not reached. -0xff: nothing.
// Otherwise 003c9e30(*004f5440), vtable 004daf10, +24 = 1, +28 = the floor, +2c = 0, +30 = the reply's +24
// agent's +c (0 without one), 003c9420(msg, lift, 0) and 003c9d60.
void ElevatorGOBJ::receive_menu_choice(std::int32_t floor, bool agent) {
    if (floor == -0xff) return;
    notice(LiftEvent::Kind::Trigger, floor, agent ? 1 : 0);
}

void ElevatorGOBJ::seek_floor(std::int32_t floor, bool ui) {
    seek(floor, ui);
}

// 00213dc0: with the links closed (00214240) trigger them (00214470; when it
// answers 1, ElevatorNotice(2, 0)) and ding; otherwise answer 1 silently. Then,
// with the floor's extras closed (002141a0), trigger them (00214310).
bool ElevatorGOBJ::open() {
    bool result = true;
    if (links_at(false)) {
        result = trigger_links();
        if (result) notice(LiftEvent::Kind::ElevatorNotice, 2, 0);
        sound_event(LiftEvent::Kind::PlaySound, LiftSound::Ding);
    }
    if (extras_at(false)) trigger_extras();
    return result;
}
bool ElevatorGOBJ::open_doors() { return open(); }

// 00213ee0: with the links open (00214080) trigger them (00214470; when it
// answers 1, ElevatorNotice(3, 0)); then with the extras open (00213fe0)
// trigger them (00214310). Answers 1 unless 00214470 ran and answered 0.
bool ElevatorGOBJ::close() {
    bool result = true;
    if (links_at(true)) {
        result = trigger_links();
        if (result) notice(LiftEvent::Kind::ElevatorNotice, 3, 0);
    }
    if (extras_at(true)) trigger_extras();
    return result;
}
bool ElevatorGOBJ::close_doors() { return close(); }

// 00214fa0: busy -> 1 (Moving); doors closed (00214160) or open (00213fa0)
// -> 0; otherwise 2 (the doors are moving).
std::int32_t ElevatorGOBJ::get_state() const {
    if (state_.busy == 1) return 1;
    if (doors_closed() || doors_open()) return 0;
    return 2;
}

void ElevatorGOBJ::set_lock(std::int32_t floor, std::uint8_t value) {
    if (floor < 0 || floor >= static_cast<std::int32_t>(locks_.size())) return;
    locks_[static_cast<std::size_t>(floor)] = value;
}
void ElevatorGOBJ::lock_floor(std::int32_t floor) { set_lock(floor, 1); }
void ElevatorGOBJ::unlock_floor(std::int32_t floor) { set_lock(floor, 0); }

// 00215010: a floor in [0, +fc) replaces its name; others are ignored.
void ElevatorGOBJ::set_floor_name(std::int32_t floor, std::string name) {
    if (floor < 0 || floor >= static_cast<std::int32_t>(floor_names_.size())) return;
    floor_names_[static_cast<std::size_t>(floor)] = std::move(name);
}

void ElevatorGOBJ::set_sound(LiftSound sound, std::string name) { sounds_[sound_slot(sound)] = std::move(name); }

ElevatorGOBJ& ElevatorWorld::add(std::uint32_t id, ElevatorGOBJ lift) {
    if (find(id)) throw std::runtime_error("ElevatorWorld: lift " + std::to_string(id) + " registered twice");
    lifts_.emplace_back(id, std::move(lift));
    return lifts_.back().second;
}
ElevatorGOBJ* ElevatorWorld::find(std::uint32_t id) {
    for (auto& [key, lift] : lifts_)
        if (key == id) return &lift;
    return nullptr;
}
ElevatorGOBJ& ElevatorWorld::at(std::uint32_t id) {
    if (auto* lift = find(id)) return *lift;
    throw std::runtime_error("ElevatorWorld: " + std::to_string(id) + " is not a hosted lift");
}
}
