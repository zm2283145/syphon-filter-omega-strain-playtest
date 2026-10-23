#include "objective_progression.hpp"

#include <stdexcept>

namespace omega {
namespace {
void valid_intern_id(std::int32_t id) {
    if (id < -1) throw std::runtime_error("Unsupported objective string-pool ID");
}
void set(ObjectiveBits& bits, std::uint32_t index) {
    bits[index / 32] |= std::uint32_t{1} << (index % 32);
}
}

std::uint32_t ObjectiveProgression::register_objective(std::int32_t intern_id,
        std::int32_t points, bool solo_required, bool multi_required) {
    valid_intern_id(intern_id);
    if (rows_.size() >= 256)
        throw std::runtime_error("Objective registration exceeds 256-bit native boundary");
    const auto index = static_cast<std::uint32_t>(rows_.size());
    rows_.push_back({intern_id, index, points});
    if (solo_required) set(solo_required_, index);
    if (multi_required) set(multi_required_, index);
    return index;
}

std::uint32_t ObjectiveProgression::lookup(std::int32_t intern_id) const {
    valid_intern_id(intern_id);
    for (const auto& row : rows_) {
        if (row.intern_id == intern_id) return row.index;
    }
    // 00330aa0 clamps its missing-entry sentinel to zero, including an empty table.
    return 0;
}

void AgentObjectiveBits::add(const ObjectiveProgression& table, std::int32_t intern_id) {
    const auto index = table.lookup(intern_id);
    set(persistent_, index);
    set(mission_, index);
}

bool AgentObjectiveBits::is_complete(const ObjectiveProgression& table, std::int32_t intern_id) const {
    const auto index = table.lookup(intern_id);
    return (persistent_[index / 32] & (std::uint32_t{1} << (index % 32))) != 0;
}
}
