#pragma once
// PORT-ONLY (checkpoint 132pw1): the CPU co-op partner's weapons (research/COOP.md "The partner's weapons").
//
// What the original does for a second agent (static reading of SCUS_972.64; COOP.md has the addresses):
//   * Loadout. Every agent chooses its own kit on its own machine: the level-start screen GuiEquipmentModify lists
//     the weapons its own agent record unlocks (002bf090 -> 00332ac0 over 001314f0(004ffb50, -1), the local agent),
//     and its Accept 002bcb50 applies them to the screen's player +48 (the local player) before the multiplayer
//     wait screen GuiEquipmentSetup (00418a80 with 005721c8, 002bcd30). The partner has no agent record of its own;
//     its pool is the player's (the agreed design), chosen here instead of the role-2 record's item words.
//   * Pickups. Backpacks (dead drops 00257640, INV: crates 00257f30) are world objects shared by every agent.
//     Their receiver 00254600 serves only a picker whose controller answers virtual +54 (a player component: the
//     AI component's answers 0, so an original NPC never picks anything up) and whose index (+68) is the
//     backpack's authority +308 (00258330 resets it to 0, the host) or a backpack with +2f3 set; any other agent
//     asks the authority with the request 005061b0 (vtable 004dcef0, 003fb020(id, -1, +308)) while 005721c8 is
//     set. The walk-over (the zone enter 001774c8 -> +1bc -> 001762c0 -> PickupWpn 00254da0) fills the agent's
//     empty slots and merges ammunition into weapons it holds (00255290); the Triangle menu's Cross 00251920 swaps
//     the agent's weapon for the backpack's. Crates send CrateInteractMsg (004f7ea0) with Who = the taker.
//     Mission items (kind 5, +3528) move only through the menu's kind-5 take 002523b0 (InventoryMsg 004f7ee0).
//   * Weapon choice. An AI component keeps two weapon preferences, +b0 (SetWeaponPreference 001498b0 -> 0014b740,
//     enum 0) and +b4 (1), and a switch distance +b8 (SetWeaponPreferenceDistance 00149860 -> 001498a0); with
//     +b8 > 0 and a target (perception +20 bit 4) the AI modes switch to +b4 beyond +b8 - 10 and back to +b0
//     nearer than +b8 (00156d00's posture, AttackFromCover 00157228 / 001572f8, DrawWeapon 0014bae0).
// The rules below choose; every inventory change runs the original paths (AddWeapon 0014b890 / RemoveWeapon
// 0014b790 / DrawWeapon 0014bae0 for the loadout, 00254600 / 00254da0 / 00251920 for the pickups, the component's
// +b0 / +b4 / +b8 for the preferences).
#include "animation_root.hpp"
#include "play_loadout.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// partner_loadout=auto (default) / copy / record. record keeps the role-2 record's item words (001b6678..001b6724,
// the kit before 132pw1; the co-op checks recorded with it replay with it).
enum class PartnerLoadoutMode : std::uint8_t { automatic, copy, record };
std::optional<PartnerLoadoutMode> parse_partner_loadout_mode(std::string_view text);
const char* partner_loadout_mode_name(PartnerLoadoutMode mode);

// 00142b50's weapon rating: record +64 x 1.0 (the AI's own "how good is this weapon", mode 1 00156d00).
std::int32_t partner_weapon_rating(const WeaponRecord& record);
// The loadout screen's two rating columns multiplied: damage +64 (002c8fd0) x fire rate +60 (002c9020), the rate
// capped by the clip +70 (no more shots between two reloads).
std::int32_t partner_loadout_score(const WeaponRecord& record);
// A firearm the AI fires near the player without harm to the team: hold class 0 (+f0), item kind 1..4 (+48: pistol,
// rifle, SMG, shotgun; not the launchers 5, heavy 6, melee 7, tasers 8, thrown 9), no blast (+c4 <= 0), no launched
// projectile (+d4 == 0), a bullet / pellet damage type (+bc 0, 1, 13), a positive rating, and not one of the WDB's
// template rows named "base" (94..102: they have no string of their own; Unlock All lists them).
bool partner_usable_firearm(const WeaponRecord& record);
// A melee weapon (+30 class 9, hold class 3 or 4: knives, batons).
bool partner_melee(const WeaponRecord& record);
// An item the partner must not be handed: a blast (+c4 > 0), a fire / gas / explosive damage type (+bc 2, 4, 5, 6),
// a launcher or heavy weapon (kind 5, 6), or a gadget (class 6, 7: goggles, flashlight).
bool partner_unsafe_item(const WeaponRecord& record);

