#include "objective_stage.hpp"

#include <stdexcept>

namespace omega {

std::uint32_t ObjectiveStage::add(std::int32_t stage) {
    if (stage < -1 || stage >= 32)
        throw std::runtime_error("Objective stage outside supported bucket range");
    auto& count = counts_[stage == -1 ? 32 : static_cast<std::size_t>(stage)];
    if (count == 32) return 0;
    // At most 33*32 objects exist, so the original counter cannot overflow here.
    records_.push_back({next_id_, stage, true, 0});
    ++count;
    return next_id_++;
}

const StagedObjective* ObjectiveStage::find(std::uint32_t id) const {
    if (id == 0 || id >= next_id_) return nullptr;
    const auto& object = records_[id - 1];
    return object.destroyed ? nullptr : &object;
}

void ObjectiveStage::set_active(std::uint32_t id, bool active) {
    if (!find(id)) throw std::runtime_error("Invalid nonnull staged objective ID");
    records_[id - 1].active = active;
}

void ObjectiveStage::set_outcome(std::uint32_t id, bool success) {
    if (!find(id)) throw std::runtime_error("Invalid nonnull objective outcome ID");
    records_[id - 1].status = success ? 1 : 2;
}

void ObjectiveStage::start_stage(std::int32_t stage, const std::function<void(std::uint32_t)>& on_deactivate) {
    if (stage < 0 || stage >= 32)
        throw std::runtime_error("Current objective stage outside supported bucket range");
    if (stage == current_stage_) return;
    for (auto& object : records_) {
        if (!object.destroyed && object.stage == current_stage_) {
            object.active = false;
            if (on_deactivate) on_deactivate(object.id);
        }
    }
    current_stage_ = stage;
}

void ObjectiveStage::destroy_all() {
    for (auto& object : records_) object.destroyed = true;
    counts_.fill(0);
}

}
