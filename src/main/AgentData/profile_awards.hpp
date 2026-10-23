#pragma once
// The disc data behind a profile's award refill (research/FRONTEND.md §6) and
// the refill host that runs awards::SetAwardsAndBonuses natively, so the
// profile create 00333f60 and the post-load refill 00335e50(agent, 0, 1) run
// with the shipped AWARDS.SO instead of a host hook.
//
// The EE builds these tables once at boot: the AWARDS registrations (read
// from the script's Init calls), the weapon database's record names, the
// common string pool and the PARTS.PRN parts table 00532b30.
#include "agent_refill.hpp"
#include "award_registry.hpp"
#include "parts_table.hpp"
#include "script.hpp"
#include "string_pool.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace omega {

class DiscImage;

struct ProfileAwards {
    Script awards;                      // GAMEDATA/COMMON/SCRIPTS.HOG AWARDS.SO
    StringPool pool;                    // GAMEDATA/COMMON/STRINGS.DAT
    std::vector<std::string> weapons;   // WEAPONS.WDB record names (+80), by id
    AwardRegistries registries;         // 00532a70..00532bb0
    AwardTables tables;                 // the same registries by name
    std::vector<PartRow> parts;         // 00532b30
};

ProfileAwards load_profile_awards(DiscImage& disc);

// 00334790(agent, name, slot) with its refill: load_agent, then for version
// >= 3 on success 00335e50(agent, 0, 1) through `host`, after which the EE
// stores +b72 = +b73 = 0 and +b70 = 1 (load_agent stores them before
// returning; the refill's UnlockZeus / LockZeus set +b72, so they are stored
// again). result.refill reports whether the refill ran.
AgentLoadResult load_profile(const ProfileAwards& awards, const AgentRefillHost& host, AgentSaveBuffer& buffer,
    std::span<std::uint8_t, agent_record_size> agent, std::optional<std::span<const std::uint8_t>> name,
    std::int32_t slot, AgentLoadState& state, const AgentCardState& card, const AgentLoadReader& read);

// An AgentRefillHost whose set_awards_and_bonuses is 003e1b60(0x555070,
// "awards::SetAwardsAndBonuses", 1, agent) through the VM over `awards`.
// rank_value is what +b50 holds for rank row k: the EE stores the row's
// address (*(00532b50 + 8) + 0x24 k), so the caller supplies it. Part names
// the table lacks are appended to missing_parts when it is given. `awards`
// must outlive the host.
AgentRefillHost profile_refill_host(const ProfileAwards& awards,
    std::function<std::uint32_t(std::size_t rank)> rank_value, std::vector<std::string>* missing_parts = nullptr);

// 00332ac0(agent, vec): the weapons the agent may pick. For each unlock row of
// 00532b10 (stride 0x18) whose bit (+4) is set in the agent's words at +b08,
// the row's record index (+14) is pushed, in registry order. The loadout
// screen's lists (002bf090) take these when *0051ee38 is 0 and keep the
// records whose class (+30) is the list's category point.
std::vector<std::int32_t> agent_loadout_weapons(const AwardRegistries& registries,
    std::span<const std::uint8_t, agent_record_size> agent);

}  // namespace omega