// auto: from `unlocked` (00332ac0's list for the player's agent record, the loadout screen's pool) the best usable
// firearm of each firearm category (back 2, hip 3, leg 5) by partner_loadout_score (ties: rating, then the lower
// ID), no grenade, the best melee by rating, and in the hand the best-scoring firearm.
EquipmentLoadoutSelection choose_partner_loadout(const WeaponDatabase& database,const std::vector<std::int32_t>& unlocked);
// The selection's items in category order (back, hip, leg, grenade, melee; None skipped) and the hand's item.
std::vector<std::int32_t> partner_loadout_items(const EquipmentLoadoutSelection& selection);
std::optional<std::int32_t> partner_loadout_hand(const EquipmentLoadoutSelection& selection);
std::string describe_partner_loadout(const WeaponDatabase& database,const EquipmentLoadoutSelection& selection);

// The weapons in slots 0..5 the partner can fire now (usable firearm, clip + spare > 0).
std::vector<std::int32_t> partner_armed_items(const WeaponDatabase& database,const WeaponInventory& inventory);
// The damage per second a weapon does at `distance` (weapon_band_damage, the bullet hit's own band, x the fire rate
// +a4); 0 beyond its far range +3c.
float partner_damage_rate(const WeaponRecord& record,float distance);
// Port-only choice of the component's preferences: +b0 the best weapon at close range (partner_near_distance),
// +b4 the best at long range (partner_far_distance) and +b8 the first distance (1.0 steps) at which +b4 is the
// better; when one weapon is the best at every distance, +b4 is the next best and +b8 = 0 (no switching).
inline constexpr float partner_near_distance=5.0F;
inline constexpr float partner_far_distance=60.0F;
struct PartnerWeaponPreference {
    std::int32_t primary_b0{-1},secondary_b4{-1};
    float distance_b8{};
    bool operator==(const PartnerWeaponPreference&) const=default;
};
PartnerWeaponPreference partner_weapon_preference(const WeaponDatabase& database,const WeaponInventory& inventory);
// Out of ammunition: the held slot (+84) holds a firearm whose clip and spare are both 0; the best other armed
// weapon by partner_loadout_score (nullopt: none, or the held weapon still has rounds).
std::optional<std::int32_t> partner_out_of_ammo_switch(const WeaponDatabase& database,const WeaponInventory& inventory);

// A backpack as the partner's choice reads it: each entry (00255500 per item of the backpack's slots, the item +20
// and its rounds), the armour +2e8 (with +2e4), +2f0 (enabled) and +2f3 (StealPlayerWeapons' stash).
struct PartnerBackpackEntry {
    std::int32_t item{-1};
    std::int32_t rounds{};   // 00255500's total (spare + clip of every pair)
    bool listed{};           // 002553e0 lists it for the partner (the walk-over's candidates)
};
struct PartnerBackpackView {
    std::vector<PartnerBackpackEntry> entries;
    std::int32_t armor{};    // +2e4 ? +2e8 : 0
    bool enabled_2f0{true};  // +2f0: the walk-over takes anything (0 for an INV: crate)
    bool stash_2f3{};
};
// An agent's view: its inventory, its armour and the rounds one item holds at most (clip +70 x 001861f0(clips +74)).
struct PartnerAgentView {
    const WeaponInventory* inventory{};
    std::int32_t armor{},max_armor{};
    std::function<std::int32_t(std::int32_t item)> capacity;
};
enum class PartnerPickupKind : std::uint8_t { none, walk_over, cross };
struct PartnerPickupChoice {
    PartnerPickupKind kind{PartnerPickupKind::none};
    std::int32_t item{-1};     // cross: the backpack's entry the menu's Cross takes
    std::int32_t gain{};       // cross: the rating gain (a weapon for an empty slot: its rating; rounds: 1); walk_over: 1
    std::string reason;
};
// The partner's decision at one backpack (port-only rules over the native steps it would run):
//   * none for a StealPlayerWeapons stash (+2f3: the player's own weapons);
//   * with +2f0 set (a dead drop) the walk-over (00254da0) adds each listed item whose slot the partner has empty and
//     merges the rounds of each listed item it holds (00255290): an unsafe item vetoes the backpack, and so does any
//     item whose rounds the player needs (the player holds it with fewer rounds than its capacity: the player has
//     priority); armour (taken when the backpack's is higher) is left to the player when the player's is lower. A
//     crate's +2f0 is 0 (00257f30): its walk-over takes nothing;
//   * cross (the menu's Cross 00251920 on one entry) for the best of: a usable firearm rated above (00142b50) the
//     partner's weapon in its slot (the gain), a usable firearm or melee weapon for an empty slot the walk-over does not
//     fill (its rating), rounds for a held firearm below its capacity from a crate (1); never an entry whose rounds the
//     player needs;
//   * else walk_over when the walk-over gains anything (a usable firearm or melee into an empty slot, rounds for a
//     weapon below its capacity, better armour).
PartnerPickupChoice partner_pickup_choice(const WeaponDatabase& database,const PartnerBackpackView& backpack,
                                          const PartnerAgentView& partner,const PartnerAgentView& player);

