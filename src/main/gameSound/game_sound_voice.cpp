#include "game_sound_voice.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::array<StaticSoundDescriptor, 37> descriptors{{
    {0x004ef0b0U, 1, "B_HIT_PC_M"},      {0x004ef250U, 1, "B_HIT_NPC_M"},     {0x004ef3f0U, 1, "B_BY_PC_S"},
    {0x004ef590U, 1, "B_HIT_CONCRETE"},  {0x004ef730U, 1, "B_HIT_KEVLAR"},    {0x004ef8d0U, 0, "F_CONCRETE_M"},
    {0x004efa70U, 0, "F_CONCRETE_F"},    {0x004efc10U, 0, "F_METAL_SOLID_M"}, {0x004efdb0U, 0, "F_METAL_SOLID_F"},
    {0x004eff50U, 2, "V_NPC_HIT_M"},     {0x004f00f0U, 2, "V_NPC_DETH_M"},    {0x004f0290U, 2, "V_NPC_HIT_F"},
    {0x004f0430U, 2, "V_NPC_DETH_F"},    {0x004f05d0U, 2, "V_PC_HIT_M"},      {0x004f0770U, 2, "V_PC_DETH_M"},
    {0x004f0910U, 2, "V_PC_HIT_F"},      {0x004f0ab0U, 2, "V_PC_DETH_F"},     {0x004f0c50U, 1, "E_FLASHLIGHT"},
    {0x004f0df0U, 1, "E_IR_ON_OFF"},     {0x004f0f90U, 1, "E_TIMER"},         {0x004f1130U, 1, "G_TASER_LAUNCH"},
    {0x004f12d0U, 1, "G_TASER_BUZZ"},    {0x004f1470U, 1, "G_TASER_FIRE"},    {0x004f1610U, 2, "V_BURN_SCREAM"},
    {0x004f17b0U, 1, "E_TAKE_AMMO"},     {0x004f1950U, 1, "E_WEP_SWITCH"},    {0x004f1af0U, 1, "E_WEP_PICKUP"},
    {0x004f1c90U, 1, "E_BACKPACK"},      {0x004f1e30U, 1, "G_NECKBREAK"},     {0x004f1fd0U, 1, "E_THERMAL_IM"},
    {0x004f2170U, 1, "E_ZOOM"},          {0x004f2310U, 1, "G_STUNSTICK"},     {0x004f24b0U, 1, "G_KNIFE_SLIT"},
    {0x004f2650U, 1, "G_KNIFE_MISS"},    {0x004f27f0U, 1, "G_HGUN_DRYFIRE"},  {0x004f2990U, 1, "G_RIFLE_DRYFIRE"},
    {0x004f2b30U, 1, "G_SHGUN_DRYFIRE"},
}};

std::string format_line(std::int32_t a, std::int32_t b, std::int32_t c) {
    char text[40];
    std::snprintf(text, sizeof text, "%d_%d_%d", static_cast<int>(a), static_cast<int>(b), static_cast<int>(c));
    return text;
}

}  // namespace

std::span<const StaticSoundDescriptor> static_sound_descriptors() { return descriptors; }

const StaticSoundDescriptor& static_sound_descriptor(std::uint32_t address) {
    for (const auto& d : descriptors)
        if (d.address == address) return d;
    char text[16];
    std::snprintf(text, sizeof text, "%08x", static_cast<unsigned>(address));
    throw std::runtime_error(std::string("Game sound: ") + text + " is not a static sound descriptor");
}

GameSoundRecord static_sound_record(const StaticSoundDescriptor& d) {
    GameSoundRecord r;
    r.type_0 = d.type;
    std::memcpy(r.name_10.data(), d.name, std::strlen(d.name));
    r.owner_20 = 0;
    r.key_24 = -1;
    r.priority_180 = 0;
    r.slot_184 = -1;
    r.subtitle_186 = 0;
    return r;
}

std::optional<std::string> player_voice_line_name(std::int32_t cue, std::int32_t line, const VoiceLineSpeaker& s) {
    if (cue <= 0) return std::nullopt;                                   // 0018c80c
    std::int32_t v = 0;                                                  // 0018c870 (solo)
    if (s.multiplayer) {
        if (s.controller_58 && s.controller_54)                          // +58, virtual +54
            v = static_cast<std::int32_t>((s.serial_c & 0x7f000000U) >> 24) - 1;
        else v = -1;
    }
    return format_line(cue, line, v + 1);                                // 0018c87c: t0 = v + 1
}

std::optional<std::string> mission_cue_line_name(std::int32_t cue, std::int32_t a0, std::int32_t a1) {
    if (cue <= 0) return std::nullopt;                                   // 00245494
    return format_line(cue, a0, a1);
}

GameSoundMessage voice_line_message(GameSoundManager& manager, std::string_view name) {
    if (name.size() >= 16)
        throw std::runtime_error("Game sound: the voice line name '" + std::string(name) +
                                 "' overflows 001ca010's 16-byte copy (00129360)");
    return {GameSoundMessage::Kind::quip_20f2a0, 1, manager.message_copy(manager.request(4, name, 0, 0), false)};
}

GameSoundMessage subtitle_voice_message(GameSoundManager& manager, std::string_view name) {
    return {GameSoundMessage::Kind::quip_20f2a0, 1, manager.message_copy(manager.request(4, name, 0, 100), false)};
}

}
