#include "script_start_probe.hpp"

#include "engine_globals.hpp"
#include "mission_script_host.hpp"
#include "script_context.hpp"

#include <utility>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {

std::string quoted(const std::string& text) { return quoted_json(text); }
std::string describe(const Value& value) { return describe_script_value(value); }

// Objective callbacks go to ObjectiveNatives (solo), whose signatures and
// semantics were checked against the original callbacks. Other void natives
// are recorded; any other value-returning native is refused, so the run stops
// at the first native S1 still has to host instead of inventing values.
struct Recorder final : Host {
    explicit Recorder(const Population& population) : world(population) {}
    struct Call {
        std::string stage;
        std::string signature;
        std::vector<std::string> arguments;
        std::optional<std::string> result;
        bool hosted{};
        bool answered{};
    };
    std::string stage;
    std::vector<Call> calls;
    // Diagnostic answer table (S2 scenario only): an unhosted value-returning
    // native answers 0, as t_orig_s2.py's recorders do. A 0 object is null,
    // so the interpreter then skips natives that take it.
    bool answer_table{};
    ObjectiveNatives objectives;
    PopulationObjects world;

    std::optional<Value> call(const Script& script, const Function& function,
                              const std::vector<Value>& arguments) override {
        const auto text = script_native_signature(script, function);
        const bool hosted = ObjectiveNatives::hosts(script, function);
        Call record{stage, text, {}, {}, hosted};
        for (const auto& argument : arguments) record.arguments.push_back(describe(argument));
        std::optional<Value> result;
        if (!hosted && function.return_type != 4) {
            if (!answer_table) throw std::runtime_error("Unhosted non-void native " + text);
            // t_orig_s2.py gives this one a fresh dummy Hotbox; a null would diverge.
            if (function.name == "game::MakeNPCInteractable")
                throw std::runtime_error("Answer table has no dummy object for " + text);
            result = Word{0};
            record.result = describe(*result);
            record.answered = true;
        }
        if (hosted) {
            result = objectives.call(script, function, arguments);
            if (result) record.result = describe(*result);
        }
        calls.push_back(std::move(record));
        return result;
    }
    Word enum_value(const std::string& name, Word) override {
        throw std::runtime_error("External enum table not hosted: " + name);
    }

    struct Lookup {
        std::string stage;
        Word raw_id{};
        bool resolved{};
    };
    std::vector<Lookup> lookups;
    Value object_from_id(Word raw_id) override {
        const auto handle = world.objects.resolve(raw_id);
        lookups.push_back({stage, raw_id, handle.generation != 0});
        return handle;
    }
    Word object_to_id(const Value& object) override {
        const auto* handle = std::get_if<RuntimeObjectHandle>(&object);
        if (!handle) throw std::runtime_error("Object ID of a non-runtime object");
        return world.objects.id_word(*handle);
    }
};


const char* selector_name(ScriptContext::Selector selector) {
    switch (selector) {
    case ScriptContext::Selector::literal: return "literal";
    case ScriptContext::Selector::variable: return "variable";
    case ScriptContext::Selector::group: return "group";
    case ScriptContext::Selector::generic: return "generic";
    }
    return "?";
}

// The handler map, and for every registered record which handlers 003e0510
// would select in each pass (selection only; no handler runs).
void write_handler_map(std::ostream& out, ScriptContext& context) {
    const auto& map = context.handlers();
    std::map<std::string, std::size_t> modules;
    out << "{\"messages\": [\n";
    bool first = true;
    for (const auto& [key, list] : map) {
        std::map<std::string, std::size_t> passes[2];
        for (const auto& handler : list) {
            ++modules[handler.module];
            if (handler.pass > 1) throw std::runtime_error("Handler pass outside 0/1: " + handler.name);
            ++passes[handler.pass][selector_name(handler.selector)];
        }
        out << (first ? "" : ",\n") << "      {\"key\": " << quoted(key) << ", \"handlers\": " << list.size();
        for (int pass = 0; pass < 2; ++pass) {
            out << ", \"pass" << pass << "\": {";
            bool inner = true;
            for (const auto& [name, count] : passes[pass]) {
                out << (inner ? "" : ", ") << quoted(name) << ": " << count;
                inner = false;
            }
            out << '}';
        }
        out << '}';
        first = false;
    }
    out << "\n    ], \"modules\": {";
    first = true;
    for (const auto& [name, count] : modules) {
        out << (first ? "" : ", ") << quoted(name) << ": " << count;
        first = false;
    }
    out << "}}";
}

