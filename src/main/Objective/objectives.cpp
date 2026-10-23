#include "objectives.hpp"

#include <array>
#include <iomanip>
#include <stdexcept>

namespace omega {
namespace {

Word number(const Value& value) {
    const auto* result = std::get_if<Word>(&value);
    if (!result) throw std::runtime_error("Objective callback expected a numeric word");
    return *result;
}

void text_argument(const Value& value) {
    if (!std::holds_alternative<std::string>(value))
        throw std::runtime_error("Objective callback expected text");
}

void manager_argument(const Value& value) {
    const auto* handle = std::get_if<ObjectHandle>(&value);
    if (!handle || handle->kind != ObjectHandle::Kind::objective_manager || handle->id != 0)
        throw std::runtime_error("Objective callback expected the manager handle");
}

void signature(const Script& script, const Function& function, const std::string& result,
               const std::vector<std::string>& parameters) {
    if (script.type_key(function.return_type) != result || function.parameters.size() != parameters.size())
        throw std::runtime_error("Unsupported callback signature: " + function.name);
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (script.type_key(function.parameters[i]) != parameters[i])
            throw std::runtime_error("Unsupported callback parameter type: " + function.name);
    }
}

void quoted(std::ostream& out, const std::string& text) {
    static constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char value : text) {
        if (value == '"' || value == '\\') out << '\\' << static_cast<char>(value);
        else if (value < 32) out << "\\u00" << digits[value >> 4] << digits[value & 15];
        else out << static_cast<char>(value);
    }
    out << '"';
}

void json_value(std::ostream& out, const Value& value) {
    if (const auto* number_value = std::get_if<Word>(&value)) out << *number_value;
    else if (const auto* text = std::get_if<std::string>(&value)) quoted(out, *text);
    else if (const auto* handle = std::get_if<ObjectHandle>(&value)) {
        quoted(out, handle->kind == ObjectHandle::Kind::objective_manager
            ? "objective_manager" : "objective:" + std::to_string(handle->id));
    } else throw std::runtime_error("Cannot serialize a frame address as an objective value");
}

void json_values(std::ostream& out, const std::vector<Value>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        json_value(out, values[i]);
    }
    out << ']';
}

}

Word ObjectiveHost::enum_value(const std::string& name, Word member) {
    // Registration 0x004d1130 declares TRUE first (1), then FALSE (0).
    if (name == "BOOL") {
        if (member > 1) throw std::runtime_error("Unresolved external enum member: BOOL");
        return member == 0 ? 1U : 0U;
    }
    static const std::map<std::string, Word> sizes{
        {"Objective_Types", 6}, {"Objective_MultiTypes", 4},
        {"Objective_NotifyTypes", 3}, {"Objective_States", 3}
    };
    const auto found = sizes.find(name);
    if (found == sizes.end() || member >= found->second)
        throw std::runtime_error("Unresolved external enum member: " + name);
    return member;
}

Objective& ObjectiveHost::objective(const Value& receiver) {
    const auto* handle = std::get_if<ObjectHandle>(&receiver);
    if (!handle || handle->kind != ObjectHandle::Kind::objective || handle->id == 0
        || handle->id > objectives.size() || objectives[handle->id - 1].destroyed)
        throw std::runtime_error("Invalid objective receiver");
    return objectives[handle->id - 1];
}

void ObjectiveHost::set_diagnostic_status(Word id, Word status) {
    if (status > 2) throw std::runtime_error("Unsupported diagnostic objective status");
    objective(ObjectHandle{ObjectHandle::Kind::objective, id}).diagnostic_status = status;
}

