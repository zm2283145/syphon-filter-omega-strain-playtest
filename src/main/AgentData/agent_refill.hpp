#pragma once
// Agent award refill, profile create and name setter (AgentData,
// research/FRONTEND.md §6). 00335e50 rebuilds the per-level records and the
// unlock bits +b08 from the AWARDS registries; 00333f60 creates a profile
// (reset, refill, slot, optional save); 003340b0 sets the name.
//
// The registries are interned strings in the EE (lowercase, compared through
// 16-byte pool rows). Here rows carry their names; every compare lowercases
// (0012b270 maps only A..Z). Names must be 1..15 ASCII characters: 16 or more
// overrun the 16-byte key / pool row in the EE, so they throw. Lookups compare
// pool ids, so every name in AwardTables::unpooled is the same key (-1).
#include "agent_load.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace omega {

inline constexpr std::size_t agent_unlock_bits = 9 * 32;  // +b08 .. +b2b, the words the refill zeroes

struct AwardLevelRow {  // 00532af0 (stride 0x18) and its pair 00532b90 (8 bytes)
    std::string name;
    std::int32_t pair_first = 0;   // -> level record +8
    std::int32_t pair_second = 0;  // -> level record +c
};
struct AwardObjectiveRow {  // 005329f0, stride 0x18
    std::string name;
    std::int32_t bit = 0;     // +4: bit of agent + 84
    std::int32_t points = 0;  // +14
};
struct AwardRegistryRow {  // 00532a70 + 0x10 L, stride 0x18
    std::string name;
    std::int32_t unlock_id = -1;  // +10
    std::int32_t points = 0;      // +14
};
struct AwardRatingRow {  // 00532ad0, stride 0x18
    std::int32_t points = 0;     // +4
    std::uint8_t stat = 0;       // +8: the word agent + 8 + 4 stat
    std::int32_t threshold = 0;  // +10
    std::int32_t unlock_id = -1; // +14
};
struct AwardRankRow {  // 00532b50, stride 0x24
    std::int32_t threshold = 0;   // +8
    std::int32_t unlock_id = -1;  // +c
};
struct AwardUnlockRow {  // 00532b10, stride 0x18
    std::string name;
    std::int32_t bit = 0;  // +4: bit of agent + b08
    std::int32_t weapon = -1;  // +14: the weapon record index (00332ac0's list)
};

struct AwardTables {
    std::vector<AwardLevelRow> levels;  // *00532af4 rows: the same count the reset sizes the level vector to
    std::vector<AwardObjectiveRow> objectives;
    std::array<std::vector<AwardRegistryRow>, agent_award_lists> awards;
    std::vector<AwardRatingRow> ratings;
    std::vector<AwardRankRow> ranks;
    std::vector<AwardUnlockRow> unlocks;
    std::vector<std::string> weapons;  // the weapon database's record names (record +80), by id
    // Names absent from the string pool. They intern to -1, and the rows store
    // -1 for them, so an unpooled weapon name matches the first unlock row
    // whose name is unpooled too (in the shipped tables row 25, bit 31).
    // Level and objective names must be pooled (the EE reads their text
    // through the id).
    std::vector<std::string> unpooled;
};

struct AgentRefillHost {
    // 003e1b60(0x555070, "awards::SetAwardsAndBonuses", 1, agent), called before
    // each of the two award passes. Required (the script is not hosted here).
    std::function<void(std::span<std::uint8_t, agent_record_size>, AgentLoadState&)> set_awards_and_bonuses;
    // What +b50 holds for rank row k (the EE stores the row's address).
    std::function<std::uint32_t(std::size_t rank)> rank_value;
};

struct AgentRefillResult {
    std::optional<std::size_t> rank;  // the last rank row whose threshold the score reached (its value went to +b50)
};

// 00335e50(agent, mark, recount). Both flags are tested as words. Visits level
// records 0 .. (s32)+c8 - 2 (the last level is skipped), zeroes +b08..+b2b and
// +b5c (and state.parts), then twice: the script, the award pass. Leaves +b50 alone when no rank
// row is reached.
AgentRefillResult refill_agent(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
    const AwardTables& tables, std::uint32_t mark, std::uint32_t recount, const AgentRefillHost& host);

