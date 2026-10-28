#pragma once
// The hitscan bullet pool (COMBAT.md §4): 50 records at 004ffe60 (stride
// 0x150, bullet.cc). 00277e20 traces the request ray against static
// collision through the LOS manager (00148290 -> 003c26d0 -> 003c3a60) and
// fills a free record (00277aa0); the per-frame manager 00275670 runs the
// update 00276460 on every active record after the actor traversals. A bullet
// travels at its weapon's +104 units per second along its frame; each update
// scans the world objects ahead of it (0022ba30: candidate distance, range-band
// damage, the object's virtual +104 hit entry) and, at the end of its range
// or at the static hit point, reports the static impact, the AI impact noise
// and the damage call to the static object. Presentation (tracers 003a2400,
// impact sounds and decals 00189560 / 00279ca0 / 00232460, the blood decal
// 002797c0) is recorded as events; retail tracer geometry and the texture
// handle stored by 00277cf8 are rendered by bullet_render.hpp at the scene's
// 002755f0 stage. Checkpoint 132uk4: hold classes 8
// and 9 at the end of travel (002774f8..00277928: the agent statistic 10, the
// area object 0025a2d0 and the blast 0021a200 through the host, the hold-9
// sound G_GRENEXP_INCIN as an event) run. Checkpoint 132uk6: explosive
// weapons (record +c4 > 0: 00277388..002774dc, the effect 00232460, the blast
// 0021a200 at 002774a4 and the scorch decal 002797c0) and the hold block's
// effect 00232460 (record +d8 != -1) run. A nonnegative tracer-particle
// resource still throws because no retail image writer produces one.
#include "bullet_hit.hpp"
#include "bullet_impact.hpp"
#include "explosion.hpp"
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// The static LOS result 00277e20 copies into the bullet (+90..+100).
struct BulletStaticHit {
    RootVector point{};                      // +100 (and the hit distance +8)
    RootVector normal{};
    std::uint64_t object{};                  // query result +64: the hit object (bullet +70 weak handle)
    std::array<std::uint32_t,4> source{};    // surface descriptor words (the material lookup input)
};
// One entry of the world object list 0022ba30 walks (004f78dc), in list order,
// with a valid weak handle and non-null bounds (virtual +88).
struct BulletCandidate {
    std::uint64_t object{};
    RootVector minimum{},maximum{};
    RootVector position_e4{};                // virtual +e4 (the designated target uses +c0 instead)
};

// A node of the target manager's list 004f78dc (004f77c0 +11c): the weak
// handle 0022c650 pushes (the object and its +c at registration).
struct BulletListEntry {
    std::uint64_t object{};
    std::int32_t id{-1};
};
// The object views 0022ba30 reads while walking the list.
struct BulletListHost {
    std::function<std::int32_t(std::uint64_t object)> id_c;                                   // object +c
    std::function<std::optional<std::array<RootVector,2>>(std::uint64_t object)> bounds_88;   // virtual +88 (nullopt: null)
    std::function<RootVector(std::uint64_t object)> position_e4;                              // virtual +e4
};
// 0022ba30's walk (0022bb10..0022bb90): an entry is skipped when its object
// is null or the object's +c no longer equals the stored id (the weak handle
// does not resolve), or when its virtual +88 returns null; the shooter (request
// +0) is skipped by scan_bullet_candidates. List order is kept.
std::vector<BulletCandidate> target_list_candidates(const std::vector<BulletListEntry>& list,const BulletListHost& host);

