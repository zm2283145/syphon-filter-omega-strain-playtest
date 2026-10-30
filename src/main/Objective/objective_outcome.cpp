#include "objective_outcome.hpp"

#include <algorithm>
#include <utility>

namespace omega {
namespace {
void quoted(std::ostream& out, const std::string& text) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u00" << digits[c >> 4] << digits[c & 15];
        else out << static_cast<char>(c);
    }
    out << '"';
}
}

ObjectiveOutcome apply_objective_outcome(ObjectiveStage& stage, std::uint32_t id,
    bool success, const std::optional<std::string>& text, ObjectiveOutcomeContext context) {
    // Build all owned effect data before committing state. Allocation failure
    // must not leave a native caller with changed status but lost requests.
    auto updated = stage;
    updated.set_outcome(id, success);
    ObjectiveOutcome result;
    for (const auto& object : updated.records()) {
        if (object.destroyed) continue;
        if (object.stage != -1 && object.stage != updated.current_stage()) continue;
        ++result.total;
        if (object.active && object.status == 1) ++result.completed;
    }
    result.completed = std::min(result.completed, 20U);
    if (text) result.effects.emplace_back(ObjectiveNotification{*text});
    if (success) {
        if (result.completed == result.total) result.threshold = 23;
        else if (result.completed == result.total / 2) result.threshold = 22;
        if (result.threshold && context.mission_cue > 0)
            result.effects.emplace_back(ObjectiveAction{context.mission_cue, *result.threshold, 1});
    }
    if (context.eligible_actor && context.mission_cue > 0)
        result.effects.emplace_back(ObjectiveAction{context.mission_cue, success ? 13 : 11, 1});
    stage = std::move(updated);
    return result;
}

ObjectiveOutcome apply_received_objective_outcome(ObjectiveStage& stage, std::uint32_t id,
    bool success, const std::optional<std::string>& text) {
    ObjectiveOutcome result;
    if (text) result.effects.emplace_back(ObjectiveNotification{*text});
    stage.set_outcome(id, success);
    return result;
}

void write_objective_outcome(std::ostream& out, const ObjectiveOutcome& outcome) {
    out << "{\"total\":" << outcome.total << ",\"completed\":" << outcome.completed
        << ",\"threshold\":";
    if (outcome.threshold) out << *outcome.threshold;
    else out << "null";
    out << ",\"effects\":[";
    bool first = true;
    for (const auto& effect : outcome.effects) {
        if (!first) out << ',';
        first = false;
        if (const auto* notification = std::get_if<ObjectiveNotification>(&effect)) {
            out << "{\"kind\":\"notification\",\"text\":";
            quoted(out, notification->text);
        } else {
            const auto& action = std::get<ObjectiveAction>(effect);
            out << "{\"kind\":\"action\",\"mission\":" << action.mission
                << ",\"category\":" << action.category << ",\"variant\":" << action.variant;
        }
        out << '}';
    }
    out << "]}";
}

}