void write_coverage(std::ostream& out, ScriptContext& context, Recorder& host) {
    std::vector<std::pair<std::string, ScriptContext::Receiver>> receivers;
    for (const auto& entry : host.world.sections) receivers.push_back(host.world.receiver(entry.first));
    out << "[\n";
    bool first = true;
    for (const auto& [key, list] : context.handlers()) {
        if (key.rfind("object:", 0) != 0) continue;
        const auto message = key.substr(7);
        // Census only: TORONTO1's message types all end in "Msg" (type bases
        // cannot classify them; SCRIPT_HOSTING.md §1.7).
        if (!message.ends_with("Msg")) continue;
        out << (first ? "" : ",\n") << "      {\"message\": " << quoted(message);
        first = false;
        for (Word pass = 0; pass < 2; ++pass) {
            std::map<std::string, std::size_t> sections;
            std::size_t total = 0, selections = 0;
            std::optional<std::string> error;
            for (const auto& [label, receiver] : receivers) {
                try {
                    const auto chosen = context.select_handlers(receiver, message, pass);
                    if (chosen.empty()) continue;
                    ++total;
                    selections += chosen.size();
                    ++sections[label];
                } catch (const std::exception& failure) {
                    if (!error) error = label + " " + std::to_string(receiver.id) + ": " + failure.what();
                }
            }
            out << ", \"pass" << pass << "\": {\"receivers\": " << total << ", \"selections\": " << selections
                << ", \"by_section\": {";
            bool inner = true;
            for (const auto& [label, count] : sections) {
                out << (inner ? "" : ", ") << quoted(label) << ": " << count;
                inner = false;
            }
            out << '}';
            if (error) out << ", \"first_error\": " << quoted(*error);
            out << '}';
        }
        out << '}';
    }
    out << "\n    ]";
}

// BOX: records by type: how many have links, start disabled, and have a
// HotboxMsg handler selected in pass 0 / pass 1 in the current context state.
// The native receiver runs only for boxes no handler selects (003c8de0).
void write_hotbox_census(std::ostream& out, ScriptContext& context, Recorder& host, const Population& population) {
    struct Row { std::size_t count{}, linked{}, disabled{}, pass0{}, pass1{}, handled_linked{}; };
    std::map<std::uint32_t, Row> rows;
    for (const auto& box : population.hotboxes) {
        auto& row = rows[box.type];
        ++row.count;
        if (!box.links.empty()) ++row.linked;
        if (box.starts_disabled()) ++row.disabled;
        const auto receiver = host.world.receiver(box.id).second;
        const bool p0 = !context.select_handlers(receiver, "HotboxMsg", 0).empty();
        const bool p1 = !context.select_handlers(receiver, "HotboxMsg", 1).empty();
        if (p0) ++row.pass0;
        if (p1) ++row.pass1;
        if ((p0 || p1) && !box.links.empty()) ++row.handled_linked;
    }
    out << '[';
    bool first = true;
    for (const auto& [type, row] : rows) {
        out << (first ? "\n" : ",\n") << "    {\"type\": " << type << ", \"count\": " << row.count << ", \"linked\": "
            << row.linked << ", \"starts_disabled\": " << row.disabled << ", \"pass0\": " << row.pass0
            << ", \"pass1\": " << row.pass1 << ", \"handled_and_linked\": " << row.handled_linked << '}';
        first = false;
    }
    out << "\n  ]";
}

void write_call(std::ostream& out, const Recorder::Call& call) {
    out << "{\"stage\": " << quoted(call.stage) << ", \"native\": " << quoted(call.signature) << ", \"arguments\": [";
    for (std::size_t j = 0; j < call.arguments.size(); ++j) out << (j ? ", " : "") << call.arguments[j];
    out << "], \"host\": " << quoted(call.hosted ? "objectives" : call.answered ? "answer_table" : "recorded");
    if (call.result) out << ", \"result\": " << *call.result;
    out << '}';
}

