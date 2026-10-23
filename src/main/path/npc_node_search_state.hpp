#pragma once
// The node-search part of a route manager request (0x610 bytes; kind +c = 2,
// NPC_COMBAT.md section 5.4, milestone N5c): offsets +10..+604 of the request,
// also the layout of the global search object 004ed9f0 the route search's
// states 0-2 use. Functions: npc_node_search.hpp. The fields start zeroed
// (as the interpreter harness clears the requests; 001644b0 writes only +0
// and +4); every one a search reads is written before it is read.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace omega {
// A candidate slot (+30 + 0x20 k, k < 32): +c score, +10 node, +14 second
// node, +18 / +1c the LOS answers of the node / the second node (-1 unknown,
// 0 blocked, 1 clear).
struct NpcSearchCandidate {
    float score_0c{};
    std::int32_t node_10{},second_14{},clear_18{},second_clear_1c{};
};
// A region table entry (+484 + 12 k): the region, -1, and 0.0 (0015e830
// computes a distance it does not store).
struct NpcSearchRegion {
    std::int32_t region_484{},parent_488{};
    float distance_48c{};
};
inline constexpr std::size_t npc_search_candidate_slots=32,npc_search_region_slots=32;
struct NpcNodeSearch {
    // +10: the candidate list (sorted by score, slot indices in order; its
    // count is +2c), the slots +30.
    std::vector<std::size_t> list;
    std::array<NpcSearchCandidate,npc_search_candidate_slots> slots{};
    float query_430{},query_434{};             // 0015e0f0: the distance band of the target
    std::uint8_t query_438{};                  // ... and the second-node byte
    RootVector self_440{},target_450{};
    float radius_460{};
    std::int32_t start_region_464{};           // 001612d0
    std::uint32_t mask_468{};                  // node types (ANDed with node +64)
    std::int8_t kind_46c{};                    // 0: the best at once; 1: node clear, second blocked; 2: node blocked, second clear
    std::int32_t exclude_470{};
    std::uint8_t found_474{};                  // 2: an answer is ready for the next step
    std::int32_t node_478{},second_47c{};
    std::uint8_t checking_second_480{},byte_481{},route_482{};
    std::array<NpcSearchRegion,npc_search_region_slots> regions{};
    std::int32_t region_count_604{};
};
}