struct BulletEvent {
    enum class Kind {
        created,           // 00277aa0 filled record `bullet`
        pool_full,         // 00277e20 found no free record (no bullet)
        candidate,         // 0022ba30 offered `object` at `distance` (category = the +104 result, -1 not called)
        near_miss_sound,   // 0022ba30: 0020f990(desc 004ef3f0, object) to 004ffb50, transport 0
        static_impact,     // 00276460 material / sound / decal / effect block for the static hit (00189560 ...)
        ai_impact_noise,   // 0016b420(msg, shooter id, point, 4, 1.0, record +fc) to "AI Sound System", transport 0
        static_damage,     // static object virtual +100(object, shooter, 1, type, null, 0, null, amount)
        blood_decal,       // 002797c0(004ffc3c, end + 0.2 (frame +50 - end), id 0x26's effect, 0, 2.0) behind a head-shot actor near a wall
        // The hold-8/9 block at the end of travel (002774f8..00277924), in its order:
        statistic,         // 00333f10(001314f0(004ffb50, -1), value 10, 1): hold 9, a local player's shot, a non-player human victim
        area,              // 0025a2d0(new 0xe0 object, owner `object` or null, &frame +50 = point, record +148 = value, 1)
        blast,             // 0021a200(source `object` or null, 1, 1, &frame +50 = point, 1, 1, 0.0, 10.0, 10.0)
        hold_sound,        // hold 9: 001ca210(desc, 1, "G_GRENEXP_INCIN", 0, 0), 0020f860(msg, desc, +100 = point), +20 = 1, to 004ffb50, transport 0;
                           // value 1: no static hit, +100 is stale stack on the EE and point is the port-only frame +50 (132uk4)
        // Checkpoint 132uk6: the effect of the explosive branch (002773b0) and of the hold block (0027758c), then
        // the explosive branch's blast and decal (00277388..002774dc):
        impact_effect,     // 00232460(record +d8 = value, &frame +20 = frame)
        explosion,         // 0021a200(source `object` or null, record +64 = value, 1, point, 0, 1, 1.0 = amount, +c4, +c8)
        scorch_decal,      // 002797c0(004ffc3c, point, *00236f20(0x25) = value, 0, 5.0 = amount)
        ended,             // +116 = 0 (the record frees itself on a later update)
        freed,             // +115 = 0, unlinked from its pellet group (002756e0)
    } kind{};
    std::size_t bullet{};
    std::uint64_t object{};
    float distance{};
    std::int32_t category{-1};
    std::int32_t value{};                    // type / effect id / shooter id / item kind
    float amount{};                          // damage / noise radius
    RootVector point{};
    std::array<std::uint32_t,4> source{};
    // static_impact with a static_material host: the block's choices (bullet_impact.hpp).
    std::optional<BulletImpact> impact;
    std::optional<ShotFrame> frame;          // impact_effect: the bullet's frame +20..+5c (132uk6)
    bool operator==(const BulletEvent&) const=default;
};

// 0025a2d0 as the hold block calls it (00277780..002777f0) after 0036b180(0,
// 0xe0, "bullet.cc", 0x1f5): the owner is the shooter while its weak handle
// (+68 / +6c) resolves, else null ({0, -1}).
struct BulletAreaCall {
    ObjectWeakHandle owner;
    RootVector point{};                      // the bullet's frame +50 after the travel step
    std::int32_t record{};                   // +148
    bool flag{};                             // t0 = 1
    bool operator==(const BulletAreaCall&) const=default;
};
// 0021a200 as the hold block calls it (002777f8..00277884); `source` as BulletAreaCall::owner.
// Checkpoint 132uk6: the explosive branch's call (00277418..002774a8) uses the same shape.
struct BulletBlastCall {
    ObjectWeakHandle source;
    std::int32_t damage{};                   // a1 = 1
    bool kind{};                             // a2 = 1
    RootVector point{};                      // a3: the frame +50 copy
    bool skip_humans{},local{};              // t0 = 1, t1 = 1
    float strength{},inner{},outer{};        // f12 = 0.0, f13 = f14 = 10.0
    bool operator==(const BulletBlastCall&) const=default;
};

struct BulletPoolHost {
    // 00148290(LOS, out, query, start, displacement, excluded object, 0, 0,
    // 0x10, 0x80, 00504030, 1): required mask 0, excluded mask 0x10, then a
    // second pass over 0x80 (water) surfaces (003c4a80) that is not native: a
    // host must throw when such a surface lies on the segment.
    std::function<std::optional<BulletStaticHit>(const RootVector& start,const RootVector& displacement,std::uint64_t excluded)> static_hit;
    std::function<std::int32_t(std::uint64_t object)> object_id;            // object +c (weak handle generation)
    std::function<bool(std::uint64_t object,std::int32_t id)> valid;         // the weak handle still resolves
    std::function<std::vector<BulletCandidate>()> candidates;
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> eye_c0;   // designated target virtual +c0
    // Object virtual +104 (00188780 for actors): the hit entry; returns the category.
    std::function<std::uint8_t(std::uint64_t object,BulletHitRequest& request)> hit;
    // The candidate's +58 controller answers +54 and its +14 is set (near-miss sound).
    std::function<bool(std::uint64_t object)> local_player_controller;
    // Static object virtual +100 at the end of travel.
    std::function<void(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount)> static_damage;
    std::function<bool(std::uint64_t object)> is_actor;                      // +4c is the actor class word 0049d010
    std::function<std::int32_t(std::int32_t index)> effect_id;              // *00236f20(index)
    // 00189560(bullet +d0): the material row of the static hit's surface. Absent
    // or null: a static impact is reported without its presentation.
    std::function<const MaterialRow*(const BulletStaticHit& hit)> static_material;
    // The hold-8/9 block: the object's +58 controller is non-null and answers
    // its virtual +54 (player) (the victim test 00277698..002776c8), the area
    // object 0025a2d0 and the blast 0021a200.
    std::function<bool(std::uint64_t object)> controller_player;
    std::function<void(const BulletAreaCall& call)> area;
    std::function<void(const BulletBlastCall& call)> blast;
};

