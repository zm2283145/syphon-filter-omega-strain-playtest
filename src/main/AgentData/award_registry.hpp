#pragma once
// AWARDS registries (research/FRONTEND.md §6). At boot the weapon database
// loader 00147400 registers one unlock row per record (00330110 into
// 00532b10, bit = (*004926e0)++ from 6), then awards::Init (the AWARDS
// script) calls the game::Register* natives bound by 004d3840. Each callback
// takes one argv array (argv[0] = the first declared argument) and builds a
// row on its stack that a vector push copies into the registry.
//
// Rows are kept as the EE holds them: names are string-pool ids (003fdb40,
// -1 when absent or null), the raw script strings only where the EE keeps the
// pointer (ranks, dossiers, omega requirements). Words the callbacks never
// write (uninitialised stack copied by the push) are not modelled; nothing
// native reads them.
#include "agent_refill.hpp"
#include "script.hpp"
#include "string_pool.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace omega {

using AwardsText = std::optional<std::string>;  // a script string; nullopt is the null pointer
using AwardsArg = std::variant<Word, AwardsText>;

// The bound overloads (004d3840) and their callbacks. E42 is
// AgentData_ScoreTypes, E43 AgentData_SpecialBonusTypes.
enum class AwardsNative : std::uint8_t {
    print,                          // client::Print(text): log only
    objective2,                     // 0032f0d0 RegisterObjective(name, points)
    objective4,                     // 0032efb0 RegisterObjective(name, points, solo, multi)
    dossier,                        // 0032ef70 RegisterDossier(name, rankName)
    level3,                         // 0032eed0 RegisterLevel(name, a1, a2)
    level4,                         // 0032edf0 RegisterLevel(name, a1, a2, a3)
    bonus_level,                    // 0032ed90 RegisterBonusLevel(name, reqName, count)
    rank2,                          // 0032ecf0 RegisterRank(name, threshold)
    rank3_unlock,                   // 0032ec50 RegisterRank(name, threshold, unlockName)
    rank3_special,                  // 0032ea40 RegisterRank(name, threshold, E43)
    rank4,                          // 0032eb90 RegisterRank(name, threshold, unlockName, text)
    rating5,                        // 0032e9d0 RegisterRating(name, points, E42, low, threshold)
    rating6_unlock,                 // 0032e960 RegisterRating(..., unlockName)
    rating6_special,                // 0032e8c0 RegisterRating(..., E43)
    special_rating4,                // 0032e850 RegisterSpecialRating(name, points, reqName, count)
    special_rating5_unlock,         // 0032e7e0 (..., unlockName)
    special_rating5_special,        // 0032e770 (..., E43)
    commendation4,                  // 0032e700
    commendation5_unlock,           // 0032e690
    commendation5_special,          // 0032e620
    medal4,                         // 0032e5b0
    medal5_unlock,                  // 0032e540
    medal5_special,                 // 0032e4d0
    omega,                          // 0032e470 RegisterOmega(name, reqName, count)
    omega_requirement4,             // 0032e400 RegisterOmegaRequirement(name, a, b, c)
    omega_requirement5,             // 0032e1c0 (..., unlockName)
};

struct AwardsInitCall {
    AwardsNative native{};
    std::vector<AwardsArg> args;  // argv order; ints and enum values as words
};

// The overload an external function binds to, by name and declared
// parameter types (strings are primitive:3 or primitive:7, ints
// primitive:1). Throws for anything else (the binding is not hosted).
AwardsNative awards_native(const Script& script, const Function& function);

// The engine values of the two AgentData enums (their members are not in the
// script): AgentData_ScoreTypes member k -> k < 13 ? k : k + 1;
// AgentData_SpecialBonusTypes -> {3, 4, 2, 1, 0, 5}. Throws past the members.
Word awards_enum_value(const std::string& name, Word member);

// Runs awards::Init in the script Vm and records every external call in
// order. Natives other than the bound overloads above throw.
std::vector<AwardsInitCall> read_awards_init(const Script& awards);

// 003fdb40 -> 003fe650 over the common pool: null -> -1; else the first
// equal 16-byte lowercased key, or -1. awards::Init runs at startup, before
// any mission pool (0055ca50) exists, so only the common pool is searched.
// Names StringPool::lookup cannot key faithfully throw: 16 or more characters
// (the EE's length scan reads past its 16-byte buffer, SCRIPT_HOSTING.md) and
// bytes outside 32..126 (0012b270's ctype table mangles bytes >= 0x80).
std::int32_t award_intern(const StringPool& pool, const AwardsText& name);

