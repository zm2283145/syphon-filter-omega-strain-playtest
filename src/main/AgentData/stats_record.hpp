#pragma once
// 002cbd40(a0, blob): the online statistics record (0xfc bytes) the login tail
// (0044ce60) and the game entry (0044e340 / 0044e430 / 0044e960) upload with
// AccountUpdateStats (research/ONLINE.md section 3.6). EE-verified against
// tests/stats_record_goldens.hpp (tools/ee/gen_stats_record.py).
//
// A blob whose byte 0 is not 0xb is cleared (0xfc bytes) and gets byte 0 = 0xb
// (the bytes it does not write, +e8..+fb, are otherwise kept: the login tail
// sets them afterwards). From the selected agent (001314f0(004ffb50, -1)):
//   +1        the rank row's +4 (*(agent + b50) + 4), 0 for a null +b50;
//   +2 / +3   cvt.w.s(100.0f * (float(stat c) / float(stat b))) and the same
//             with stats a / 9, 0 when the numerator (read twice) is 0;
//   +4..+c    u16 stats d, 1, 4, 6, 7;  +10..+20  u32 stats 9, a, 2, 0, 8;
//   +24       bit i for each of the first 24 rating rows (00333600) whose
//             flag (+8) is set, bit 24 + i for the first 8 rows of award list
//             0 (003334c0(agent, 0));
//   +28..+e7  agent +9a8 (the compact appearance), after a memset.
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace omega {

inline constexpr std::size_t stats_record_size = 0xfc;
inline constexpr std::uint8_t stats_record_version = 0xb;

struct StatsRecordAgent {
    std::optional<std::uint32_t> rank;                       // *(agent + b50) + 4; nullopt: +b50 is null
    std::function<std::int32_t(std::uint32_t)> statistic;    // 00333ee0
    std::span<const std::uint8_t> rating_flags;              // 00333600 rows' +8
    std::span<const std::uint8_t> award_flags;               // 003334c0(agent, 0) rows' +8
    std::span<const std::uint8_t, 0xc0> appearance;          // agent + 9a8
};

// cvt.w.s(100.0f * (float(num) / float(den))) for num != 0: the EE's x / 0
// gives the signed maximum and cvt.w.s saturates (0x7fffffff / 0x80000000).
std::int32_t stats_record_percent(std::int32_t num, std::int32_t den);

void build_stats_record(std::span<std::uint8_t, stats_record_size> blob, const StatsRecordAgent& agent);

}  // namespace omega
