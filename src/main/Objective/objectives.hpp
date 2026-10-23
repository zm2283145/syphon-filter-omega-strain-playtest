#pragma once

#include "script.hpp"

#include <ostream>

namespace omega {

struct Objective {
    Word id{};
    Word category{}; // Legacy report name; original AddObjective argument selects a stage bucket.
    std::map<std::string, std::vector<Value>> properties;
    std::vector<std::string> script_variables;
    // Constructor 0x00229948 writes state zero. Mutation here is diagnostic only.
    Word diagnostic_status{};
    // DestroyObjectives (00227c60): GetObjective no longer finds it.
    bool destroyed{};
};

struct RecordedCall {
    std::string name;
    std::vector<Value> arguments;
};

class ObjectiveHost final : public Host {
public:
    explicit ObjectiveHost(bool multiplayer) : multiplayer_(multiplayer) {}
    std::optional<Value> call(const Script& script, const Function& function,
                              const std::vector<Value>& arguments) override;
    Word enum_value(const std::string& name, Word member) override;
    // Research injection only: does not perform original Succeed/Fail side effects.
    void set_diagnostic_status(Word id, Word status);
    // The answer game::IsMultiplayer gives (the mission host scopes it to a
    // call site, 132ob8).
    void set_multiplayer(bool multiplayer) { multiplayer_ = multiplayer; }
    std::vector<Objective> objectives;
    std::vector<RecordedCall> calls;
private:
    bool multiplayer_;
    Objective& objective(const Value& receiver);
};

struct ObjectiveRun {
    bool multiplayer{};
    std::uint64_t steps{};
    std::uint64_t initialization_steps{};
    std::vector<Objective> objectives;
    std::vector<RecordedCall> calls;
};

ObjectiveRun run_objectives(const Script& script, bool multiplayer);
void write_objective_runs(std::ostream& output, const std::vector<ObjectiveRun>& runs);
// Executes actual local CheckObjective_*/WasFailed_* functions against explicit
// status fixtures. The result is separate from the unchanged creation report.
void write_objective_status_probe(std::ostream& output, const Script& script,
                                 const std::vector<bool>& modes);

}
