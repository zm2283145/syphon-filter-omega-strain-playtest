#pragma once
// The profile services the front end's GuiCommandCenter reads (research/
// FRONTEND.md "Checkpoint 126 front-end flow"), over the loaded agent record
// (0xb78 bytes), its AgentLoadState and the boot award registries:
//   00332860 level_flag, 003328d0 level_start, 00332fe0 level_done,
//   00333b10 objective_done, 00336060 short_name, 00336170 / 002c90f0
//   agent_name, 00336110 / 002c9070 agent_rank, 003334c0 award_rows,
//   00333260 award_items, 003fdae0 key_text, 0040d580 card_space_needed.
#include "agent_load.hpp"
#include "award_registry.hpp"
#include "string_pool.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {

using AgentRecord = std::span<std::uint8_t, agent_record_size>;
using AgentRecordView = std::span<const std::uint8_t, agent_record_size>;

// 00334100(agent): the record's save size.
inline constexpr std::int32_t agent_profile_size = 0x3e80;

// 00332860(agent, a1, a2): the level-state bytes (count s32 +d8, entries of
// four signed bytes at +dc): 1 when an entry whose byte 3 is 0 has byte 0 ==
// a1 and (a2 == -1 or byte 1 == a2), else 0.
std::int32_t agent_level_flag(AgentRecordView agent, std::int32_t a1, std::int32_t a2);
// Checkpoint 132zf (the ZEUS slide show): 003329b0(agent, a1, a2, a3) is 1
// when an entry has bytes 0..2 == a1..a3 (signed), else 0; 00332950 answers
// that first entry's byte 3 != 0 instead (0 when none).
std::int32_t agent_zeus_has(AgentRecordView agent, std::int32_t a1, std::int32_t a2, std::int32_t a3);
std::int32_t agent_zeus_viewed(AgentRecordView agent, std::int32_t a1, std::int32_t a2, std::int32_t a3);
// 003328d0(agent, a1, a2, a3, t0): the first entry with bytes 0..2 == a1..a3
// (signed) gets byte 3 = t0 when it differs, and +b72 = 1 then.
void agent_level_start(AgentRecord agent, std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t t0);
// 00332a10(agent, a1, a2, a3): when no entry has bytes 0..2 == a1, a2 - 1,
// a3 - 1 (signed bytes against the words), the entry at +dc + 4 * +d8 becomes
// {a1, a2 - 1, a3 - 1, 0}, +d8 + 1 and +b72 = 1. A negative +d8 searches
// nothing and appends below +dc; that and an entry reaching the name (+8ac)
// throw.
void agent_level_progress(AgentRecord agent, std::int32_t a1, std::int32_t a2, std::int32_t a3);

// 003fda40: the 16-byte lower-case key (a name of 16 or more characters throws:
// the key is then not NUL terminated for 003fdb40).
std::string agent_lower_key(const std::string& name);

// 00332fe0(agent, key) over 003309c0 (the level registry 00532af0) and
// 00333b10(agent, name) over 00330aa0 (the objective registry 005329f0): the
// row whose interned key equals 003fdb40(key) gives the bit (none: -1, and a
// negative result becomes 0); the bit of the words at +b08 / +84.
std::int32_t agent_level_done(AgentRecordView agent, const AwardRegistries& registries, const StringPool& pool,
                              const std::string& key16);
std::int32_t agent_objective_done(AgentRecordView agent, const AwardRegistries& registries, const StringPool& pool,
                                  const std::string& name);

// 00336060(agent, s): s[0..2] and the last character when it is a digit (the
// ctype bit 4 of 0x499c71); a string under 3 characters or a byte >= 0x80
// throws (reads past the string / before the table).
std::string agent_short_name(const std::string& s);

// The gates 002c90f0 / 002c9070 of the name / rank: app +35 set, byte
// 005721c8 clear and app +24c a level with a table entry (0x48c1b0 /
// 0x48c200: the levels where the player is another character).
struct AgentGate {
    bool app_35 = false;
    bool mp_5721c8 = false;
    std::int32_t app_24c = -1;
};
using AgentStrings = std::function<std::optional<std::string>(const std::string& key)>;   // 003fdb50

// 00336170: the gate's string, else the C string at +8ac.
std::optional<std::string> agent_name(AgentRecordView agent, const AgentGate& gate, const AgentStrings& strings);
// 00336110: the gate's string, else 003fdb50(sprintf("S_%s", rank row name)),
// the rank row being the one +b50 points at (rank_value(k) == +b50).
std::optional<std::string> agent_rank(AgentRecordView agent, const AgentGate& gate, const AgentStrings& strings,
                                      const AwardRegistries& registries,
                                      const std::function<std::uint32_t(std::size_t)>& rank_value);

struct AgentAwardRow {
    std::int32_t key = -1;   // +4 the registry row's key
    std::uint8_t flag = 0;   // +8 the agent element's flag
    std::int32_t unlock = -1;  // +c the registry row's +10 (+10 is 0)
};
struct AgentAwardItem {
    std::string name;        // +4 003fdae0(requirement + i)
    std::uint8_t bit = 0;    // +8 the element's byte i
};
// 003334c0(agent, kind) and 00333260(agent, kind, index) for kinds 0..3 (kind
// 4 goes through agent_omega_items).
std::vector<AgentAwardRow> agent_award_rows(const AgentLoadState& state, const AwardRegistries& registries,
                                            std::int32_t kind);
