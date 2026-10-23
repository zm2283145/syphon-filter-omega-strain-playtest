#pragma once
// The actor side of a fire request (COMBAT.md §4): actor action 0x3b kind
// 0xb (001a92a0 -> 001aa000, payload +30..+50) and the actor fire 001960c0
// with its helpers: the held class 001437a0, the dry-fire step 00141300 and
// the can-attack test 00141240. Firearms reach the discharge 0013ff40 when the
// secondary (upper-body) component's two states both allow firing (table
// 0049f790, byte +4c). Hold classes 1 and 7 call the host's throw start
// (00197560 / 00197140, actor_throw.hpp); hold classes 3 / 4 / 10 / 11 call
// the host's melee attack (001414d0 then 00196480, actor_melee.hpp).
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace omega {

// The payload of action 0x3b kind 0xb: +0 the target name word (resolved by
// the host through 0016f7e0 / 0040c540; the "none" word *004bdd00 is no
// target), +4 subtarget, +8 aim-view flag, +10 aim point, +20 throw power.
struct ActorFireRequest {
    std::uint64_t target{};
    std::int32_t target_id{-1};
    std::int32_t subtarget{-1};
    bool aim_view{};
    RootVector point{};
    float power{};
};
// The actor fields 001960c0 reads.
struct ActorFireState {
    std::uint8_t life_state{};                        // virtual +108
    std::uint32_t flags_3210{},flags_3248{};
    bool byte_33e4{},byte_33e8{};
    // Secondary component +3314 (allocated for every activated actor) and its
    // states +310 / +311; nullopt without the component.
    std::optional<std::array<std::uint8_t,2>> secondary_states;
    bool agent_unlock_0{};                            // 00332f90(agent data, 0) (unarmed attack)
};

// 001437a0: the held item's hold class (record +f0, signed), 11 with nothing held.
std::int32_t weapon_held_class(const WeaponInventory& inventory,const WeaponDatabase& database);
// 00141240: by hold class (jump table 0049b1e0): 0, 8, 9 ready (00143400 without
// continuation); 1, 7 spare rounds > 0; 3, 4, 5, 10 always; 2, 6 and >= 11 never;
// nothing held: never.
bool weapon_can_attack(const WeaponInventory& inventory,const WeaponDatabase& database);
// 00141300: with a hold-class-0 weapon whose clip is empty, spare rounds send
// the reload action 0x3c (returns 1); otherwise a locally owned player gets
// the empty click (descriptor 004f2990 for item kind 2, 004f2b30 for 4, else
// 004f27f0).
bool weapon_dry_fire(const WeaponInventory& inventory,const WeaponDatabase& database,bool player,bool byte_14,
    std::vector<WeaponDischargeEvent>& events);
// 0049f790 byte +4c of a secondary state (boot image rows 0..24); a state
// outside the table throws.
bool secondary_state_allows_fire(std::uint8_t state);

enum class ActorFireResult {
    ignored,          // dying/dead, cannot attack, busy, or the secondary states refuse
    not_fired,        // the discharge ran and returned 0
    fired,            // the discharge returned 1
    throw_requested,  // hold class 1 / 7: 00197560 / 00197140 ran (the one-shot may still refuse)
    melee_requested,  // hold classes 3 / 4 / 10 / 11: 001414d0 then 00196480 ran (which may still refuse)
};
// 001960c0 (after 001aa000 built the request). The discharge host must carry
// the actor's player/AI/+14 answers.
ActorFireResult handle_actor_fire(WeaponInventory& inventory,const WeaponDatabase& database,const ActorFireRequest& request,
    const ActorFireState& state,const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events);

}
