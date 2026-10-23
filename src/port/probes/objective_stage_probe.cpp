#include "objective_stage_probe.hpp"

#include "objective_stage.hpp"
#include "objective_outcome.hpp"
#include "objective_progression.hpp"
#include "objective_registration.hpp"
#include "objective_markers.hpp"
#include "localization.hpp"
#include "script_objects_runtime.hpp"
#include "voice_sound_bridge.hpp"
#include "objectives.hpp"
#include <array>
#include <stdexcept>

namespace omega {
namespace {
class StageHost final : public Host {
public:
    ObjectiveStage stage;
    ObjectiveHost configuration{false};
    const StringPool* pool{};
    const ObjectiveProgression* progression{};
    AgentObjectiveBits progression_bits;
    std::vector<Word> progression_indices;
    std::vector<ObjectiveOutcome> outcomes;
    std::vector<std::string> transition_order;
    struct VoiceRequest { std::string sample; std::int32_t priority; };
    std::vector<VoiceRequest> voice_requests;
    Vm* voice_vm{};
    VoiceSoundBridge* sound_bridge{};
    bool solo_end{};
    struct SoundRequest { std::int32_t bank; std::string key; Word object_id; };
    std::vector<SoundRequest> sound_requests;
    std::vector<std::string> skipped_null_calls;
    std::size_t level_end_requests{};
    RuntimeObjectRegistry* objects{};
    const Localization* localization{};
    ObjectiveMarkers markers;
    struct LabelRequest { std::string key, text; };
    struct Lookup { Word raw_id; RuntimeObjectHandle handle; };
    struct OwnerLookup { Word controller_id, owner_id; };
    struct MarkerEvent { MarkerAction action; ObjectiveMarker marker; };
    std::vector<LabelRequest> label_requests;
    std::vector<Lookup> object_lookups;
    std::vector<OwnerLookup> owner_lookups;
    std::vector<MarkerEvent> marker_events;
    Value object_from_id(Word raw_id) override {
        if (!objects) throw std::runtime_error("This diagnostic has no runtime object registry");
        const auto handle = objects->resolve(raw_id);
        object_lookups.push_back({raw_id, handle});
        transition_order.push_back("lookup");
        return handle;
    }
    Word enum_value(const std::string& name, Word member) override {
        if (sound_bridge && name == "SFX_ACTION") return sound_bridge->action_member(member);
        if (voice_vm && name == "GameSound_SndBank") {
            // Registration table at 0048a340 uses 12-byte descriptors.
            constexpr std::array<Word,7> values{0,1,2,3,5,4,0xffffffffU};
            if (member >= values.size()) throw std::runtime_error("Unknown sound-bank member");
            return values[member];
        }
        return configuration.enum_value(name, member);
    }
    std::optional<Value> call(const Script& script, const Function& function,
                             const std::vector<Value>& args) override {
        const auto& name = function.name;
        if (name == "voice::PlaySound" && sound_bridge && voice_vm && objects) {
            if (args.size() != 3) throw std::runtime_error("Invalid sound bridge argument count");
            const auto* actor = std::get_if<RuntimeObjectHandle>(&args[1]);
            if (!actor || actor->kind != RuntimeObjectHandle::Kind::world_object || !objects->alive(*actor))
                throw std::runtime_error("Sound bridge requires a live world object");
            return sound_bridge->invoke(*voice_vm, script, function, args);
        }
        if (name == "game::StopSnd" && solo_end && objects) {
            check(script, function, args, {"enum:GameSound_SndBank", "primitive:3", "object:GOBJ"});
            const auto* key = std::get_if<std::string>(&args[1]);
            const auto* actor = std::get_if<RuntimeObjectHandle>(&args[2]);
            if (!function.external || !key || !actor || actor->kind != RuntimeObjectHandle::Kind::world_object
                || !objects->alive(*actor)) throw std::runtime_error("Invalid sound-stop request");
            // The wrapper reads the declared bank as a signed byte. Record the
            // request only; original event dispatch/audio lifetime are separate.
            const Word byte = word(args[0]) & 0xffU;
            const auto bank = byte < 128 ? static_cast<std::int32_t>(byte) : static_cast<std::int32_t>(byte) - 256;
            sound_requests.push_back({bank, *key, actor->id});
            transition_order.push_back("sound_stop");
            return std::nullopt;
        }
        if (name == "game::RequestLevelEnd" && solo_end) {
            check(script, function, args, {});
            if (!function.external) throw std::runtime_error("Expected external level-end request");
            ++level_end_requests;
            transition_order.push_back("request_level_end");
            return std::nullopt;
        }
        if (name == "GetGOBJ" && objects) {
            if (!function.external || args.size() != 1 || function.parameters.size() != 1
                || script.type_key(function.return_type) != "object:GOBJ"
                || script.type_key(function.parameters[0]) != "object:AI")
                throw std::runtime_error("Unsupported AI owner lookup signature");
            const auto* handle = std::get_if<RuntimeObjectHandle>(&args[0]);
            if (!handle || handle->kind != RuntimeObjectHandle::Kind::ai)
                throw std::runtime_error("AI owner lookup requires an AI controller handle");
            const auto owner = objects->get_gobj(*handle);
            owner_lookups.push_back({handle->id, owner.id});
            transition_order.push_back("owner");
            return owner;
        }
        if (name == "SetDisplayLabel" && localization) {
            check(script, function, args, {"object:ObjectiveMan", "primitive:7"});
            manager(args[0]);
            const auto* key = std::get_if<std::string>(&args[1]);
            if (!key) throw std::runtime_error("Display label requires a string key");
            const auto text = localization->resolve(*key);
            if (text) {
                label_requests.push_back({*key, std::string(*text)});
                transition_order.push_back("display_label");
            }
            return std::nullopt; // Localized notification request, no HUD delivery.
        }
        if ((name == "game::AddHudObjective" || name == "game::RemoveHudObjective") && objects && localization) {
            const bool add = name == "game::AddHudObjective";
            check(script, function, args, add ?
                std::vector<std::string>{"object:GOBJ", "primitive:3", "primitive:3"} :
                std::vector<std::string>{"object:GOBJ"});
            const auto* handle = std::get_if<RuntimeObjectHandle>(&args[0]);
            if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects->alive(*handle))
                throw std::runtime_error("Marker binding requires a live world object");
            if (add) {
                const auto* key = std::get_if<std::string>(&args[1]);
                const auto* cue = std::get_if<std::string>(&args[2]);
                if (!key || !cue) throw std::runtime_error("Marker binding requires owned text arguments");
                markers.add(handle->id, localization->resolve(*key), *cue);
            } else markers.remove(handle->id);
            return std::nullopt;
        }
        if (name == "voice::PlaySample" && voice_vm) {
            check(script, function, args, {"primitive:3"});
            if (!function.external || !std::holds_alternative<std::string>(args[0])
                || function.dispatch_fields != std::array<Word,5>{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0})
                throw std::runtime_error("Unsupported voice import or string argument");
            return voice_vm->invoke(name, args);
        }
        if (name == "game::PlayQuip" && voice_vm) {
            check(script, function, args, {"primitive:3", "primitive:1"});
            const auto* sample = std::get_if<std::string>(&args[0]);
            if (!sample || !function.external) throw std::runtime_error("Unsupported voice request");
            voice_requests.push_back({*sample, word_signed(word(args[1]))});
            transition_order.push_back("voice");
            return std::nullopt;
        }
        if (name == "game::AddObjective" && pool && progression) {
            check(script, function, args, {"primitive:7"});
            const auto* key = std::get_if<std::string>(&args[0]);
            if (!key) throw std::runtime_error("Completion probe expected a progression key");
            const auto intern_id = pool->lookup(*key);
            progression_indices.push_back(progression->lookup(intern_id));
            progression_bits.add(*progression, intern_id);
            transition_order.push_back("progression");
            return std::nullopt;
        }
        if ((name == "Succeed" || name == "Fail") && progression) {
            check(script, function, args, {"object:ObjectiveMan", "object:Objective"});
            manager(args[0]);
            const auto* handle = std::get_if<ObjectHandle>(&args[1]);
            if (!handle || handle->kind != ObjectHandle::Kind::objective)
                throw std::runtime_error("Completion probe requires a typed objective");
            // 003ddfc0 suppresses the original native call for a null declared
            // object argument. Admit only this proved two-argument Fail boundary.
            if (solo_end && function.external && name == "Fail" && handle->id == 0) {
                skipped_null_calls.push_back(name);
                transition_order.push_back("skip_null:Fail");
                return std::nullopt;
            }
            if (!stage.find(handle->id))
                throw std::runtime_error("Completion probe requires a valid nonnull objective");
            const auto& properties = configuration.objectives.at(handle->id - 1).properties;
            const auto found = properties.find(name == "Succeed" ? "SetCompleteString" : "SetFailString");
            std::optional<std::string> text;
            if (found != properties.end()) {
                if (found->second.size() != 1 || !std::holds_alternative<std::string>(found->second[0]))
                    throw std::runtime_error("Completion probe requires symbolic objective text");
                text = std::get<std::string>(found->second[0]);
            }
            // Two-argument original wrappers supply a null optional actor.
            // Cue 123 is explicit diagnostic context; requests remain undelivered.
            outcomes.push_back(apply_objective_outcome(stage, handle->id, name == "Succeed", text, {123,false}));
            transition_order.push_back(name == "Succeed" ? "succeed" : "fail");
            return std::nullopt;
        }
        if ((name == "IsComplete" || name == "WasFailed") && progression) {
            if (function.parameters.size() != 1 || args.size() != 1
                || script.type_key(function.return_type) != "primitive:1"
                || script.type_key(function.parameters[0]) != "object:Objective")
                throw std::runtime_error("Unsupported completion query signature");
            const auto* handle = std::get_if<ObjectHandle>(&args[0]);
            if (!handle || handle->kind != ObjectHandle::Kind::objective)
                throw std::runtime_error("Completion query requires typed objective");
            if (!handle->id) return Word{0};
            const auto* object = stage.find(handle->id);
            if (!object) throw std::runtime_error("Completion query has invalid nonnull receiver");
            return Word(object->status == (name == "IsComplete" ? 1 : 2));
        }
        if (name == "StartStage") {
            check(script, function, args, {"object:ObjectiveMan", "primitive:1"});
            manager(args[0]);
            stage.start_stage(word_signed(word(args[1])));
            return std::nullopt;
        }
        if (name == "Activate" || name == "Deactivate") {
            check(script, function, args, {"object:Objective"});
            const auto* handle = std::get_if<ObjectHandle>(&args[0]);
            if (!handle || handle->kind != ObjectHandle::Kind::objective)
                throw std::runtime_error("Stage probe expected typed objective receiver");
            // Interpreter's typed-null skip is a no-op; not a native dereference.
            if (handle->id != 0) stage.set_active(handle->id, name == "Activate");
            return std::nullopt;
        }
        if (name == "AddObjective") {
            if (args.size() != 2 || function.parameters.size() != 2
                || script.type_key(function.return_type) != "primitive:1"
                || script.type_key(function.parameters[0]) != "object:ObjectiveMan"
                || script.type_key(function.parameters[1]) != "primitive:1")
                throw std::runtime_error("Stage probe expected manager AddObjective(int)");
            manager(args[0]);
            const auto id = stage.add(word_signed(word(args[1])));
            if (!id) return Word{0};
            // Creation succeeds in both hosts with the constructor-proven ID 1.
            const auto result = configuration.call(script, function, args);
            if (!result || !std::holds_alternative<Word>(*result)
                || std::get<Word>(*result) != id)
                throw std::runtime_error("Stage/configuration objective identities diverged");
            return result;
        }
        return configuration.call(script, function, args);
    }
private:
    static Word word(const Value& value) {
        if (const auto* result = std::get_if<Word>(&value)) return *result;
        throw std::runtime_error("Stage probe expected integer word");
    }
    static void manager(const Value& value) {
        const auto* h = std::get_if<ObjectHandle>(&value);
        if (!h || h->kind != ObjectHandle::Kind::objective_manager || h->id != 0)
            throw std::runtime_error("Stage probe expected manager receiver");
    }
    static void check(const Script& script, const Function& function,
                      const std::vector<Value>& args, const std::vector<std::string>& types) {
        if (script.type_key(function.return_type) != "primitive:4"
            || args.size() != types.size() || function.parameters.size() != types.size())
            throw std::runtime_error("Unsupported stage callback signature");
        for (std::size_t i = 0; i < types.size(); ++i)
            if (script.type_key(function.parameters[i]) != types[i])
                throw std::runtime_error("Unsupported stage callback parameter");
    }
};

void write_state(std::ostream& out, const ObjectiveStage& stage) {
    out << "{\"current_stage\":" << stage.current_stage()
        << ",\"next_id\":" << stage.next_id() << ",\"records\":[";
    bool first = true;
    for (const auto& object : stage.records()) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << object.id << ",\"stage\":" << object.stage
            << ",\"active\":" << (object.active ? "true" : "false")
            << ",\"status\":" << static_cast<unsigned>(object.status) << '}';
    }
    out << "]}";
}
}

