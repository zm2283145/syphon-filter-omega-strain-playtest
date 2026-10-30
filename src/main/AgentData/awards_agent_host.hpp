#pragma once
// The AgentData member natives the AWARDS script calls from
// awards::SetAwardsAndBonuses (research/FRONTEND.md §6), bound by 004d3840 to
// the callbacks 0032f130..0032fc60. Each callback takes one argv array
// (argv[0] the agent) and forwards to a worker on the agent record, the
// AWARDS registries (00532a70..00532bb0) and the parts table 00532b30.
//
// Names reach the workers three ways, as in the EE: 003fdb40 (null -> -1,
// else the pool id: award_intern), 003fda40 (strncpy 16 + lowercase into a
// key, then the pool id; a null name crashes, so it throws) and raw strcmp /
// strcasecmp against registry text. Anything the EE would read or write past
// the modelled state (an element index past the agent's list, a bit past an
// element's byte vector, a Zeus entry past +8ab, a level row past the
// registry) throws.
#include "agent_refill.hpp"
#include "award_registry.hpp"
#include "parts_table.hpp"
#include "script.hpp"
#include "string_pool.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

inline constexpr std::size_t agent_zeus_capacity = (0x8ac - 0xdc) / 4;  // entries +dc.., count +d8
inline constexpr std::uint32_t agent_parts_limit = 0x1800000;           // 00139200 aborts past it

struct AwardsAgentContext {
    std::span<std::uint8_t, agent_record_size> agent;
    AgentLoadState& state;
    const AwardRegistries& registries;
    const AwardTables& tables;  // the same registries by name, for the unlock (003352c0) and the score (003305d0)
    const StringPool& pool;
    std::span<const PartRow> parts;
    // 00332610's sprintf("partname %s is not in table\n") for a part name not
    // in the table (the text is built on the stack and dropped).
    std::vector<std::string> missing_parts;
    // PORT-ONLY: nullopt keeps native score-based HasRank; -1 forces no rank
    // for a scratch AWARDS pass; nonnegative selects one registered rank row.
    std::optional<std::int32_t> port_rank_override;
};

// One member native by its script name; `arguments` follow the agent (strings
// as std::string, a null string as Word 0; ints and enum engine values as
// Word). The int natives return their value, the void ones nullopt. Throws
// for any other name.
std::optional<Word> call_awards_agent_native(AwardsAgentContext& context, std::string_view name,
    const std::vector<Value>& arguments);

// A Host serving those natives to the AWARDS script. The agent is passed as
// the Word `agent_token`; every member call must carry it. Each external is
// checked against its declared parameter and return types.
class AwardsAgentHost final : public Host {
public:
    AwardsAgentHost(AwardsAgentContext& context, Word agent_token) : context_(context), token_(agent_token) {}
    std::optional<Value> call(const Script& script, const Function& function,
        const std::vector<Value>& arguments) override;
    Word enum_value(const std::string& name, Word member) override { return awards_enum_value(name, member); }

private:
    AwardsAgentContext& context_;
    Word token_;
};

// 003e1b60(0x555070, "awards::SetAwardsAndBonuses", 1, agent): the whole
// script call over the agent (about 1.1 M VM steps on the shipped script).
void run_set_awards_and_bonuses(const Script& awards, AwardsAgentContext& context);

}  // namespace omega