// 00333890(this, pattern, s, '*'): the prefix up to the pattern's first '*'
// and the suffix after its last '*', case-insensitively.
bool award_wildcard(std::string_view pattern, std::string_view s);

// 003352c0(agent, id): id < 0 nothing; id < 10000 the weapon record's unlock
// row bit (00147840 / 00333050); else (u8)(id - 10000). Sets that bit of +b08.
void award_unlock(std::span<std::uint8_t, agent_record_size> agent, const AwardTables& tables, std::int32_t id);

// 003305d0(agent): objective points of the +84 bits set, rating points whose
// stat reached its threshold, award points of the elements whose flag is set,
// plus 5 (+40) + 5 (+44), all wrapping.
std::int32_t award_score(std::span<const std::uint8_t, agent_record_size> agent, const AgentLoadState& state,
    const AwardTables& tables);

// 00335350(agent, element, row, mark): when every bit of the element is 2 (an
// empty list counts), its flag becomes 2 under mark, else 0 -> 1; then the
// row's unlock.
void award_element_step(std::span<std::uint8_t, agent_record_size> agent, AgentAwardEntry& entry,
    std::int32_t unlock_id, std::uint32_t mark, const AwardTables& tables);

// 00333f60(agent, slot, nosave): reset_agent, refill (0, 1), +4 = slot,
// +b70 = 1; then, when nosave (a word) is 0 and the card is not busy, the save
// 00334110 and its result; else 0. state.level_count is *00532af4, the level
// table's own count, so it must equal tables.levels.size().
std::uint32_t create_agent(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
    const AwardTables& tables, std::int32_t slot, std::uint32_t nosave, bool card_busy, const AgentRefillHost& host,
    const std::function<std::uint32_t()>& save);

// 003340b0(agent, name): strcpy to +8ac (throws past +9a3, the appearance
// pointer); when *0052fe80 is nonzero, +b73 = 1 and *0052fe80 = 0.
void set_agent_name(std::span<std::uint8_t, agent_record_size> agent, std::string_view name, std::uint32_t& message_count);

// 00333b90(agent, name, 0): the objective row's bit (00330aa0 over the
// lowercased 16-byte key 003fda40; 0 when no row matches) is set in +84 and
// +a4. With a nonzero third argument multiplayer also sends an event; that
// form is not hosted.
void set_agent_objective(std::span<std::uint8_t, agent_record_size> agent, const AwardTables& tables, std::string_view name);

struct AgentEndLevelHost {
    // 00245890(*004ffc2c): the mission clock, read when +d4 < 0.
    std::function<float()> mission_time;
    // 00335e50(agent, 0, 1): the refill (refill_agent with an AgentRefillHost).
    std::function<void()> refill;
    // 002888c0(list) / 00332500(agent, list) / 00405a60(*(agent + 9a4), list) /
    // 002887c0(list, -1): the parts list the appearance is updated with.
    std::function<void()> parts;
};

struct AgentEndLevelInput {
    std::string level;              // 004ffc90, the current level name
    std::uint32_t completed = 0;    // a1 (a word; +b74 keeps its low byte)
    bool multiplayer = false;       // 005721c8
    std::array<std::uint32_t, 8> solo{};   // 00532a10: the objectives a solo completion requires
    std::array<std::uint32_t, 8> multi{};  // 00532a40
};

// 00335830 AgentData_EndLevel(agent, completed), from 0012fa60 at the level
// end: +b74 = completed; +d4 = the mission clock when +d4 < 0. For each level
// record i < (s32)+c8 whose registry name (00532af0 row i) equals the level
// case-insensitively, when completed: +3c + 1; the objectives matching
// "<prefix>_*" and not "*_COMPLETED" (prefix: 00336060) are scanned; when
// none is missing from +a4 while its bit is set in the solo (multiplayer:
// multi) requirement words, +b71 = 1 and the record's time word (+0 solo, +4
// multiplayer) takes (s32)+d4 when it is negative or above +d4; the record's
// +14 / +10 take +70 / +6c when negative or above; then 00333b90(agent,
// "<prefix of 004ffc90>_COMPLETED", 0). Afterwards the refill 00335e50(agent,
// 0, 1) and the parts tail run.
void end_agent_level(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
    const AwardTables& tables, const AgentEndLevelInput& input, const AgentEndLevelHost& host);

}  // namespace omega