std::optional<Value> ObjectiveHost::call(const Script& script, const Function& function,
                                        const std::vector<Value>& arguments) {
    if (arguments.size() != function.parameters.size())
        throw std::runtime_error("Objective callback argument count mismatch");
    const auto& name = function.name;
    const std::string manager = "object:ObjectiveMan";
    const std::string object = "object:Objective";
    if (name == "game::IsMultiplayer") {
        signature(script, function, "primitive:1", {});
        calls.push_back({name, arguments});
        return Word(multiplayer_);
    }
    if (name == "game::GetObjectiveManager") {
        signature(script, function, manager, {});
        calls.push_back({name, arguments});
        return ObjectHandle{ObjectHandle::Kind::objective_manager, 0};
    }
    if (name == "game::GetString") {
        signature(script, function, "primitive:3", {"primitive:7"});
        text_argument(arguments[0]);
        calls.push_back({name, arguments});
        return arguments[0];
    }
    if (name == "AddObjective") {
        signature(script, function, "primitive:1", {manager, "primitive:1"});
        manager_argument(arguments[0]);
        const Word category = number(arguments[1]);
        if (objectives.size() >= 1024) throw std::runtime_error("Prototype objective limit reached");
        const Word id = static_cast<Word>(objectives.size() + 1);
        objectives.push_back(Objective{id, category, {}, {}});
        calls.push_back({name, arguments});
        return id;
    }
    if (name == "GetObjective") {
        signature(script, function, object, {manager, "primitive:1"});
        manager_argument(arguments[0]);
        const Word requested = number(arguments[1]);
        // Original lookup 0x002275a0 returns null for an absent ID. IDs here
        // remain research handles, not original engine receiver identities.
        const ObjectHandle handle{ObjectHandle::Kind::objective,
            requested != 0 && requested <= objectives.size()
                    && !objectives[requested - 1].destroyed ? requested : 0};
        calls.push_back({name, arguments});
        return handle;
    }
    if (name == "IsComplete" || name == "WasFailed") {
        signature(script, function, "primitive:1", {object});
        const auto* handle = std::get_if<ObjectHandle>(&arguments[0]);
        if (!handle || handle->kind != ObjectHandle::Kind::objective)
            throw std::runtime_error("Objective status query expected a typed objective");
        Word result = 0;
        if (handle->id != 0) {
            const auto status = objective(arguments[0]).diagnostic_status;
            result = Word(status == (name == "IsComplete" ? 1U : 2U));
        }
        // Typed null models the interpreter's skipped native call (return 0),
        // not a dereference by the native getter. Record the host query attempt.
        calls.push_back({name, arguments});
        return result;
    }
    static const std::array<std::string, 6> text_setters{
        "SetName", "SetMenuString", "SetMapString", "SetAddString", "SetCompleteString", "SetFailString"
    };
    for (const auto& setter : text_setters) {
        if (name == setter) {
            signature(script, function, "primitive:4", {object, "primitive:3"});
            text_argument(arguments[1]);
            objective(arguments[0]).properties[name] = {arguments[1]};
            calls.push_back({name, arguments});
            return std::nullopt;
        }
    }
    if (name == "SetType" || name == "SetNotifyType") {
        const std::string type = name == "SetType" ? "Objective_Types" : "Objective_NotifyTypes";
        signature(script, function, "primitive:4", {object, "enum:" + type});
        enum_value(type, number(arguments[1]));
        objective(arguments[0]).properties[name] = {arguments[1]};
    } else if (name == "SetMultiType") {
        signature(script, function, "primitive:4", {object, "enum:Objective_MultiTypes", "primitive:1"});
        enum_value("Objective_MultiTypes", number(arguments[1]));
        number(arguments[2]);
        objective(arguments[0]).properties[name] = {arguments[1], arguments[2]};
    } else if (name == "SetMaxOwners") {
        signature(script, function, "primitive:4", {object, "primitive:1"});
        objective(arguments[0]);
        number(arguments[1]);
        // Verified callback 0x00225600 resolves only the receiver through the
        // identity helper 0x00225790. It never reads the limit or mutates state.
        // Preserve its recorded call without inventing an ownership property.
    } else if (name == "Deactivate") {
        signature(script, function, "primitive:4", {object});
        objective(arguments[0]).properties[name] = {};
    } else {
        throw std::runtime_error("Unsupported engine callback: " + name);
    }
    calls.push_back({name, arguments});
    return std::nullopt;
}

ObjectiveRun run_objectives(const Script& script, bool multiplayer) {
    ObjectiveHost host(multiplayer);
    Vm vm(script, host);
    vm.initialize();
    const auto initialization_steps = vm.steps();
    vm.invoke("objectives::CreateObjectives");
    for (auto& objective : host.objectives) {
        for (const auto& variable : script.variables) {
            if (!variable.external && variable.type == 2) {
                const auto* value = std::get_if<Word>(&vm.global(variable.storage));
                if (value && word_float(*value) == static_cast<float>(objective.id))
                    objective.script_variables.push_back(variable.name);
            }
        }
    }
    return {multiplayer, vm.steps() - initialization_steps, initialization_steps,
            std::move(host.objectives), std::move(host.calls)};
}