void write_objective_outcome_probe(std::ostream& out, const Script& script) {
    struct Scenario { const char* name; bool activate; bool retire; std::int32_t cue; bool actor; };
    constexpr std::array scenarios{
        Scenario{"startup_active_only", false, false, 123, true},
        Scenario{"all_active", true, false, 123, true},
        Scenario{"retired_stage", false, true, 123, true},
        Scenario{"silent_context", true, false, 0, true},
        Scenario{"negative_context", true, false, -1, true},
        Scenario{"no_actor", true, false, 123, false}};
    out << "{\"scope\":\"Solo outcome state and ordered effect requests; diagnostic operations after real startup, no gameplay triggers or effect delivery\",\"scenarios\":[";
    bool first_scenario = true;
    for (const auto& scenario : scenarios) {
        StageHost host;
        Vm vm(script, host);
        vm.initialize();
        const auto initialization = vm.steps();
        vm.invoke("objectives::StartUp");
        const auto startup = vm.steps() - initialization;
        if (host.stage.records().size() != 7 || host.configuration.objectives.size() != 7)
            throw std::runtime_error("Outcome probe requires seven solo Carthage objectives");
        if (scenario.activate)
            for (std::uint32_t id = 1; id <= 7; ++id) host.stage.set_active(id, true);
        if (scenario.retire) host.stage.start_stage(2);
        if (!first_scenario) out << ',';
        first_scenario = false;
        out << "{\"name\":\"" << scenario.name << "\",\"mission_cue\":" << scenario.cue
            << ",\"eligible_actor\":" << (scenario.actor ? "true" : "false")
            << ",\"initialization_steps\":" << initialization << ",\"startup_steps\":" << startup
            << ",\"initial\":";
        write_state(out, host.stage);
        out << ",\"steps\":[";
        constexpr std::array<std::pair<std::uint32_t, bool>, 10> operations{{
            {1,true},{2,true},{3,true},{4,true},{5,true},{6,true},{7,true},{7,true},{4,false},{4,true}}};
        bool first_step = true;
        for (const auto& [id, success] : operations) {
            const auto& properties = host.configuration.objectives[id - 1].properties;
            const auto found = properties.find(success ? "SetCompleteString" : "SetFailString");
            std::optional<std::string> text;
            if (found != properties.end()) {
                if (found->second.size() != 1 || !std::holds_alternative<std::string>(found->second[0]))
                    throw std::runtime_error("Outcome probe requires symbolic objective text");
                text = std::get<std::string>(found->second[0]);
            }
            const auto result = apply_objective_outcome(host.stage, id, success, text,
                {scenario.cue, scenario.actor});
            if (!first_step) out << ',';
            first_step = false;
            out << "{\"id\":" << id << ",\"success\":" << (success ? "true" : "false")
                << ",\"outcome\":";
            write_objective_outcome(out, result);
            out << ",\"state\":";
            write_state(out, host.stage);
            out << '}';
        }
        out << "]}";
    }
    out << "]}\n";
}

