#include "agent_parts.hpp"

#include "awards_agent_host.hpp"

#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

std::int32_t rd32(std::span<const std::uint8_t> b, std::size_t o) {
    std::int32_t v = 0;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}
void wr32(std::span<std::uint8_t> b, std::size_t o, std::int32_t v) { std::memcpy(b.data() + o, &v, 4); }

}  // namespace

std::vector<std::uint32_t> parts_above_score(std::span<const PartRow> parts, std::int32_t score) {
    if (parts.size() > agent_parts_limit)
        throw std::runtime_error("Parts list 003301e0: more rows than 00139200 grows to (it aborts)");
    std::vector<std::uint32_t> list;
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (score < parts[i].value) list.push_back(static_cast<std::uint32_t>(i));
    return list;
}

std::vector<std::uint32_t> locked_award_parts(std::span<std::uint8_t, agent_record_size> agent,
    const AgentLoadState& state, std::span<const PartRow> parts, std::int32_t score) {
    auto list = parts_above_score(parts, score);
    const std::int32_t owned = rd32(agent, 0xb5c);
    if (owned > 0 && static_cast<std::size_t>(owned) > state.parts.size())
        throw std::runtime_error("Parts filter 00332500: +b5c counts more parts than the agent's vector holds");
    for (std::int32_t s0 = 0; s0 < owned; ++s0) {
        const std::uint32_t part = state.parts[static_cast<std::size_t>(s0)];
        std::int64_t a3 = static_cast<std::int64_t>(list.size()) - 1;
        for (std::int64_t t0 = 0; t0 <= a3; ++t0) {
            if (list[static_cast<std::size_t>(t0)] != part) continue;
            list[static_cast<std::size_t>(t0)] = list[static_cast<std::size_t>(a3)];
            --a3;
        }
        list.resize(static_cast<std::size_t>(a3 + 1));
    }
    const auto count = static_cast<std::int32_t>(list.size());
    if (rd32(agent, 0xb6c) < 0) {
        wr32(agent, 0xb6c, count);
    } else if (count < rd32(agent, 0xb6c)) {
        agent[0xb68] = 1;
        wr32(agent, 0xb6c, count);
    }
    return list;
}

std::vector<std::uint32_t> locked_award_parts(std::span<std::uint8_t, agent_record_size> agent,
    const AgentLoadState& state, const AwardTables& tables, std::span<const PartRow> parts) {
    return locked_award_parts(agent, state, parts, award_score(agent, state, tables));
}

}  // namespace omega
