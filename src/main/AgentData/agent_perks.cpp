#include "agent_perks.hpp"

#include <stdexcept>
#include <string>

namespace omega {

namespace {
std::uint32_t unlock_word_offset(std::uint32_t index) { return agent_unlock_words + 4 * ((index & 0xff) >> 5); }
}

bool agent_has_unlock(std::span<const std::uint8_t, agent_record_size> agent, std::uint32_t index) {
    const auto o = unlock_word_offset(index);
    const std::uint32_t word = std::uint32_t(agent[o]) | std::uint32_t(agent[o + 1]) << 8 |
                               std::uint32_t(agent[o + 2]) << 16 | std::uint32_t(agent[o + 3]) << 24;
    return (word >> (index & 31)) & 1;
}

std::vector<std::int32_t> agent_unlocked_weapons(std::span<const std::uint8_t, agent_record_size> agent,
                                                 const AwardTables& tables) {  // 00332ac0
    std::vector<std::int32_t> r;
    for (const auto& row : tables.unlocks) {
        if (row.bit < 0 || static_cast<std::uint32_t>(row.bit) >= (0xb50 - agent_unlock_words) * 8)
            throw std::runtime_error("00332ac0: unlock row " + row.name + " bit " + std::to_string(row.bit) +
                                     " outside +b08..+b4f");
        const auto o = agent_unlock_words + 4 * (static_cast<std::uint32_t>(row.bit) >> 5);
        const std::uint32_t word = std::uint32_t(agent[o]) | std::uint32_t(agent[o + 1]) << 8 |
                                   std::uint32_t(agent[o + 2]) << 16 | std::uint32_t(agent[o + 3]) << 24;
        if ((word >> (row.bit & 31)) & 1) r.push_back(row.weapon);  // 001c1080(vec, &row +14)
    }
    return r;
}

std::uint32_t run_agent_unlock_test(const SpawnedActorMemory& memory, std::uint32_t record, std::uint32_t index) {
    // srav of the sign-extended word by (index & 31), then andi 1: the bit itself.
    return (memory.read(record + unlock_word_offset(index)) >> (index & 31)) & 1;
}

std::uint32_t run_agent_record_accessor(const SpawnedActorMemory& memory, std::uint32_t profiles, std::uint32_t slot) {
    if (slot == 0xffffffffU) slot = memory.read(profiles + agent_active_slot);
    return memory.read(profiles + agent_records + 4 * slot);
}

} // namespace omega