void write_objective_runs(std::ostream& out, const std::vector<ObjectiveRun>& runs) {
    out << "[\n";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (i) out << ",\n";
        const auto& run = runs[i];
        out << "{\"multiplayer\":" << (run.multiplayer ? "true" : "false")
            << ",\"steps\":" << run.steps << ",\"initialization_steps\":" << run.initialization_steps
            << ",\"callback_count\":" << run.calls.size() << ",\"objectives\":[";
        for (std::size_t j = 0; j < run.objectives.size(); ++j) {
            if (j) out << ',';
            const auto& objective = run.objectives[j];
            out << "{\"id\":" << objective.id << ",\"category\":" << objective.category << ",\"properties\":{";
            bool first = true;
            for (const auto& [name, values] : objective.properties) {
                if (!first) out << ',';
                first = false;
                quoted(out, name);
                out << ':';
                json_values(out, values);
            }
            out << "},\"script_variables\":[";
            for (std::size_t k = 0; k < objective.script_variables.size(); ++k) {
                if (k) out << ',';
                quoted(out, objective.script_variables[k]);
            }
            out << "]}";
        }
        out << "],\"calls\":[";
        for (std::size_t j = 0; j < run.calls.size(); ++j) {
            if (j) out << ',';
            out << "{\"name\":";
            quoted(out, run.calls[j].name);
            out << ",\"arguments\":";
            json_values(out, run.calls[j].arguments);
            out << '}';
        }
        out << "],\"limitations\":";
        quoted(out, "Native research prototype: recording objective callbacks and symbolic localization keys; no rendering, gameplay or networking.");
        out << '}';
    }
    out << "\n]\n";
    if (!out) throw std::runtime_error("Failed to write native objective report");
}

void write_objective_status_probe(std::ostream& out, const Script& script,
                                 const std::vector<bool>& modes) {
    std::vector<std::string> queries;
    for (const auto& function : script.functions) {
        if (!function.external && (function.name.starts_with("objectives::CheckObjective_")
                || function.name.starts_with("objectives::WasFailed_"))) {
            signature(script, function, "primitive:2", {});
            queries.push_back(function.name);
        }
    }
    if (queries.empty()) throw std::runtime_error("Script has no objective status queries");
    out << "{\"scope\":\"Diagnostic status fixtures; actual script queries, no gameplay transitions\",\"runs\":[";
    bool first_run = true;
    for (const bool multiplayer : modes) {
        ObjectiveHost host(multiplayer);
        Vm vm(script, host);
        vm.initialize();
        vm.invoke("objectives::CreateObjectives");
        if (!first_run) out << ',';
        first_run = false;
        out << "{\"multiplayer\":" << (multiplayer ? "true" : "false")
            << ",\"objective_count\":" << host.objectives.size() << ",\"scenarios\":[";
        bool first_scenario = true;
        // Isolate every objective in every state, with the others set to a
        // different state; this detects queries accidentally resolving another ID.
        for (Word selected = 0; selected <= host.objectives.size(); ++selected) {
            for (Word status = 0; status < (selected == 0 ? 1U : 3U); ++status) {
                if (selected != 0) for (const auto& objective : host.objectives)
                    host.set_diagnostic_status(objective.id,
                        objective.id == selected ? status : (status + 1) % 3);
                if (!first_scenario) out << ',';
                first_scenario = false;
                out << "{\"selected_id\":" << selected << ",\"selected_status\":" << status
                    << ",\"statuses\":[";
                for (std::size_t i = 0; i < host.objectives.size(); ++i) {
                    if (i) out << ',';
                    out << host.objectives[i].diagnostic_status;
                }
                out << "],\"queries\":[";
                bool first_query = true;
                for (const auto& name : queries) {
                    host.calls.clear();
                    const auto before = vm.steps();
                    const auto result = vm.invoke(name);
                    if (!result || !std::holds_alternative<Word>(*result)
                        || host.calls.size() != 3
                        || host.calls[0].name != "game::GetObjectiveManager"
                        || host.calls[1].name != "GetObjective"
                        || (host.calls[2].name != "IsComplete" && host.calls[2].name != "WasFailed"))
                        throw std::runtime_error("Status probe encountered an unsupported query shape");
                    const auto receiver = std::get<ObjectHandle>(host.calls[2].arguments.at(0));
                    if (!first_query) out << ',';
                    first_query = false;
                    out << "{\"name\":";
                    quoted(out, name);
                    out << ",\"value\":" << word_float(std::get<Word>(*result))
                        << ",\"steps\":" << vm.steps() - before << ",\"receiver_id\":";
                    if (receiver.id) out << receiver.id;
                    else out << "null";
                    out << ",\"skipped_native_getter\":" << (receiver.id ? "false" : "true") << '}';
                }
                out << "]}";
            }
        }
        out << "]}";
    }
    out << "]}\n";
    if (!out) throw std::runtime_error("Failed to write objective status probe");
}

}
