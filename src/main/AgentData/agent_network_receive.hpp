#pragma once

#include "agent_network_codec.hpp"
#include "agent_refill.hpp"

namespace omega {

// cNetAgentUpdateMsg_NetReceive (0012f0d0) has already selected the remote
// AgentData slot and decoded its cNetMsg body when this is called. The caller
// owns one agent record and AgentLoadState per client slot, plus the real
// AWARDS host used by 00335e50. This does not select slots or send messages.
//
// Full updates reproduce 003314f0: Reset, field reads, then either
// SetBonusBits(agent,0,0) for awards!=0 (the level-end send) or
// StartLevel(agent,0) for awards==0, and finally +b70=1. Stats-only updates
// reproduce 00330e40: statistics 14..30 and +b70=1, with no reset/refill.
void apply_agent_network_update(const AgentNetworkUpdate& update,
                                std::span<std::uint8_t, agent_record_size> agent,
                                AgentLoadState& state, const AwardTables& tables,
                                const AgentRefillHost& refill_host);

}  // namespace omega
