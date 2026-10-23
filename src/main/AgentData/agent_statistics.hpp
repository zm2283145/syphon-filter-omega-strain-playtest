#pragma once
// The statistics of the local player's agent record ("AgentData.cc", the
// record 001314f0(004ffb50, -1) returns: 004ffb50 +1c8 indexed by +1d8;
// research/COMBAT.md "Checkpoint 55"), interpreter-verified
// (tests/agent_statistics_goldens.hpp):
//   00333f10  add: index & 0xff below 0x1f and amount > 0: word +8 + 4i
//             += amount, and for i < 0xd also the level copy +50 + 4i (that
//             is statistic i + 0x12);
//   00333ee0  read: word +8 + 4i for i < 0x1f, else 0;
//   00333df0  the level reset (script natives ResetLevel 0012f6e0 and
//             ResetStats 0012f790): statistics 0..0xc lose their level copy,
//             the copies 0x12..0x1e and the words +a4..+c0 become 0.
// The constructor 00336700 zeroes +84..+c0 and calls 003361e0, which zeroes
// statistics 0..0x1e (+8..+80) (static), so a new game's record is all zero. Statistic 0x19 is the level copy of statistic 7, which only the kill
// statistics 00186f30 add (at 00186fe4, when the local player kills an actor
// that is not its enemy): the NPC death notice reads it to pick the warning
// line (1: 0xc, 2: 0xe, 3: 0xf, 4: 0x11, else 0x10). It is not a difficulty
// setting.
#include <array>
#include <cstdint>

namespace omega {

struct AgentStatistics {
    std::array<std::int32_t,31> statistics{};   // +8 .. +80 (0x12..0x1e: level copies of 0..0xc)
    std::array<std::int32_t,8> words_84{};       // +84 .. +a0 (not touched by these functions)
    std::array<std::int32_t,8> words_a4{};       // +a4 .. +c0 (zeroed by the level reset)
    bool operator==(const AgentStatistics&) const=default;
};
inline constexpr std::uint8_t agent_statistic_friendly_kills=7;
inline constexpr std::uint8_t agent_statistic_level_friendly_kills=0x19;

void add_agent_statistic(AgentStatistics& stats,std::uint32_t index,std::int32_t amount);   // 00333f10
std::int32_t read_agent_statistic(const AgentStatistics& stats,std::uint32_t index);        // 00333ee0
void reset_agent_level_statistics(AgentStatistics& stats);                                  // 00333df0

}
