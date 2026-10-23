#pragma once
// Agent unlock ("perk") leaves (research/AGENT_PERKS.md): the unlock test
// 00332f90 and the agent record accessor 001314f0. Awards set the unlock
// bits at agent + b08 (00335e50's award_unlock, agent_refill.hpp); bits 0..5
// are cAgentData::eSpecialBonusTypes (table 00492860), the gameplay perks.
#include "agent_refill.hpp"
#include "agent_save.hpp"
#include "spawned_actor_ctor.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace omega {

enum class AgentPerk : std::uint32_t {
    neck_snap = 0,          // unarmed takedown (001960c0)
    dual_wield = 1,         // weapon pickups (00251920, 002526f0, 00254da0) and loadout 002bd560
    subtargeting = 2,       // head / subtarget cycle (00247d60)
    increased_armor = 3,    // spawn armor x *0048a1c8 (001b5fe0)
    increased_health = 4,   // spawn health x *0048a1d0 (001b5fe0)
    increased_ammo = 5,     // clips x *0048a2d0 (001861f0)
};
inline constexpr std::uint32_t agent_perk_count = 6;     // 0045fab0's AB_SPECIAL_<i> loop
inline constexpr std::uint32_t agent_unlock_words = 0xb08;
inline constexpr std::uint32_t agent_records = 0x1c8;     // G + 1c8 .. + 1d4: four record pointers
inline constexpr std::uint32_t agent_active_slot = 0x1d8; // G + 1d8: the active record index
inline constexpr std::uint32_t agent_profiles = 0x4ffb50; // G

// 00332f90 on a record held natively: bit (index & 0xff) of the words at +b08.
bool agent_has_unlock(std::span<const std::uint8_t, agent_record_size> agent, std::uint32_t index);
inline bool agent_has_perk(std::span<const std::uint8_t, agent_record_size> agent, AgentPerk perk) {
    return agent_has_unlock(agent, static_cast<std::uint32_t>(perk));
}

// 00332ac0(agent, vec): for each AWARDS unlock row of 00532b10 in order whose
// bit (+4) is set in the agent's +b08 words, its weapon record index (+14)
// pushed (no duplicate check). Throws on a bit outside +b08..+b4f.
std::vector<std::int32_t> agent_unlocked_weapons(std::span<const std::uint8_t, agent_record_size> agent,
                                                 const AwardTables& tables);

// 00332f90(record, index) over EE memory: returns 0 / 1. The word read is
// record + b08 + 4 * ((index & 0xff) >> 5) (no null check; 001861f0 passes 0
// for a non-player and reads EE address 0xb08).
std::uint32_t run_agent_unlock_test(const SpawnedActorMemory& memory, std::uint32_t record, std::uint32_t index);
// 001314f0(G, slot): slot -1 is *(G + 1d8); returns *(G + 1c8 + 4 * slot),
// unchecked (32-bit address wrap).
std::uint32_t run_agent_record_accessor(const SpawnedActorMemory& memory, std::uint32_t profiles, std::uint32_t slot);

} // namespace omega