void write_objective_stage_probe(std::ostream& out, const Script& script) {
    StageHost host;
    Vm vm(script, host);
    vm.initialize();
    out << "{\"scope\":\"Solo objective activation and stages; configuration remains symbolic\","
        << "\"initialization_steps\":" << vm.steps() << ",\"phases\":[";
    constexpr std::array entries{"objectives::StartUp", "objectives::AddObjective_Scan01",
        "objectives::StartStage2", "objectives::AddObjective_Scan01", "objectives::StartStage2"};
    bool first = true;
    for (const auto* entry : entries) {
        const auto before = vm.steps();
        vm.invoke(entry);
        if (!first) out << ',';
        first = false;
        out << "{\"entry\":\"" << entry << "\",\"steps\":" << vm.steps() - before
            << ",\"current_stage\":" << host.stage.current_stage()
            << ",\"next_id\":" << host.stage.next_id() << ",\"records\":[";
        bool first_record = true;
        for (const auto& object : host.stage.records()) {
            if (!first_record) out << ',';
            first_record = false;
            out << "{\"id\":" << object.id << ",\"stage\":" << object.stage
                << ",\"active\":" << (object.active ? "true" : "false")
                << ",\"status\":" << static_cast<unsigned>(object.status) << '}';
        }
        out << "]}";
    }
    out << "]}\n";
}