// t_orig_s2.py's inputs (SCRIPT_HOSTING.md 1.8) on the started context, in
// order, with explicit script-clock steps. Diagnostic only: unhosted
// value-returning natives answer 0 as the original run's recorders did, and
// message getters answer from the delivered message. Stops at the first error.
std::string run_s2_scenario(ScriptContext& context, Recorder& host, const Population& population) {
    if (population.npcs.empty()) throw std::runtime_error("S2 scenario needs the player record");
    const Word player = population.npcs[0].id;
    struct Step {
        std::string label;
        std::optional<bool> handled;
        std::size_t ran{}, first_call{}, calls{}, lookups{}, unresolved{};
        std::vector<std::string> skipped;
        std::optional<std::string> error;
    };
    std::vector<Step> steps;
    bool stopped = false;
    auto step = [&](const std::string& label, auto action) {
        if (stopped) return;
        host.stage = label;
        const auto calls = host.calls.size();
        const auto skipped = context.skipped_calls().size();
        const auto lookups = host.lookups.size();
        Step result{label};
        try { action(result); }
        catch (const std::exception& error) { result.error = error.what(); stopped = true; }
        result.first_call = calls;
        result.calls = host.calls.size() - calls;
        result.skipped.assign(context.skipped_calls().begin() + static_cast<std::ptrdiff_t>(skipped),
                              context.skipped_calls().end());
        for (auto i = lookups; i < host.lookups.size(); ++i) {
            ++result.lookups;
            if (!host.lookups[i].resolved) ++result.unresolved;
        }
        steps.push_back(std::move(result));
    };
    // HotboxMsg getters: Action is the tracker byte +0x24 as the registered
    // enum value (Enter 0, Exit 2); Who is the actor +0x30; WhoPlayer its
    // player component.
    auto hotbox = [&](Word box, Word action, const char* name) {
        step(std::string("HotboxMsg ") + name + " -> " + std::to_string(box), [&](Step& result) {
            ScriptContext::Message message{"HotboxMsg", {{"Action", Value(Word{action})},
                                                         {"Who", Value(host.world.objects.resolve(player | 0x80000000U))},
                                                         {"WhoPlayer", Value(host.world.objects.resolve(player))}}};
            result.handled = context.deliver(host.world.receiver(box).second, std::move(message));
        });
    };
    auto activated = [&](Word npc) {
        step("AIActivatedMsg -> " + std::to_string(npc), [&](Step& result) {
            result.handled = context.deliver(host.world.receiver(npc).second,
                                             ScriptContext::Message{"AIActivatedMsg", {}});
        });
    };
    auto advance = [&](std::int32_t ms) {
        step("advance " + std::to_string(ms) + " ms", [&](Step& result) { result.ran = context.schedules().advance(ms); });
    };

    host.answer_table = true;
    hotbox(140045, 0, "Enter");
    advance(1999);
    advance(1);
    advance(10000);
    hotbox(140045, 2, "Exit");
    hotbox(140045, 0, "Enter");
    hotbox(140153, 0, "Enter");
    advance(2000);
    hotbox(140153, 0, "Enter");
    activated(133122);
    advance(3000);
    advance(2000);
    advance(100);
    activated(133141);
    advance(3000);
    activated(133143);
    activated(133122);
    advance(3000);
    host.answer_table = false;

    std::ostringstream out;
    out << "{\"answers\": " << quoted("diagnostic answer table: unhosted value-returning natives give 0") << ", \"player\": "
        << player << ", \"pending_schedules\": " << context.schedules().snapshot().size() << ", \"steps\": [\n";
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& item = steps[i];
        out << "      {\"step\": " << quoted(item.label);
        if (item.handled) out << ", \"handled\": " << (*item.handled ? "true" : "false");
        if (item.label.starts_with("advance")) out << ", \"callbacks\": " << item.ran;
        out << ", \"lookups\": " << item.lookups << ", \"unresolved\": " << item.unresolved << ", \"skipped\": [";
        for (std::size_t j = 0; j < item.skipped.size(); ++j) out << (j ? ", " : "") << quoted(item.skipped[j]);
        out << "], \"natives\": [";
        for (std::size_t j = 0; j < item.calls; ++j) {
            out << (j ? ",\n        " : "\n        ");
            write_call(out, host.calls[item.first_call + j]);
        }
        out << (item.calls ? "\n      ]" : "]");
        if (item.error) out << ", \"error\": " << quoted(*item.error);
        out << '}' << (i + 1 < steps.size() ? "," : "") << '\n';
    }
    out << "    ]}";
    return out.str();
}

}

