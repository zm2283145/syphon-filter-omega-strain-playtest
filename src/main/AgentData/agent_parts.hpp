#pragma once
// The level end's parts list (research/LEVEL_END.md "The parts tail"):
// 00335830's tail builds a local vector (002888c0: {+0 capacity, +4 count,
// +8 data}, zeroed), fills it with 00332500(agent, list), hands it to the
// appearance patch 00405a60(*(agent + 9a4), list) and frees its data
// (002887c0(list, -1) -> 0036b130). The list holds parts-table row pointers
// (*00532b38 + 0x14 * row); natively it holds the row indices, which compare
// equal exactly when the pointers do.
#include "agent_load.hpp"
#include "agent_refill.hpp"
#include "parts_table.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace omega {

// 003301e0(list, score): count = 0, then every row i < *00532b34 (re-read
// each pass) with score < its value (signed slt) is appended (00330280).
std::vector<std::uint32_t> parts_above_score(std::span<const PartRow> parts, std::int32_t score);

// 00332500(agent, list) after its score: 003301e0(list, score), then for each
// owned part s0 < (s32)+b5c (agent + b58's vector, state.parts) the list is
// scanned t0 = 0 .. a3 = count - 1 (signed): a match takes the last element
// (list[t0] = list[a3], a3 - 1) and t0 moves on without rechecking it; the
// list is resized to a3 + 1 (001bac40). Then +b6c < 0 takes the count, else a
// count below +b6c sets byte +b68 = 1 and +b6c = count. Returns the list.
std::vector<std::uint32_t> locked_award_parts(std::span<std::uint8_t, agent_record_size> agent,
    const AgentLoadState& state, std::span<const PartRow> parts, std::int32_t score);

// 00332500 with its score 003305d0 (award_score).
std::vector<std::uint32_t> locked_award_parts(std::span<std::uint8_t, agent_record_size> agent,
    const AgentLoadState& state, const AwardTables& tables, std::span<const PartRow> parts);

}  // namespace omega
