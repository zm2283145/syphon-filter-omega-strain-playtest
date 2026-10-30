#include "agent_network_receive.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {

void put_word(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
}

void start_level_from_network(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
                              const AwardTables& tables, const AgentRefillHost& host) {
    // 00335d50(agent, 0): the 13 level-stat words, including +80.
    std::fill(agent.begin() + 0x50, agent.begin() + 0x84, std::uint8_t{0});
    refill_agent(agent, state, tables, 1, 0, host);
    std::fill(agent.begin() + 0xa4, agent.begin() + 0xc4, std::uint8_t{0});
    // 00335d40 returns 9; shown unlock bits are a nine-word copy.
    std::copy(agent.begin() + 0xb08, agent.begin() + 0xb2c, agent.begin() + 0xb2c);
    agent[0xb70] = 1;
    agent[0xb71] = 0;
    std::copy(agent.begin() + 0xb50, agent.begin() + 0xb54, agent.begin() + 0xb54);
    put_word(agent, 0xd4, 0xbf800000U);
}

}  // namespace

void apply_agent_network_update(const AgentNetworkUpdate& update,
                                std::span<std::uint8_t, agent_record_size> agent,
                                AgentLoadState& state, const AwardTables& tables,
                                const AgentRefillHost& refill_host) {
    if (update.client >= 4) throw std::runtime_error("AgentData network receive: client outside four slots");
    if (update.stats_only) {
        // 00330e40: no Reset and no awards script on a statistics packet.
        for (std::size_t i = 0; i < update.stats_tail.size(); ++i)
            put_word(agent, 8 + (i + 14) * 4, update.stats_tail[i]);
        agent[0xb70] = 1;
        return;
    }
    // The full stream omits the level count. 003361e0 Reset sizes the
    // receiving vector from *00532af4, not from the sender's record.
    if (state.level_count < 0 || static_cast<std::size_t>(state.level_count) != tables.levels.size() ||
        update.level_words.size() != tables.levels.size())
        throw std::runtime_error("AgentData network receive: level count differs from receiving registry");
    if (!refill_host.set_awards_and_bonuses || (!tables.ranks.empty() && !refill_host.rank_value))
        throw std::runtime_error("AgentData network receive: AWARDS refill services are unavailable");
    if (update.name.size() >= 16 || update.name.find('\0') != std::string::npos)
        throw std::runtime_error("AgentData network receive: name does not fit the 16-byte field");

    reset_agent(agent, state);  // 003361e0, before the first read
    // 003314f0 clears the first byte of all three name scratch fields.
    agent[0x8ac] = agent[0x8bc] = agent[0x8cc] = 0;
    for (std::size_t i = 0; i < update.objective_bits.size(); ++i)
        put_word(agent, 0x84 + 4 * i, update.objective_bits[i]);
    for (std::size_t i = 0; i < update.statistics.size(); ++i)
        put_word(agent, 8 + 4 * i, update.statistics[i]);
    for (std::size_t i = 0; i < update.level_words.size(); ++i)
        put_word(state.level_records[i], 0, update.level_words[i]);
    std::copy(update.name.begin(), update.name.end(), agent.begin() + 0x8ac);
    agent[0x8ac + update.name.size()] = 0;
    put_word(agent, 0xd4, update.mission_time_bits);
    agent[0xb74] = update.completed != 0 ? 1 : 0;  // 003314f0 stores boolean

    if (update.awards) refill_agent(agent, state, tables, 0, 0, refill_host);
    else start_level_from_network(agent, state, tables, refill_host);
    agent[0xb70] = 1;
}

}  // namespace omega
