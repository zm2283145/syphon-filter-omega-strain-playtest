#pragma once

#include "agent_load.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega {

// The bytes written by cNetAgentUpdateMsg::Pack (00138820) after the generic
// cNetMsg envelope's 12-byte header. The outer category-2 packet is separate.
// A full update is the 0012eee0(G, 0, 1) level-end send.
struct AgentNetworkUpdate {
    std::uint8_t client{};
    std::uint8_t stats_only{};
    std::uint8_t awards{};
    std::array<std::uint32_t, 8> objective_bits{};      // agent +84..+a3; full updates
    std::array<std::uint32_t, 31> statistics{};          // agent +8..+83; full updates
    std::vector<std::uint32_t> level_words;             // first word of each +c8 level record
    std::string name;                                   // agent +8ac, at most 15 bytes plus NUL
    std::uint32_t mission_time_bits{};                  // agent +d4, verbatim float bits
    std::uint8_t completed{};                           // agent +b74
    std::array<std::uint32_t, 17> stats_tail{};          // stats-only: statistics 14..30
};

// Pack the original inner stream. Requires a NUL in the agent's 16-byte name
// and a result of at most 256 bytes, the generic cNetMsg payload capacity.
std::vector<std::uint8_t> encode_agent_network_update(
    std::uint8_t client, std::uint8_t stats_only, std::uint8_t awards,
    std::span<const std::uint8_t, agent_record_size> agent, const AgentLoadState& state);

// The full stream contains no level count. The caller must supply the number
// of level records from its receiving AgentData (+c8). This only decodes; the
// original 003314f0 also Reset/StartLevel or SetBonusBits before setting +b70.
// Those mutations belong to a separate, explicitly hosted receive step.
AgentNetworkUpdate decode_agent_network_update(std::span<const std::uint8_t> bytes,
                                               std::size_t expected_level_records);

}  // namespace omega