void write_objective_completion_probe(std::ostream& out, const Script& script,
        const Script& awards, const StringPool& pool) {
    ObjectiveProgression progression;
    for (const auto& row : read_objective_registration_prefix(awards).rows) {
        const auto intern_id = pool.lookup(row.key);
        if (intern_id < 0) throw std::runtime_error("Completion probe requires registered base-pool keys");
        progression.register_objective(intern_id, row.points, row.solo_required, row.multi_required);
    }
    StageHost host;
    host.pool = &pool;
    host.progression = &progression;
    Vm vm(script, host);
    vm.initialize();
    const auto initialization = vm.steps();
    vm.invoke("objectives::StartUp");
    const auto startup = vm.steps() - initialization;
    out << "{\"scope\":\"Real solo CHA and collateral script transitions; diagnostic invocations, symbolic text and undelivered effects\","
        << "\"mission_cue\":123,\"initialization_steps\":" << initialization
        << ",\"startup_steps\":" << startup << ",\"initial\":";
    write_state(out, host.stage);
    out << ",\"phases\":[";
    constexpr std::array entries{"objectives::PassObjective_CHA", "objectives::PassObjective_CHA",
        "objectives::FailObjective_CHA", "objectives::PassObjective_CHA",
        "objectives::FailObjective_CollateralDamage", "objectives::PassObjective_CollateralDamage",
        "objectives::PassObjective_CollateralDamage", "objectives::FailObjective_CollateralDamage"};
    bool first = true;
    for (const auto* entry : entries) {
        host.outcomes.clear();
        host.progression_indices.clear();
        host.transition_order.clear();
        const auto before = vm.steps();
        vm.invoke(entry);
        if (!first) out << ',';
        first = false;
        out << "{\"entry\":\"" << entry << "\",\"steps\":" << vm.steps() - before
            << ",\"transition_order\":[";
        for (std::size_t i = 0; i < host.transition_order.size(); ++i) {
            if (i) out << ',';
            out << '"' << host.transition_order[i] << '"';
        }
        out << "],\"progression_indices\":[";
        for (std::size_t i = 0; i < host.progression_indices.size(); ++i) {
            if (i) out << ',';
            out << host.progression_indices[i];
        }
        out << "],\"outcomes\":[";
        for (std::size_t i = 0; i < host.outcomes.size(); ++i) {
            if (i) out << ',';
            write_objective_outcome(out, host.outcomes[i]);
        }
        out << "],\"state\":";
        write_state(out, host.stage);
        auto bits = [&](const ObjectiveBits& values) {
            out << '[';
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) out << ',';
                out << values[i];
            }
            out << ']';
        };
        out << ",\"persistent\":"; bits(host.progression_bits.persistent());
        out << ",\"mission\":"; bits(host.progression_bits.mission());
        out << '}';
    }
    out << "]}\n";
}