struct AwardUnlockEntry {  // 00532b10, stride 0x18 (00330110): +8 -1, +c 0
    std::int32_t key = -1;    // +0 intern(record +80)
    std::int32_t bit = 0;     // +4 (*004926e0)++
    std::int32_t weapon = 0;  // +14 record index
};
struct AwardObjectiveEntry {  // 005329f0, stride 0x18 (00330180): +8 -1, +c 0
    std::int32_t key = -1;    // +0
    std::int32_t index = 0;   // +4 (*00532bc0)++
    std::int32_t points = 0;  // +14
};
struct AwardLevelEntry {  // 00532af0 (00330180) and its pair 00532b90
    std::int32_t key = -1;          // +0
    std::int32_t bit = 0;           // +4 (*004926e0)++
    std::int32_t mission = -1;      // +14 argument 3 (-1 for the 3-argument form)
    std::int32_t pair_first = 0;    // pair +0 argument 1
    std::int32_t pair_second = 0;   // pair +4 argument 2
};
struct AwardEntry {  // 00532a70 + 0x10 L, stride 0x18 (0032ffd0 / 0032ff50)
    std::int32_t key = -1;          // +0
    std::int32_t requirement = -1;  // +8 intern(reqName)
    std::int32_t count = 0;         // +c
    std::int32_t unlock = -1;       // +10
    std::int32_t points = 0;        // +14
};
struct AwardRatingEntry {  // 00532ad0, stride 0x18 (0032fe00 / 0032fd70)
    std::int32_t key = -1;        // +0
    std::int32_t points = 0;      // +4
    std::uint8_t stat = 0;        // +8 (a byte; +9..+b not written)
    std::int32_t low = 0;         // +c
    std::int32_t threshold = 0;   // +10
    std::int32_t unlock = -1;     // +14
};
struct AwardRankEntry {  // 00532b50 ranks / 00532b70 dossiers, stride 0x24 (0032eb00 / 00330d10)
    AwardsText name;              // +0 the script string pointer
    std::int32_t index = 0;       // +4
    std::int32_t threshold = 0;   // +8
    std::int32_t unlock = -1;     // +c
    // +10: strncpy(text, 16) (zero padded, no NUL at 16 or more characters);
    // nullopt when the text is null: one NUL byte, +11..+1f not written.
    std::optional<std::array<char, 16>> text;
    std::int32_t title_key = -1;  // +20 intern(sprintf("S_RANK_%2.2d", index + 1))
};
struct AwardOmegaRequirementEntry {  // 00532bb0, stride 0x1c (0032e260)
    std::int32_t text_key = -1;  // +0 is 003fdb50(name): GetString of this id (null when -1)
    AwardsText name;             // +4
    std::int32_t a = 0, b = 0, c = 0;  // +c / +10 / +14 (+8 is the byte 0; +9..+b not written)
    std::int32_t unlock = -1;    // +18
};

struct AwardRegistries {
    std::vector<AwardUnlockEntry> unlocks;
    std::vector<AwardObjectiveEntry> objectives;
    std::array<std::uint32_t, 8> solo{};   // 00532a10 (8 words; 00532a30 is another object)
    std::array<std::uint32_t, 8> multi{};  // 00532a40
    std::array<std::vector<AwardEntry>, agent_award_lists> awards;  // SpecialRating, Commendation, Medal, BonusLevel, Omega
    std::vector<AwardRatingEntry> ratings;
    std::vector<AwardLevelEntry> levels;
    std::vector<AwardRankEntry> ranks;
    std::vector<AwardRankEntry> dossiers;
    std::vector<AwardOmegaRequirementEntry> omega_requirements;
    std::int32_t objective_counter = 0;  // *00532bc0
    std::int32_t bit_counter = 6;        // *004926e0
};

// 00147400's per-record step: counter++ then 00330110(row, i, old counter).
void register_weapon_unlocks(AwardRegistries& registries, const StringPool& pool,
                             std::span<const std::string> weapon_names);
// One callback. Throws where the EE would write outside the registries (a
// solo/multi bit at index 256 or more), dereference a null string (a dossier
// with ranks registered and a null name on either side), or for argument
// kinds the overload does not take.
void apply_awards_call(AwardRegistries& registries, const StringPool& pool, const AwardsInitCall& call);
AwardRegistries build_award_registries(const StringPool& pool, std::span<const std::string> weapon_names,
                                       std::span<const AwardsInitCall> calls);

// The name-based tables refill_agent consumes. Pooled names become their
// pool key text; every id -1 (and every unpooled weapon) becomes one shared
// name listed in AwardTables::unpooled, so it compares equal to itself only.
// Level and objective rows must be pooled (the EE reads their text by id).
AwardTables to_award_tables(const AwardRegistries& registries, const StringPool& pool,
                            std::span<const std::string> weapon_names);

}  // namespace omega
