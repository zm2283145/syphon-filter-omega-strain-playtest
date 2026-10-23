#pragma once
// Agent profile load (AgentData, research/FRONTEND.md §6): the record reset
// 003361e0 and the file reader 00334790, the inverse of save_agent. The load
// reads the card file into the same persistent 0x4e20 buffer the save uses,
// decrypts the whole buffer in place (version >= 0xc) and copies the stream
// into the record by version gates (2 .. 0x11). Everything the two functions
// reach through the record's pointers and through globals is in
// AgentLoadState.
#include "agent_save.hpp"

#include <vector>

namespace omega {

inline constexpr std::size_t agent_award_lists = 5;
inline constexpr std::size_t agent_string_scratch_size = 0x80;
inline constexpr std::uint32_t agent_level_vector_limit = 0x300000;  // 0033ad90 aborts (00121b60) past it

using AgentLevelRecord = std::array<std::uint8_t, agent_level_record_size>;

// One 0x14-byte element of an award list: byte +0 and the byte vector at +4
// (count +8, data +c). The reset clears both.
struct AgentAwardEntry {
    std::uint8_t flag = 0;
    std::vector<std::uint8_t> bits;
};

struct AgentLoadState {
    // agent + aa8 + 0x10 i: count +aac, elements +ab0. The record's count words must match.
    std::array<std::vector<AgentAwardEntry>, agent_award_lists> awards;
    std::vector<std::uint8_t> award_bits;                 // *(agent + b00), agent + afc bytes
    std::vector<AgentLevelRecord> level_records;          // the vector at agent + c4 (capacity, count +c8, data +cc)
    // The vector at agent + b58 (capacity +b58, count +b5c, data +b60): the parts-table
    // rows UnlockPart pushed, as row indices. The count word must match.
    std::vector<std::uint32_t> parts;
    std::array<std::uint8_t, agent_appearance_size> appearance{};          // *(agent + 9a4)
    std::array<std::uint8_t, agent_string_scratch_size> string_scratch{};  // 0052fe00 (strings of versions < 0x10)
    std::uint32_t message_count = 0;                                       // 0052fe80
    std::array<std::uint8_t, agent_message_slots * agent_message_entry_size> messages{};  // 0052fe90
    std::int32_t level_count = 0;                                          // *00532af4
    // Words 6 and 7 of the reset's fill record (its sp + 0xa8 / 0xac) are
    // never written; the record keeps whatever the stack held there.
    std::array<std::uint32_t, 2> reset_record_tail{};
    std::uint32_t last_card_id = 0;  // 004ffbbc
};

// 003361e0(agent): zeroes +8..+83, +84..+c3, +b08..+b4f, +d8, +b50 and the
// bytes b68 / b70 / b71 / b72 / b74, sets +b6c = -1, clears every award
// element's flag and bits and the +b00 bytes, then resizes the level vector
// to 0 and to *00532af4 records of {-1, -1, 0, 0, -1, -1, tail}. Leaves the
// name +8ac and everything from +8ac to +aa7 alone.
void reset_agent(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state);

// 0040df00(W, name, buffer, 0x4e20): fills the buffer with the file; returns
// the read's result.
using AgentLoadReader = std::function<bool(std::string_view file_name, AgentSaveBuffer& buffer)>;

struct AgentLoadResult {
    std::int32_t result = 1;            // 0 loaded; 1 busy / no card / unformatted / read failed; 2 corrupt
    std::string file_name;              // 003fd550's name, or "" when that fails
    bool read_called = false;
    std::optional<std::size_t> cursor;  // *0052fdf0 - 0052afd0, when the load set it
    // 00335e50(agent, 0, 1) (the unlock / award rebuild from the objective
    // database) is due: version >= 3 on success. Not hosted; the caller runs it.
    bool refill = false;
};

// 00334790(agent, name, slot). name null means agent + 8ac; a negative slot
// names the file with agent +4 but is stored to +4 as given on success.
AgentLoadResult load_agent(AgentSaveBuffer& buffer, std::span<std::uint8_t, agent_record_size> agent,
    std::optional<std::span<const std::uint8_t>> name, std::int32_t slot, AgentLoadState& state,
    const AgentCardState& card, const AgentLoadReader& read);

}  // namespace omega
