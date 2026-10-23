#include "objective_schedule.hpp"
#include "objectives.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
class ProbeHost final : public Host {
public:
    ObjectiveHost objectives{false};
    std::vector<std::string> trace;
    std::optional<Value> call(const Script& script, const Function& function,
                              const std::vector<Value>& arguments) override {
        if (function.name == "game::RequestLevelEnd") {
            if (function.return_type != 4 || !function.parameters.empty() || !arguments.empty())
                throw std::runtime_error("Unexpected level-end diagnostic signature");
            trace.push_back("request_level_end_recorded");
            return std::nullopt;
        }
        auto result = objectives.call(script, function, arguments);
        if (function.name == "IsComplete") trace.push_back("is_complete");
        if (function.name == "WasFailed") trace.push_back("was_failed");
        return result;
    }
    Word enum_value(const std::string& name, Word member) override {
        return objectives.enum_value(name, member);
    }
};
}
void write_objective_schedule_probe(std::ostream& out, const Script& script) {
    out << "{\"scope\":\"Solo LevelEnd diagnostic; injected CHA status; recorded transition only\",\"runs\":[";
    bool first_run = true;
    for (const Word state : {1U, 2U}) {
        for (const auto& deltas : std::vector<std::vector<std::int32_t>>{{0,8999,1,999,1},{10000},{9000,1000}}) {
            ProbeHost host;
            Vm vm(script, host);
            vm.initialize();
            vm.invoke("objectives::CreateObjectives");
            const Variable* cha = nullptr;
            for (const auto& variable : script.variables) {
                if (variable.name == "objectives::Obj_cha") cha = &variable;
            }
            if (!cha || cha->external || cha->type != 2) throw std::runtime_error("Missing solo CHA variable");
            const auto* value = std::get_if<Word>(&vm.global(cha->storage));
            if (!value) throw std::runtime_error("Unexpected CHA ID type");
            const float id = word_float(*value);
            if (!std::isfinite(id) || id < 1 || id > host.objectives.objectives.size() || float(Word(id)) != id)
                throw std::runtime_error("Unexpected CHA objective ID");
            host.objectives.set_diagnostic_status(Word(id), state);
            host.trace.clear();
            const auto initial_steps = vm.steps();
            vm.invoke("objectives::LevelEnd");
            if (!first_run) out << ',';
            first_run = false;
            out << "{\"cha_status\":" << state << ",\"schedule_steps\":" << vm.steps()-initial_steps;
            auto pending = [&] {
                out << '[';
                bool first = true;
                for (const auto& event : vm.schedules().snapshot()) {
                    if (!first) out << ',';
                    first = false;
                    out << "{\"handle\":" << event.handle << ",\"remaining_ticks\":" << event.remaining_ticks << '}';
                }
                out << ']';
            };
            out << ",\"initial_pending\":"; pending();
            out << ",\"updates\":[";
            bool first_update = true;
            for (const auto delta : deltas) {
                const auto before = vm.steps();
                host.trace.clear();
                const auto count = vm.schedules().advance(delta);
                if (!first_update) out << ',';
                first_update = false;
                out << "{\"delta\":" << delta << ",\"executed\":" << count << ",\"steps\":" << vm.steps()-before << ",\"trace\":[";
                for (std::size_t i=0; i<host.trace.size(); ++i) {
                    if (i) out << ',';
                    out << '\"' << host.trace[i] << '\"';
                }
                out << "],\"pending\":"; pending(); out << '}';
            }
            out << "]}";
        }
    }
    out << "]}";
}
}
