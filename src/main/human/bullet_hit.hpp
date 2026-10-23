#pragma once
// A bullet reaching an actor (COMBAT.md §4): the range-band damage of the
// candidate scan 0022ba30 (0022be78..0022bf70, also used by the bullet update
// 00276460 for a static hit) and the actor's bullet hit entry 00188780
// (actor virtual +104), which picks the hit location and calls the damage
// virtual +100 (00187360, apply_actor_damage). The hit-volume query 00188140
// (model hit volumes 003a9d40 / 0038b990 / 0038c050) is not resolved: it is a
// host function, and a host without it throws when a bullet reaches it.
#include "animation_root.hpp"
#include "weapon_database.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// d = travelled + candidate distance. d <= near: damage +68; near < d <= mid:
// trunc(+68 + (+64 - +68)(d - near)/(mid - near)); d > mid:
// trunc(+68 + (+6c - +64)(d - mid)/(far - mid)) (based on +68, not +64, and
// not clamped beyond far). Equal band limits (+3c == +38, d > mid) divide by
// zero: div.s 0022bf44 gives +-FLT_MAX and cvt.w.s 0022bf58 saturates to
// 0x7fffffff / 0x80000000 (Checkpoint 132uk4, tests/band_damage_zero_goldens.hpp).
std::int32_t weapon_band_damage(const WeaponRecord& record,float distance);
// 00188780's amount: cvt.w.s 001887fc of the request's +40 (saturating).
std::int32_t bullet_hit_amount(float damage_40);

// The request the bullet update builds for 0022ba30 (its stack +b50) and that
// 00188780 reads and writes.
struct BulletHitRequest {
    std::uint64_t shooter_0{};                // weak handle resolved (0: gone)
    std::array<RootVector,4> frame_4{};       // the bullet frame: row 1 forward, row 3 origin
    std::uint64_t designated_8{};             // designated target (hit roll)
    bool rolled_c{};                          // the hit roll succeeded
    bool aim_view_d{};
    std::int32_t subtarget_10{-1};
    float step_14{};                          // segment length of this update
    const WeaponRecord* record_18{};
    float travelled_1c{};
    bool skip_damage_20{};
    std::int32_t penetration_24{};
    float volume_28{0.2F};                    // copied to the hit-volume record (+4c)
    bool statistics_2c{true};
    bool bullet_type_2d{};                    // record +bc == 1
    std::int32_t category_30{};
    float distance_34{};
    std::uint64_t victim_38{};
    std::int32_t node_3c{};
    float damage_40{};
};

struct BulletHitVolumes {
    std::uint32_t flags{};                    // 0x100 hit, 0x40 head, 0x80 armored torso, 0x20 part 0x14
    float distance{};                         // record +24
    std::vector<std::int8_t> nodes;           // record +40.. (count +48), hit node codes
};
struct BulletHitShooter {
    std::uint64_t object{};
    bool owned_14{};
    // aim target: 003ea550 (+28/+2c resolved). port_agent_slot: see BulletHitVictim.
    struct Controller {bool player_54{};std::uint64_t aim_target{};bool port_agent_slot{};};
    std::optional<Controller> controller;     // +58
};
// Checkpoint 132uk6: a shooter that is not an actor (the tank's fire 00286690), as 00188780 and the damage
// virtual 00187360 read it: its +14 (owned_14), a null +58 (no controller: BulletHitShooter::controller
// nullopt) and its virtual +e4 (read by 00187360 for type 2 and by the kill for types 1 / 2 / 0xd).
struct BulletObjectShooter {
    bool owned_14{};
    RootVector position_e4{};
};
struct BulletHitVictim {
    std::uint64_t object{};
    std::optional<bool> controller_player_54; // +58 (nullopt: none)
    std::int32_t armor_33a0{};
    bool invulnerable_33a4{};
    // Port-only (checkpoint 132ob7, --coop-partner): the actor holds a port agent slot 1..3 (coop_partner.hpp
    // AgentSlots), the place a second player takes. 00188780's player-to-player test (both controllers answer
    // +54) counts it as a player, and only there, so the CPU partner and the player do each other 0 damage as
    // two players do; everything else reads the AI controller.
    bool port_agent_slot{};
};
struct BulletHitEvent {
    enum class Kind {
        hit_test,       // 00188140(victim, origin, direction, mode, record)
        statistic,      // 00333f10(stats, id, 1): 10 hit, 12 aim-view hit, 6 type-7 hit outside the head
        damage,         // victim +100(victim, shooter, category, type, &request+24, 0, null, amount)
    } kind{};
    std::int32_t value{};                     // mode / statistic id / category
    std::uint8_t type{};
    std::int32_t penetration{};               // damage: request +24 before the call
    float amount{};
    bool operator==(const BulletHitEvent&) const=default;
};
struct BulletHitHost {
    std::function<std::uint32_t()> random;    // 00127dd0
    std::function<BulletHitVolumes(const RootVector& origin,const RootVector& direction,std::int32_t mode,float volume)> hit_volumes;
    std::function<std::uint8_t()> life_state; // victim virtual +108
    // Victim virtual +100 (00187360 for actors); it may change the penetration.
    std::function<void(std::uint8_t category,std::uint8_t type,std::int32_t& penetration,float amount)> damage;
};
// 00188780: returns the category (0 miss, 1 body, 2 head, 3 armored torso, 4 part 0x14).
std::uint8_t apply_bullet_hit(const BulletHitVictim& victim,const std::optional<BulletHitShooter>& shooter,BulletHitRequest& request,
    const BulletHitHost& host,std::vector<BulletHitEvent>& events);

// The actor virtuals the aim and the discharge read (static readings):
// +c8 = 00188e60 stance from +3204 (bit 5: 2; bits 1, 7 or 6: 1; bit 3 or 4
// with bit 0: 4; else 0) and +cc = 00188d30 the target size factor of a
// subtarget (0: 0.3 (0048a2a8), 1 and 2: 0.5 (0048a2b0), 3: 1.0).
std::uint8_t actor_stance_c8(std::uint32_t flags_3204);
float actor_target_size_cc(std::int32_t subtarget);

}