struct BulletRecord {
    bool active_115{},alive_116{};
    float travelled_0{},speed_4{},remaining_8{};
    std::int32_t subtarget_c{-1},penetration_10{};
    ShotFrame frame_20{};                    // +20..+5c; row 3 (+50) is the position
    std::uint64_t target_60{};std::int32_t target_id_64{-1};
    std::uint64_t shooter_68{};std::int32_t shooter_id_6c{-1};
    std::uint64_t static_object_70{};std::int32_t static_object_id_74{-1};
    std::vector<std::size_t> links_80;       // +7c/+80: the other pellets of the shot (bullet indices)
    std::optional<BulletStaticHit> static_hit;   // +90..+100
    RootVector end_point_100{};
    bool rolled_114{},tracer_117{},aim_view_118{},statistics_119{};
    std::int32_t tracer_texture_11c{-1};         // *00236f20(0x1a), stored by 00277cf8..00277d2c
    std::uint8_t tracer_phase_140{};             // cBullet::sUvSets qword: 0,4,8,12
    std::int32_t item_148{};
};

class BulletPool {
public:
    static constexpr std::size_t capacity=50;
    // 00277e20: returns the bullet handle (index + 1) or 0 when the pool is full.
    std::uint64_t request(const ShotRequest& request,const WeaponDatabase& database,const BulletPoolHost& host,std::vector<BulletEvent>& events);
    // 00275750(a, b): b joins a's pellet list.
    void link(std::uint64_t a,std::uint64_t b);
    // 00275670(dt): 00276460 on every active record, in record order.
    void update(float dt,const WeaponDatabase& database,const BulletPoolHost& host,std::vector<BulletEvent>& events);
    const BulletRecord& record(std::uint64_t handle) const;
    const std::array<BulletRecord,capacity>& records() const{return records_;}
private:
    void update_one(std::size_t index,float dt,const WeaponDatabase& database,const BulletPoolHost& host,std::vector<BulletEvent>& events);
    std::array<BulletRecord,capacity> records_{};
    std::uint8_t next_tracer_phase_{};
};

// 0022ba30 for one bullet update: candidates ahead of the frame within
// `request.step_14`, nearest first; the designated target is skipped unless
// the hit roll succeeded; otherwise +40 = float(band damage) (unless +20) and
// the object's +104 runs; a hit stops the scan unless the penetration +24
// stays positive. Fills +30/+34/+38/+3c through the hit entry.
void scan_bullet_candidates(BulletHitRequest& request,std::size_t bullet,const BulletPoolHost& host,std::vector<BulletEvent>& events);

// 00276460's end-of-travel hold block (002774e0..00277924) for bullet
// `index` after its travel step (frame +50 = b.frame_20[3]): false when the
// update returns there (no victim and the end not reached); otherwise hold 9,
// or hold 8 with a victim or a resolving static object +70 / +74, run the
// statistic / area / blast / sound events and calls (BulletEvent), and true.
bool bullet_hold_block(const BulletRecord& b,std::size_t index,std::uint64_t victim,bool reached,const WeaponRecord& record,
    const BulletPoolHost& host,std::vector<BulletEvent>& events);

// Checkpoint 132uk6: 00276460's explosive branch (00277388..002774dc) for
// bullet `index` after its travel step, with a victim or the end reached at a
// resolving static object and a damage type other than 4: when record +c4 is
// not <= 0 (c.ole.s: a NaN runs it, a denormal counts as 0) the effect
// 00232460(+d8, frame) for +d8 != -1, the blast 0021a200(the shooter while it
// resolves, +64, 1, end + frame row 1 * -2.0, 0, 1, 1.0, +c4, +c8) and, for
// *00236f20(0x25) >= 0, the scorch decal 002797c0(*004ffc3c, the same point,
// that id, 0, 5.0). Returns whether it ran.
bool bullet_explosion(const BulletRecord& b,std::size_t index,const WeaponRecord& record,const BulletPoolHost& host,
    std::vector<BulletEvent>& events);

}
