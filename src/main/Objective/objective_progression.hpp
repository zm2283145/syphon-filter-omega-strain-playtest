#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace omega {
using ObjectiveBits = std::array<std::uint32_t, 8>;

struct ProgressionObjective {
    std::int32_t intern_id{};
    std::uint32_t index{};
    std::int32_t points{};
    bool operator==(const ProgressionObjective&) const = default;
};

// Original string-pool IDs are inputs. -1 means the original pool lookup failed.
// This does not replace the pool by a hash or assume unknown strings are unique.
class ObjectiveProgression {
public:
    std::uint32_t register_objective(std::int32_t intern_id, std::int32_t points,
                                    bool solo_required = false, bool multi_required = false);
    std::uint32_t lookup(std::int32_t intern_id) const;
    const std::vector<ProgressionObjective>& rows() const { return rows_; }
    const ObjectiveBits& solo_required() const { return solo_required_; }
    const ObjectiveBits& multi_required() const { return multi_required_; }
private:
    std::vector<ProgressionObjective> rows_;
    ObjectiveBits solo_required_{};
    ObjectiveBits multi_required_{};
};

class AgentObjectiveBits {
public:
    void add(const ObjectiveProgression& table, std::int32_t intern_id);
    bool is_complete(const ObjectiveProgression& table, std::int32_t intern_id) const;
    // Objective-bit portion only; not the full original mission-stat reset.
    void clear_mission_bits_only() { mission_.fill(0); }
    const ObjectiveBits& persistent() const { return persistent_; }
    const ObjectiveBits& mission() const { return mission_; }
private:
    ObjectiveBits persistent_{};
    ObjectiveBits mission_{};
};
}
