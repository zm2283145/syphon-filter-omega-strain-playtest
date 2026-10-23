#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace omega {

struct StagedObjective {
    std::uint32_t id{};
    std::int32_t stage{};
    bool active{true};
    std::uint8_t status{};
    // DestroyObjectives (00227c60) deleted the object: no bucket holds it.
    bool destroyed{};
    bool operator==(const StagedObjective&) const = default;
};

// Single-player bucket/state semantics. Effects are composed separately.
class ObjectiveStage {
public:
    std::uint32_t add(std::int32_t stage);
    void set_active(std::uint32_t id, bool active);
    void start_stage(std::int32_t stage);
    void set_outcome(std::uint32_t id, bool success);
    // DestroyObjectives(manager) 002257f0 -> 00227c60: every object of the 32
    // stage buckets (+b0 + 0x80 g, counts +1130) and of the global bucket
    // (+10b0, count +11b0) is destroyed (virtual +8 = 00229180, then freed) and
    // the counts are zeroed. The ID counter +a8 and the current stage are kept,
    // so the next AddObjective continues the IDs; a destroyed ID is not found.
    void destroy_all();
    const StagedObjective* find(std::uint32_t id) const;
    std::int32_t current_stage() const { return current_stage_; }
    std::uint32_t next_id() const { return next_id_; }
    const std::vector<StagedObjective>& records() const { return records_; }
private:
    std::int32_t current_stage_{};
    std::uint32_t next_id_{1};
    // Index 32 is the separate global bucket, selected by stage -1.
    std::array<std::uint32_t, 33> counts_{};
    std::vector<StagedObjective> records_;
};

}
