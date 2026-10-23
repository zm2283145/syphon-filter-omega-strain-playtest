#pragma once
// Voice lines and the static sound descriptors (research/GENERATOR_RUNTIME.md
// "Checkpoint 119n"):
//   - the 37 descriptors 004ef0b0 .. 004f2b30 (stride 0x1a0) the static
//     constructors 004c7530 .. 004c8cf4 build (001ca210(desc, type, name, 0,
//     0)); the damage, death, pickup and weapon sites send them through
//     0020f990 / 0020f860 / 0020eba0;
//   - 0018c770(actor, line), the player's voice line, and 00245400(a0, a1),
//     the mission cue line: the name "%d_%d_%d" (0049dc10 / 004a72a8) of the
//     level's cue 00330b80(004ffc90) (nothing when it is <= 0), an inline
//     record 001ca010(rec, 4, name, 0, 0), the quip message 0020f2a0 with
//     +20 = 1, sent with 003c9420(msg, 004ffb50, 1) (delivered at once).
#include "game_sound.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace omega {

struct StaticSoundDescriptor {
    std::uint32_t address{};
    std::int32_t type{};      // +0: 0 ENVIRON (F_*), 1 WEPS, 2 VOX (V_*)
    const char* name{};       // +10
};

std::span<const StaticSoundDescriptor> static_sound_descriptors();
// Throws for an address that is not one of the 37 descriptors.
const StaticSoundDescriptor& static_sound_descriptor(std::uint32_t address);
// The record as the static constructor leaves it: owner 0, key -1, priority
// 0, slot -1, +186 0, the resources as 0036e5d0 constructs them.
GameSoundRecord static_sound_record(const StaticSoundDescriptor& descriptor);

// 0018c770's speaker: in a multiplayer game (*005721c8) the third number is
// ((+c & 0x7f000000) >> 24) when the actor's controller +58 is present and its
// virtual +54 answers true, otherwise 0; in a solo game it is 1.
struct VoiceLineSpeaker {
    bool multiplayer{};
    bool controller_58{};
    bool controller_54{};
    std::uint32_t serial_c{};
};
// The names; nullopt when the cue is <= 0 (nothing is sent).
std::optional<std::string> player_voice_line_name(std::int32_t cue, std::int32_t line, const VoiceLineSpeaker& speaker);
std::optional<std::string> mission_cue_line_name(std::int32_t cue, std::int32_t a0, std::int32_t a1);
// 001ca010(rec, 4, name, 0, 0) + 0020f2a0 + (+20 = 1): the message the two
// functions deliver (transport 1). 001ca010's strcpy is unbounded: a name of
// 16 or more characters throws.
GameSoundMessage voice_line_message(GameSoundManager& manager, std::string_view name);
// Checkpoint 132hud4: 0045ce10's PlayXA voice (0x0045cee0..0x0045cf28): a stack descriptor
// 001ca210(desc, 4, name, 0, 100), 0020f2a0(msg, desc), +20 = 1; the caller delivers it with
// 003c9420(msg, 004ffb50, 1). 001ca210's copy is the unbounded 00129360: 16 or more characters throw.
GameSoundMessage subtitle_voice_message(GameSoundManager& manager, std::string_view name);

}