std::vector<AgentAwardItem> agent_award_items(const AgentLoadState& state, const AwardRegistries& registries,
                                              const StringPool& pool, std::int32_t kind, std::int32_t index);
// 00333260(agent, 4, index)'s loop: for each byte i of the element awards[4][index], the omega requirement
// row 00532bb0 i (stride 0x1c) gets that byte at +8 and is pushed whole ({+0 GetString(name), +4 name,
// +8 the byte, +c a, +10 b, +14 c, +18 unlock}). The tail (00335350 on element 0, then the last row
// with +8 = 2 when element 0's flag is set) mutates the agent and is the caller's.
struct AgentOmegaItem {
    AwardsText name;
    std::uint8_t bit = 0;
    std::int32_t a = 0, b = 0, c = 0;
    std::int32_t unlock = -1;
};
std::vector<AgentOmegaItem> agent_omega_items(const AgentLoadState& state, const AwardRegistries& registries,
                                              std::int32_t index);
AgentOmegaItem agent_omega_last(const AwardRegistries& registries);  // [00532bb8] + 0x1c (count - 1), +8 = 2
// 003353f0(agent, short, list): the objective rows 005329f0 whose key text (003fdae0) matches "<short>_*"
// (00333890, '*') but not "*_COMPLETED", in registry order. The short is built inline (s[0..2] plus the
// last character when the ctype table marks it a digit). The caller adds +0 GetString(key) and +8
// 00333b10(agent, key) ? 2 : 1; +c..+14 are 0 and +18 is -1.
std::vector<std::string> agent_level_objective_keys(const AwardRegistries& registries, const StringPool& pool,
                                                    const std::string& name);
// 003fdae0(id): "" for a negative id, else the pool row's key up to its NUL.
std::string pool_key_text(const StringPool& pool, std::int32_t id);

// 003305d0(agent): the command centre's CLP total (32-bit wrapping sums):
// the points (+14) of each objective row 005329f0 whose bit (+4) is set in
// the words at +84; the points (+4) of each rating row 00532ad0 whose agent
// word +8 + 4 * stat (+8) is not below its threshold (+10); for each award
// list 0..4 (00532a70 + 0x10 k, through 003334c0) the points (+14) of the
// registry row of every element whose flag is set; then 5 * +40 and 5 * +44.
std::int32_t agent_clp(AgentRecordView agent, const AgentLoadState& state, const AwardRegistries& registries);

// 00333600(agent, list): one row per rating 00532ad0 (count 00532ad4), pushed by 00339270 as {+0 003fdb80(key),
// +4 key, +8 the agent's +b00 byte i (signed), +c the row's unlock (+14), +10 the percentage}: 100 when the
// agent word +8 + 4 * stat (+8) is not below the threshold (+10); else (word - low) * 100 / (threshold - low)
// (32-bit wrapping, signed div) when low (+c) < word; else 0. The text is left to the caller (loc_text).
struct AgentRatingRow {
    std::int32_t key = -1;
    std::int8_t flag = 0;
    std::int32_t unlock = -1;
    std::int32_t percent = 0;
};
std::vector<AgentRatingRow> agent_rating_rows(AgentRecordView agent, const AgentLoadState& state,
                                              const AwardRegistries& registries);

// 0040d580(card, size): KB of the save ((size + 0x3ff) >> 10) + 1, plus 0x2e
// for the save directory when it is absent; 0 for size 0 with the directory.
std::int32_t card_space_needed(std::int32_t size, bool save_directory_present);

// 00336700(agent): the boot constructor 00136710 runs (once, after 00147400
// and the AWARDS script 003e1b60) for each agent at app +1c8. Zeroes +84..
// +c0, sets up the empty level vector (+c4), clears +8dc / +91c / +95c,
// +99c = +9a0 = -1, builds each award list k from the registry 00532a70 +
// 0x10 k (one element {0, row.count zero bytes} per row), the +af8 bytes
// (one per rating row 00532ad0), zeroes +b08..+b4f, the empty parts vector
// (+b58), then 003361e0 with *00532af4 = the level rows, and clears +8ac /
// +8bc / +8cc, +0 = -1, +4 = 0 and the 0x440-byte appearance block (+9a4,
// 00494760 new) with +9a8..+aa7. The record's pointer words are host
// meaningless and stay 0; the count words match the state vectors.
void construct_agent(AgentRecord agent, AgentLoadState& state, const AwardRegistries& registries);

// 00288510: the title's "a saved agent exists" test: card +59d, +59e and
// +59f all set, then any entry of the directory 0040d600 lists that has no
// '.' (00129088), starts with a digit (ctype bit 4 of 0x49c971; a byte >=
// 0x80 indexes before the table and throws) and decodes with 003fd380(name,
// out, &slot, 0x10).
std::uint8_t card_save_present(bool present, bool formatted, bool directory, std::span<const std::string> entries,
                               bool font_ready, std::int32_t map_count);

}  // namespace omega
