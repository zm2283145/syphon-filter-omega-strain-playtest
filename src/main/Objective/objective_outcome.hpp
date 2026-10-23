#pragma once

#include "objective_stage.hpp"
#include <optional>
#include <ostream>
#include <string>
#include <variant>

namespace omega {

struct ObjectiveNotification {
    std::string text;
    bool operator==(const ObjectiveNotification&) const = default;
};

// Type-4 action request. Delivery, asset loading and audio playback are separate.
struct ObjectiveAction {
    std::int32_t mission{}, category{}, variant{1};
    bool operator==(const ObjectiveAction&) const = default;
};
using ObjectiveEffect = std::variant<ObjectiveNotification, ObjectiveAction>;

struct ObjectiveOutcomeContext {
    std::int32_t mission_cue{}; // Explicit resolved value; no guessed mission mapping.
    bool eligible_actor{}; // Caller has validated the optional actor's native class.
};

struct ObjectiveOutcome {
    std::uint32_t total{}, completed{};
    std::optional<std::int32_t> threshold;
    std::vector<ObjectiveEffect> effects;
};

// Bounded solo transition, not a complete gameplay callback. Text is supplied by
// configuration (null and empty differ). Returns ordered requests for the host;
// this function does not deliver them or perform progression-key updates.
ObjectiveOutcome apply_objective_outcome(ObjectiveStage& stage, std::uint32_t id,
    bool success, const std::optional<std::string>& text, ObjectiveOutcomeContext context);
void write_objective_outcome(std::ostream& out, const ObjectiveOutcome& outcome);

}
