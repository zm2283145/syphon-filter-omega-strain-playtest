#include "profile_awards.hpp"

#include "awards_agent_host.hpp"
#include "disc.hpp"
#include "weapon_database.hpp"

#include <stdexcept>
#include <utility>

namespace omega {

ProfileAwards load_profile_awards(DiscImage& disc) {
    ProfileAwards out;
    out.awards = parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
    out.pool = decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
    const auto db = WeaponDatabase::load(disc.read(disc.file({"GAMEDATA", "COMMON", "WEAPONS.WDB"})));
    for (const auto& r : db.records()) out.weapons.push_back(r.display_name_80());
    out.registries = build_award_registries(out.pool, out.weapons, read_awards_init(out.awards));
    out.tables = to_award_tables(out.registries, out.pool, out.weapons);
    out.parts = parse_parts_prn(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "PARTS.PRN"));
    return out;
}

AgentLoadResult load_profile(const ProfileAwards& awards, const AgentRefillHost& host, AgentSaveBuffer& buffer,
    std::span<std::uint8_t, agent_record_size> agent, std::optional<std::span<const std::uint8_t>> name,
    std::int32_t slot, AgentLoadState& state, const AgentCardState& card, const AgentLoadReader& read) {
    auto result = load_agent(buffer, agent, name, slot, state, card, read);
    if (result.result == 0 && result.refill) {
        refill_agent(agent, state, awards.tables, 0, 1, host);
        agent[0xb72] = 0;
        agent[0xb73] = 0;
        agent[0xb70] = 1;
    }
    return result;
}

AgentRefillHost profile_refill_host(const ProfileAwards& awards,
    std::function<std::uint32_t(std::size_t rank)> rank_value, std::vector<std::string>* missing_parts) {
    AgentRefillHost host;
    host.rank_value = std::move(rank_value);
    host.set_awards_and_bonuses = [&awards, missing_parts](std::span<std::uint8_t, agent_record_size> agent,
                                      AgentLoadState& state) {
        AwardsAgentContext context{agent, state, awards.registries, awards.tables, awards.pool, awards.parts, {}};
        run_set_awards_and_bonuses(awards.awards, context);
        if (missing_parts)
            missing_parts->insert(missing_parts->end(), context.missing_parts.begin(), context.missing_parts.end());
    };
    return host;
}

std::vector<std::int32_t> agent_loadout_weapons(const AwardRegistries& registries,
    std::span<const std::uint8_t, agent_record_size> agent) {
    std::vector<std::int32_t> out;
    for (const auto& row : registries.unlocks) {
        // sra (bit + 31 when negative) 5; the shipped rows count up from 6 (*004926e0).
        if (row.bit < 0) throw std::runtime_error("00332ac0: a negative unlock bit");
        const auto bit = static_cast<std::uint32_t>(row.bit);
        const std::size_t o = 0xb08 + 4 * (bit >> 5);
        if (o + 4 > agent.size()) throw std::runtime_error("00332ac0: an unlock bit past the agent record");
        const std::uint32_t word = static_cast<std::uint32_t>(agent[o]) | static_cast<std::uint32_t>(agent[o + 1]) << 8 |
                                   static_cast<std::uint32_t>(agent[o + 2]) << 16 |
                                   static_cast<std::uint32_t>(agent[o + 3]) << 24;
        if ((word >> (bit & 31)) & 1) out.push_back(row.weapon);   // 001c1080 push of +14
    }
    return out;
}

}  // namespace omega