void write_script_start_probe(std::ostream& out, const std::map<std::string, Script>& modules,
                              const Population& population) {
    Recorder natives(population);
    EngineStruct to_hit("ToHitSettings", to_hit_settings_defaults());
    ScriptContext context(natives, [&](const std::string& name) -> std::optional<Script> {
        const auto found = modules.find(name);
        if (found == modules.end()) return std::nullopt;
        return found->second;
    });
    // Written by init::ClientStartLevel before any read (the S1 host owns it).
    context.define_engine_variable("game::DarknessDecreaser", Word{0});
    context.define_engine_variable("game::ToHit", to_hit.address());

    struct Stage {
        std::string name;
        std::optional<std::string> result;
        std::optional<std::string> error;
        std::size_t calls{};
    };
    std::vector<Stage> stages;
    auto run = [&](const std::string& name, auto action) {
        natives.stage = name;
        const auto before = natives.calls.size();
        Stage stage{name, {}, {}, {}};
        try { stage.result = action(); }
        catch (const std::exception& error) { stage.error = error.what(); }
        stage.calls = natives.calls.size() - before;
        stages.push_back(std::move(stage));
        return !stages.back().error;
    };
    std::vector<std::pair<std::string, std::string>> coverage;
    auto census = [&](const std::string& after) {
        std::ostringstream text;
        try { write_coverage(text, context, natives); }
        catch (const std::exception& error) { text.str(""); text << quoted(std::string("error: ") + error.what()); }
        coverage.emplace_back(after, text.str());
    };
    if (run("start_level", [&] { return std::string(context.start_level() ? "true" : "false"); })) {
        census("start_level");
        bool all = true;
        for (const auto* entry : {"init::ClientStartLevel", "init::StartLevel"}) {
            if (!run(entry, [&] { context.invoke(entry); return std::string("void"); })) { all = false; break; }
        }
        if (all) census("init::StartLevel");
    }
    // S1 records end here; the S2 scenario's are reported in its own section.
    const auto s1_calls = natives.calls.size();
    const auto s1_lookups = natives.lookups.size();
    std::optional<std::string> hotbox_census;
    if (stages.size() == 3 && !stages.back().error) {
        std::ostringstream text;
        try { write_hotbox_census(text, context, natives, population); }
        catch (const std::exception& error) { text.str(""); text << quoted(std::string("error: ") + error.what()); }
        hotbox_census = text.str();
    }
    std::optional<std::string> s2;
    if (stages.size() == 3 && !stages.back().error) {
        try { s2 = run_s2_scenario(context, natives, population); }
        catch (const std::exception& error) { s2 = quoted(std::string("error: ") + error.what()); }
    }
    std::ostringstream handler_map;
    try { write_handler_map(handler_map, context); }
    catch (const std::exception& error) { handler_map.str(""); handler_map << quoted(std::string("error: ") + error.what()); }

    out << "{\n  \"probe\": \"script start (003e20a0, 003e1a20)\",\n  \"modules\": [";
    const auto names = context.module_names();
    for (std::size_t i = 0; i < names.size(); ++i) out << (i ? ", " : "") << quoted(names[i]);
    out << "],\n  \"stages\": [\n";
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        out << "    {\"stage\": " << quoted(stage.name) << ", \"native_calls\": " << stage.calls;
        if (stage.result) out << ", \"result\": " << quoted(*stage.result);
        if (stage.error) out << ", \"error\": " << quoted(*stage.error);
        out << '}' << (i + 1 < stages.size() ? "," : "") << '\n';
    }
    out << "  ],\n  \"log\": [";
    for (std::size_t i = 0; i < context.log().size(); ++i) out << (i ? ", " : "") << quoted(context.log()[i]);
    out << "],\n  \"calls\": [\n";
    for (std::size_t i = 0; i < s1_calls; ++i) {
        out << "    ";
        write_call(out, natives.calls[i]);
        out << (i + 1 < s1_calls ? "," : "") << '\n';
    }
    out << "  ],\n  \"handler_map\": " << handler_map.str() << ",\n  \"handler_coverage\": [";
    for (std::size_t i = 0; i < coverage.size(); ++i)
        out << (i ? ",\n" : "\n") << "    {\"after\": " << quoted(coverage[i].first) << ", \"messages\": "
            << coverage[i].second << '}';
    out << "\n  ],\n  \"identity_context\": "
        << quoted("One diagnostic scope; every decoded population record by ID, NPC 0 as player and the rest as AI")
        << ",\n  \"id_collisions\": [";
    for (std::size_t i = 0; i < natives.world.collisions.size(); ++i)
        out << (i ? ", " : "") << quoted(natives.world.collisions[i]);
    out << "],\n  \"object_lookups\": [\n";
    std::map<Word, std::string> debug_names;
    for (const auto& name : population.debug_names) debug_names.emplace(static_cast<Word>(name.id), name.name);
    for (std::size_t i = 0; i < s1_lookups; ++i) {
        const auto& lookup = natives.lookups[i];
        const auto raw = static_cast<Word>(static_cast<std::uint32_t>(lookup.raw_id) & 0x7fffffffu);
        const auto section = natives.world.sections.find(raw);
        const auto debug_name = debug_names.find(raw);
        out << "    {\"stage\": " << quoted(lookup.stage) << ", \"id\": " << static_cast<std::uint32_t>(lookup.raw_id)
            << ", \"section\": " << (section == natives.world.sections.end() ? "null" : quoted(std::as_const(section->second)))
            << ", \"resolved\": " << (lookup.resolved ? "true" : "false");
        if (debug_name != debug_names.end()) out << ", \"debug_name\": " << quoted(debug_name->second);
        out << '}' << (i + 1 < s1_lookups ? "," : "") << '\n';
    }
    out << "  ]";
    if (hotbox_census) out << ",\n  \"hotbox_census\": " << *hotbox_census;
    if (s2) out << ",\n  \"s2_scenario\": " << *s2;
    out << "\n}\n";
}

}
