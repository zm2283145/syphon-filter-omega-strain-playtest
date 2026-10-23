#include "stats_record.hpp"

#include <algorithm>
#include <cstring>

namespace omega {
namespace {

void put16(std::span<std::uint8_t, stats_record_size> b, std::size_t at, std::int32_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> 8);
}

void put32(std::span<std::uint8_t, stats_record_size> b, std::size_t at, std::int32_t v) {
    for (std::size_t k = 0; k < 4; ++k) b[at + k] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * k));
}

}  // namespace

std::int32_t stats_record_percent(std::int32_t num, std::int32_t den) {
    if (den == 0) return num > 0 ? 0x7fffffff : static_cast<std::int32_t>(0x80000000u);
    const float r = 100.0F * (static_cast<float>(num) / static_cast<float>(den));
    if (r >= 2147483648.0F) return 0x7fffffff;
    if (r <= -2147483648.0F) return static_cast<std::int32_t>(0x80000000u);
    return static_cast<std::int32_t>(r);
}

void build_stats_record(std::span<std::uint8_t, stats_record_size> blob, const StatsRecordAgent& agent) {
    if (blob[0] != stats_record_version) {
        std::fill(blob.begin(), blob.end(), std::uint8_t{0});
        blob[0] = stats_record_version;
    }
    const auto& stat = agent.statistic;
    blob[1] = static_cast<std::uint8_t>(agent.rank.value_or(0));
    const auto ratio = [&](std::uint32_t num, std::uint32_t den) -> std::uint8_t {
        if (stat(num) == 0) return 0;   // 002cbdbc / 002cbdcc read the numerator twice
        return static_cast<std::uint8_t>(stats_record_percent(stat(num), stat(den)));
    };
    blob[2] = ratio(0xc, 0xb);
    blob[3] = ratio(0xa, 9);
    put16(blob, 4, stat(0xd));
    put16(blob, 6, stat(1));
    put16(blob, 8, stat(4));
    put16(blob, 0xa, stat(6));
    put16(blob, 0xc, stat(7));
    put32(blob, 0x10, stat(9));
    put32(blob, 0x14, stat(0xa));
    put32(blob, 0x18, stat(2));
    put32(blob, 0x1c, stat(0));
    put32(blob, 0x20, stat(8));
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < agent.rating_flags.size() && i < 24; ++i)
        if (agent.rating_flags[i] != 0) mask |= 1u << i;
    for (std::size_t i = 0; i < agent.award_flags.size() && i < 8; ++i)
        if (agent.award_flags[i] != 0) mask |= 1u << (24 + i);
    put32(blob, 0x24, static_cast<std::int32_t>(mask));
    std::copy(agent.appearance.begin(), agent.appearance.end(), blob.begin() + 0x28);
}

}  // namespace omega