void write_objective_voice_probe(std::ostream& out, const Script& script,
        const Script& awards, const StringPool& pool, const Script& voice) {
    // Explicitly bind just this import. This is not a general module linker:
    // external globals, object receivers and arbitrary overloads remain rejected.
    std::size_t exports = 0;
    for (const auto& function : voice.functions) {
        if (function.external || function.name != "voice::PlaySample") continue;
        if (function.return_type != 4 || function.parameters != std::vector<Word>{3}
            || function.dispatch_fields != std::array<Word,5>{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0})
            throw std::runtime_error("Unsupported voice export signature");
        ++exports;
    }
    if (exports != 1) throw std::runtime_error("Expected one local voice::PlaySample export");
    ObjectiveProgression progression;
    for (const auto& row : read_objective_registration_prefix(awards).rows) {
        const auto id = pool.lookup(row.key);
        if (id < 0) throw std::runtime_error("Voice probe requires registered base-pool keys");
        progression.register_objective(id, row.points, row.solo_required, row.multi_required);
    }
    auto text = [&](const std::string& value) {
        static constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
            else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else out << static_cast<char>(c);
        }
        out << '"';
    };
    struct Scenario { const char* name; const char* entry; bool repeat; std::vector<std::int32_t> deltas; };
    const std::array scenarios{
        Scenario{"equipment", "objectives::PassObjective_Equipment", true, {}},
        Scenario{"samples", "objectives::PassObjective_Samples", false, {2999,1}},
        Scenario{"autopsy", "objectives::PassObjective_Autopsy", false, {9999,1,0}}};
    out << "{\"scope\":\"Solo equipment, samples and autopsy scripts with explicit voice bridge; diagnostic triggers and undelivered audio\",\"mission_cue\":123,\"scenarios\":[";
    bool first_scenario = true;
    for (const auto& scenario : scenarios) {
        StageHost host;
        host.pool = &pool;
        host.progression = &progression;
        Vm voice_vm(voice, host);
        host.voice_vm = &voice_vm;
        voice_vm.initialize();
        Vm vm(script, host);
        vm.initialize();
        const auto initialization = vm.steps();
        vm.invoke("objectives::StartUp");
        const auto startup = vm.steps() - initialization;
        if (!first_scenario) out << ',';
        first_scenario = false;
        out << "{\"name\":"; text(scenario.name);
        out << ",\"initialization_steps\":" << initialization << ",\"startup_steps\":" << startup
            << ",\"voice_initialization_steps\":" << voice_vm.steps() << ",\"initial\":";
        write_state(out, host.stage);
        out << ",\"phases\":[";
        auto phase = [&](const char* entry, std::int32_t delta) {
            host.outcomes.clear(); host.progression_indices.clear();
            host.transition_order.clear(); host.voice_requests.clear();
            const auto before = vm.steps(), voice_before = voice_vm.steps();
            out << '{';
            if (entry) {
                vm.invoke(entry);
                out << "\"entry\":"; text(entry);
            } else {
                const auto executed = vm.schedules().advance(delta);
                out << "\"delta\":" << delta << ",\"executed\":" << executed;
            }
            out << ",\"steps\":" << vm.steps() - before << ",\"voice_steps\":" << voice_vm.steps() - voice_before
                << ",\"transition_order\":[";
            for (std::size_t i=0; i<host.transition_order.size(); ++i) {
                if (i) out << ',';
                text(host.transition_order[i]);
            }
            out << "],\"progression_indices\":[";
            for (std::size_t i=0; i<host.progression_indices.size(); ++i) {
                if (i) out << ',';
                out << host.progression_indices[i];
            }
            out << "],\"outcomes\":[";
            for (std::size_t i=0; i<host.outcomes.size(); ++i) {
                if (i) out << ',';
                write_objective_outcome(out, host.outcomes[i]);
            }
            out << "],\"voice_requests\":[";
            for (std::size_t i=0; i<host.voice_requests.size(); ++i) {
                if (i) out << ',';
                out << "{\"sample\":"; text(host.voice_requests[i].sample);
                out << ",\"priority\":" << host.voice_requests[i].priority << '}';
            }
            out << "],\"state\":"; write_state(out, host.stage);
            auto bits = [&](const ObjectiveBits& values) {
                out << '[';
                for (std::size_t i=0; i<values.size(); ++i) {
                    if (i) out << ',';
                    out << values[i];
                }
                out << ']';
            };
            out << ",\"persistent\":"; bits(host.progression_bits.persistent());
            out << ",\"mission\":"; bits(host.progression_bits.mission());
            out << ",\"pending\":[";
            const auto pending = vm.schedules().snapshot();
            for (std::size_t i=0; i<pending.size(); ++i) {
                if (i) out << ',';
                out << "{\"handle\":" << pending[i].handle << ",\"remaining_ticks\":" << pending[i].remaining_ticks << '}';
            }
            out << "]}";
        };
        phase(scenario.entry, 0);
        if (scenario.repeat) { out << ','; phase(scenario.entry, 0); }
        for (const auto delta : scenario.deltas) { out << ','; phase(nullptr, delta); }
        out << "]}";
    }
    out << "]}\n";
}