// The partner's trips to backpacks (the play's driver; the follow driver yields while one is on, and any combat,
// co-op box, partner command or ladder ends it). Distances are horizontal, heights within max_dz.
struct CoopPickupConfig {
    std::uint32_t scan_interval{30};    // frames between looks at the backpacks
    float reach{12.0F};                 // a backpack within this of the partner
    float leash{16.0F};                 // and of the player (the partner stays with the player)
    float max_dz{2.5F};
    // The walk-over runs within this of the backpack: the route manager walks no last segment under 2.0 (00160720,
    // coop_route_min_segment), so a route ends at most that far from its point (a second agent would step into the box).
    float take_distance{2.0F};
    std::uint32_t give_up_frames{450};  // a trip that has not arrived by then is dropped
    std::uint32_t retry_frames{1800};   // a dropped or visited backpack is not chosen again for this long
};
struct CoopPickupCandidate {
    std::size_t backpack{};
    RootVector position{};
    PartnerPickupChoice choice;
};
struct CoopPickupState {
    std::optional<std::size_t> backpack;    // the trip's backpack (index into the session's backpacks)
    std::optional<std::int32_t> swap;       // the Cross's entry at arrival (none: the walk-over alone)
    RootVector goal{};
    std::uint32_t started{},next_scan{},phase_at{};
    std::uint8_t phase{};                   // 0 MoveToNode to the node nearest the goal, 1 its route, 2 the route beside it
    std::vector<RootVector> points;         // the route points beside the backpack still to try (routable, nearest first)
    std::size_t next_point{};
    bool route_ended{};                     // the route beside it ended (path event 6) within reach
    std::vector<std::pair<std::size_t,std::uint32_t>> skipped;   // backpack, the frame its skip ends
    std::uint32_t trips{},taken{};
};
// The candidate (index into `candidates`) to go to: a choice other than none, not skipped, within reach of the
// partner and leash of the player; Crosses first (largest gain), then the nearest to the partner.
std::optional<std::size_t> coop_partner_pickup_pick(const std::vector<CoopPickupCandidate>& candidates,const CoopPickupState& state,
                                                    const CoopPickupConfig& config,const RootVector& partner,
                                                    const RootVector& player,std::uint32_t frame);
bool coop_partner_pickup_arrived(const CoopPickupConfig& config,const RootVector& goal,const RootVector& partner);
// The route points beside a backpack: a crate's own collision blocks the route search's line to its centre (path
// event 3) and a crate by a wall often lies outside every navigation region, so the partner is sent beside it: 8
// directions at 0.7, 1.2, 1.8, 2.5 and 3.2, ring by ring, each ring nearest the partner first (the caller keeps the
// routable ones).
std::vector<RootVector> coop_partner_pickup_points(const RootVector& backpack,const RootVector& partner);
// Starts a trip (phase 0) / ends it, the backpack skipped until frame + retry_frames.
void coop_partner_pickup_start(CoopPickupState& state,const CoopPickupCandidate& candidate,std::uint32_t frame);
void coop_partner_pickup_end(CoopPickupState& state,const CoopPickupConfig& config,std::uint32_t frame);

}
