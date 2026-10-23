#pragma once
// Agent profile saves (AgentData, research/FRONTEND.md §6): the file stream
// written by 00334110, its "Bite Me!!" cipher and XOR checksum, and the card
// file name encoder 003fd550. The original builds the stream in a persistent
// global buffer (0052afd0, 0x4e20 bytes) and encrypts the whole buffer in
// place, so bytes it does not write (three after +b68, the stale tail of a
// short stream) carry over from the buffer's previous use; AgentSaveBuffer
// keeps that state.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace omega {

inline constexpr std::size_t agent_record_size = 0xb78;
inline constexpr std::size_t agent_save_buffer_size = 0x4e20;
inline constexpr std::size_t agent_appearance_size = 0x440;
inline constexpr std::size_t agent_level_record_size = 0x20;
inline constexpr std::size_t agent_message_entry_size = 0xac;
inline constexpr std::size_t agent_message_slots = 0x40;
inline constexpr std::uint32_t agent_save_version = 0x11;
inline constexpr std::string_view agent_save_key = "Bite Me!!";  // *004926d0 -> 004b8e00

using AgentSaveBuffer = std::array<std::uint8_t, agent_save_buffer_size>;

// The per-save key word at file +4: x = T0_COUNT (10000800) + *00533870;
// x += x << 16; x ^ 0xffff.
std::uint32_t agent_save_key_word(std::uint32_t t0_count, std::uint32_t seed_533870);

// 003fd550(name, out, slot, max): each character c becomes '0' + (c - 0x20) / 0x3e
// and '@' + (c - 0x20) % 0x3e, then '0' + slot. Fails (nullopt) when the font
// is not ready (byte 0055d450 or word 0055ca90 zero), when 2 * strlen + 1 >= max
// (unsigned), when slot is outside 0..9, or for a character below 0x20 or at or
// past the glyph count. name is read up to its NUL; a name without one throws.
std::optional<std::string> encode_agent_file_name(std::span<const std::uint8_t> name, std::int32_t slot,
    bool font_ready, std::int32_t glyph_count, std::uint32_t max = 0x20);

// The cipher loop of 00334110 (and the load's decrypt): k = (s8)buffer[4],
// step = (s8)buffer[5]; for i = 8 .. size - 1: buffer[i] ^= (u8)(k + key[j]),
// and after each pass over the key k += step. Its own inverse.
void agent_save_cipher(std::span<std::uint8_t> buffer, std::string_view key = agent_save_key);

// What 00334110 reads besides the record itself.
struct AgentSaveSources {
    std::span<const std::uint8_t, agent_appearance_size> appearance;  // *(agent + 9a4)
    std::span<const std::uint8_t> level_records;                       // *(agent + cc), agent + c8 records
    std::uint32_t message_count = 0;                                   // *0052fe80 (stored as is)
    std::span<const std::uint8_t, agent_message_slots * agent_message_entry_size> messages;  // 0052fe90
    std::span<const std::uint8_t, agent_message_entry_size> stack_filler;  // its sp + 0x60, for slots >= count
    std::uint32_t key_word = 0;
};

// The memory-card wrapper state the save and load test: byte 0055a834 (busy),
// the card W = *0055a830 (+59d present, +59e formatted, +5a8 id) and the font
// state of 003fd550.
struct AgentCardState {
    bool busy = false;
    bool card_present = true;
    bool font_ready = true;
    std::int32_t glyph_count = 0;
    bool card_formatted = true;  // the card's +59e (the load tests it)
    std::uint32_t card_id = 0;   // the card's +5a8 (the load copies it to 004ffbbc)
};

// 0040e090(W, name, buffer, size, 1); returns the card write's result.
using AgentSaveWriter = std::function<bool(std::string_view file_name, std::span<const std::uint8_t> file)>;

struct AgentSaveResult {
    bool flushed_messages = false;  // 002c8090(*004ffc04) ran (name present)
    bool written = false;           // the writer was called
    bool result = false;            // 00334110's return value
    std::size_t size = 0;
};

// 00334110(agent): returns early (0) with the name's first byte zero (before
// the flush), busy, or no card. Otherwise builds the version-0x11 stream in
// buffer, appends the XOR of its words, encrypts the whole buffer, writes the
// file (named by encode_agent_file_name, or "" when that fails) and clears
// agent +b72 / +b73.
AgentSaveResult save_agent(AgentSaveBuffer& buffer, std::span<std::uint8_t, agent_record_size> agent,
    const AgentSaveSources& sources, const AgentCardState& card, const AgentSaveWriter& write);

}  // namespace omega