static void write_population_objective_probe(std::ostream& out, const Script& script,
        const Script& awards, const StringPool& base_pool, const StringPool& mission_pool,
        const Script& voice, const Population& population, bool solo_end) {
    std::size_t exports = 0;
    for (const auto& f : voice.functions) {
        if (f.external || f.name != "voice::PlaySample") continue;
        if (f.return_type != 4 || f.parameters != std::vector<Word>{3}
            || f.dispatch_fields != std::array<Word,5>{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0})
            throw std::runtime_error("Unsupported Proust voice export");
        ++exports;
    }
    if (exports != 1) throw std::runtime_error("Expected one Proust voice export");
    if (population.npcs.empty()) throw std::runtime_error("Proust probe requires population NPC records");
    ObjectiveProgression progression;
    for (const auto& row : read_objective_registration_prefix(awards).rows) {
        const auto id = base_pool.lookup(row.key);
        if (id < 0) throw std::runtime_error("Proust probe requires base-pool progression keys");
        progression.register_objective(id, row.points, row.solo_required, row.multi_required);
    }
    Localization localization(base_pool);
    localization.replace_mission(mission_pool);
    RuntimeObjectRegistry objects;
    // Explicit diagnostic scope, not a reconstruction of all mission containers.
    objects.add_scope(1, std::nullopt);
    objects.set_current_scope(1);
    for (std::size_t i = 0; i < population.npcs.size(); ++i) {
        const auto id = population.npcs[i].id;
        objects.register_npc(id, 1);
        objects.attach(id, i ? RuntimeObjectHandle::Kind::ai : RuntimeObjectHandle::Kind::player);
    }
    std::size_t gob_count = 0;
    if (solo_end) {
        for (const auto& object : population.objects) {
            if (object.kind != 1 && object.kind != 4 && object.kind != 5) continue;
            // These constructors initially project to self; no attachments or
            // physical behavior are claimed for the diagnostic world identities.
            objects.register_world_object(object.id, 1);
            ++gob_count;
        }
        out << "{\"scope\":\"Solo Carthage Scan success and failure through the complete mission-end script chain; diagnostic triggers and undelivered presentation\","
            << "\"identity_context\":\"One explicit diagnostic scope; population NPC controllers and unattached kind-1/4/5 world identities\","
            << "\"mission_cue\":123,\"npc_count\":" << population.npcs.size() << ",\"gob_count\":" << gob_count
            << ",\"base_strings\":" << localization.base_count() << ",\"mission_strings\":" << localization.mission_count()
            << ",\"scenarios\":[";
    }
    std::optional<VoiceSoundBridge> bridge;
    if (solo_end) bridge.emplace(script, voice);
    constexpr std::array scenario_names{"scan_pass_split", "scan_fail_split", "scan_pass_large", "scan_fail_large"};
    for (std::size_t scenario = 0; scenario < (solo_end ? scenario_names.size() : 1); ++scenario) {
        if (scenario) out << ',';
        StageHost host;
        host.pool = &base_pool; host.progression = &progression;
        host.objects = &objects; host.localization = &localization;
        host.solo_end = solo_end; host.sound_bridge = bridge ? &*bridge : nullptr;
        host.markers.observe([&](MarkerAction action, const ObjectiveMarker& marker) {
            host.marker_events.push_back({action, marker});
            host.transition_order.push_back(action == MarkerAction::added ? "marker_added" : "marker_removed");
        });
        Vm voice_vm(voice, host); host.voice_vm = &voice_vm;
        voice_vm.initialize();
        const auto voice_init = voice_vm.steps();
        Vm vm(script, host); vm.initialize();
        const auto initialization = vm.steps();
        vm.invoke("objectives::StartUp");
        const auto startup = vm.steps() - initialization;
        auto text = [&](std::string_view value) {
            static constexpr char hex[] = "0123456789abcdef";
            out << '"';
            for (const unsigned char c : value) {
                if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
                else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
                else out << static_cast<char>(c);
            }
            out << '"';
        };
        auto hex_text = [&](std::string_view value) {
            static constexpr char digits[] = "0123456789abcdef";
            out << '"';
            for (const unsigned char c : value) out << digits[c >> 4] << digits[c & 15];
            out << '"';
        };
        auto list = [&](const auto& values, auto write) {
            out << '[';
            bool first = true;
            for (const auto& value : values) {
                if (!first) out << ',';
                first = false; write(value);
            }
            out << ']';
        };
        auto marker_fields = [&](const ObjectiveMarker& marker) {
            out << "\"object_id\":" << marker.object_id << ",\"kind\":" << marker.kind << ",\"label_hex\":";
            hex_text(marker.label); out << ",\"cue_hex\":"; hex_text(marker.cue);
        };
        if (solo_end) { out << "{\"name\":"; text(scenario_names[scenario]); }
        else out << "{\"scope\":\"Solo Proust activation and completion through population identities and localized logical markers; diagnostic triggers and undelivered presentation\","
            << "\"identity_context\":\"One explicit diagnostic scope; population NPC identities with loader-selected controller roles\","
            << "\"mission_cue\":123,\"npc_count\":" << population.npcs.size()
            << ",\"base_strings\":" << localization.base_count() << ",\"mission_strings\":" << localization.mission_count();
        out << ",\"initialization_steps\":" << initialization << ",\"startup_steps\":" << startup
            << ",\"voice_initialization_steps\":" << voice_init << ",\"initial\":";
        write_state(out, host.stage);
        out << ",\"phases\":[";
        bool first_phase = true;
        auto phase = [&](const char* entry, std::int32_t delta) {
            host.outcomes.clear(); host.progression_indices.clear(); host.transition_order.clear();
            host.voice_requests.clear(); host.label_requests.clear(); host.marker_events.clear();
            host.object_lookups.clear(); host.owner_lookups.clear();
            host.sound_requests.clear(); host.skipped_null_calls.clear(); host.level_end_requests = 0;
            const auto before = vm.steps(), voice_before = voice_vm.steps();
            if (!first_phase) out << ',';
            first_phase = false; out << '{';
            if (entry) { vm.invoke(entry); out << "\"entry\":"; text(entry); }
            else out << "\"delta\":" << delta << ",\"executed\":" << vm.schedules().advance(delta);
            out << ",\"steps\":" << vm.steps()-before << ",\"voice_steps\":" << voice_vm.steps()-voice_before;
            out << ",\"transition_order\":"; list(host.transition_order, text);
            out << ",\"progression_indices\":"; list(host.progression_indices, [&](auto id) { out << id; });
            out << ",\"outcomes\":"; list(host.outcomes, [&](const auto& value) { write_objective_outcome(out, value); });
            out << ",\"voice_requests\":"; list(host.voice_requests, [&](const auto& value) {
                out << "{\"sample\":"; text(value.sample); out << ",\"priority\":" << value.priority << '}';
            });
            out << ",\"label_requests\":"; list(host.label_requests, [&](const auto& value) {
                out << "{\"key\":"; text(value.key); out << ",\"text_hex\":"; hex_text(value.text); out << '}';
            });
            out << ",\"marker_events\":"; list(host.marker_events, [&](const auto& value) {
                out << "{\"action\":"; text(value.action == MarkerAction::added ? "added" : "removed");
                out << ','; marker_fields(value.marker); out << '}';
            });
            out << ",\"markers\":"; list(host.markers.records(), [&](const auto& value) { out << '{'; marker_fields(value); out << '}'; });
            out << ",\"object_lookups\":"; list(host.object_lookups, [&](const auto& value) {
                out << "{\"raw_id\":" << value.raw_id << ",\"kind\":";
                text(value.handle.kind == RuntimeObjectHandle::Kind::ai ? "ai" :
                    value.handle.kind == RuntimeObjectHandle::Kind::player ? "player" : "world_object");
                out << ",\"id\":" << value.handle.id << '}';
            });
            out << ",\"owner_lookups\":"; list(host.owner_lookups, [&](const auto& value) {
                out << "{\"controller_id\":" << value.controller_id << ",\"owner_id\":" << value.owner_id << '}';
            });
            if (solo_end) {
                out << ",\"sound_requests\":"; list(host.sound_requests, [&](const auto& value) {
                    out << "{\"bank\":" << value.bank << ",\"key\":"; text(value.key);
                    out << ",\"object_id\":" << value.object_id << '}';
                });
                out << ",\"skipped_null_calls\":"; list(host.skipped_null_calls, text);
                out << ",\"level_end_requests\":" << host.level_end_requests;
            }
            out << ",\"state\":"; write_state(out, host.stage);
            out << ",\"persistent\":"; list(host.progression_bits.persistent(), [&](auto value) { out << value; });
            out << ",\"mission\":"; list(host.progression_bits.mission(), [&](auto value) { out << value; });
            out << ",\"pending\":"; list(vm.schedules().snapshot(), [&](const auto& value) {
                out << "{\"handle\":" << value.handle << ",\"remaining_ticks\":" << value.remaining_ticks << '}';
            });
            out << '}';
        };
        phase("objectives::PassObjective_Samples", 0);
        for (auto delta : {2999,1,1999,1}) phase(nullptr, delta);
        if (solo_end) {
            phase("objectives::AddObjective_Autopsy", 0);
            phase("objectives::AddObjective_Scan", 0);
            phase("objectives::PassObjective_Autopsy", 0);
            phase(scenario % 2 ? "objectives::FailObjective_Scan" : "objectives::PassObjective_Scan", 0);
        } else phase("objectives::AddObjective_Proust", 0);
        phase("objectives::PassObjective_Proust", 0);
        for (auto delta : {1999,1}) phase(nullptr, delta);
        if (solo_end) {
            if (scenario < 2) for (auto delta : {1999,1,5999,1,999,1,999,1}) phase(nullptr, delta);
            else phase(nullptr, 10000);
        } else phase("objectives::AddObjective_Proust", 0);
        out << "]}\n";
    }
    if (solo_end) out << "]}\n";
}

void write_objective_proust_probe(std::ostream& out, const Script& script,
        const Script& awards, const StringPool& base_pool, const StringPool& mission_pool,
        const Script& voice, const Population& population) {
    write_population_objective_probe(out, script, awards, base_pool, mission_pool, voice, population, false);
}

void write_objective_solo_end_probe(std::ostream& out, const Script& script,
        const Script& awards, const StringPool& base_pool, const StringPool& mission_pool,
        const Script& voice, const Population& population) {
    write_population_objective_probe(out, script, awards, base_pool, mission_pool, voice, population, true);
}

}
