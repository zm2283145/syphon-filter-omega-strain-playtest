#include "script_context.hpp"

#include "engine_globals.hpp"
#include "water_effects.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace omega {
namespace {

constexpr Word untyped = 0xffffffffU;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// 003e1540 + 00128e80: base name (no directory, no extension), compared
// case-insensitively.
std::string base_name(const std::string& name) {
    auto result = name.substr(name.find_last_of('\\') == std::string::npos ? 0 : name.find_last_of('\\') + 1);
    if (const auto dot = result.find('.'); dot != std::string::npos) result.resize(dot);
    return lower(std::move(result));
}

bool same_parameters(const Script& a, const Function& fa, const Script& b, const Function& fb) {
    if (fa.parameters.size() != fb.parameters.size()) return false;
    for (std::size_t i = 0; i < fa.parameters.size(); ++i)
        if (a.type_key(fa.parameters[i]) != b.type_key(fb.parameters[i])) return false;
    return true;
}

struct GroupNative {
    const char* name;
    std::vector<std::string> parameters;
    const char* result;
};

// cGroup's natives as registered by 004d60dc..004d63a4.
const std::vector<GroupNative>& group_natives() {
    static const std::vector<GroupNative> table{
        {"Add", {"object:Group", "object:System"}, "primitive:4"},       // 003d9910 -> 003d9bf0
        {"AddDup", {"object:Group", "object:System"}, "primitive:4"},    // 003d98c0 -> 003d9ba0
        {"Remove", {"object:Group", "object:System"}, "primitive:4"},    // 003d9870 -> 003d9ad0
        {"RemoveAll", {"object:Group"}, "primitive:4"},                  // 003d9840 -> 00175230
        {"Print", {"object:Group"}, "primitive:4"},                      // 003d97e0 -> 003d99f0 (empty)
        {"Find", {"object:Group", "object:System"}, "primitive:1"},      // 003d9790 -> 003d9c80
        {"Contains", {"object:Group", "object:System"}, "primitive:1"},  // 003d9740 -> 003d9c50
        {"GetSize", {"object:Group"}, "primitive:1"},                    // 003d9710
        {"Get", {"object:Group", "primitive:1"}, "object:System"},       // 003d96d0 -> 003d9990
        {"Randomize", {"object:Group"}, "primitive:4"},                  // 003d9810 -> 003d9a00
    };
    return table;
}

// Array natives (registration table entries with their wrappers -> bodies).
const std::vector<GroupNative>& array_natives() {
    static const std::vector<GroupNative> table{
        {"game::CreateArray", {"primitive:1"}, "object:Array"},                      // 00268710 -> 002689f0
        {"AddElem", {"object:Array", "primitive:2"}, "primitive:1"},                 // 00269000
        {"GetIndex", {"object:Array", "primitive:1"}, "primitive:2"},                // 00268cc0
        {"ChangeIndex", {"object:Array", "primitive:1", "primitive:2"}, "primitive:4"}, // 00268b80
        {"RemoveIndex", {"object:Array", "primitive:1"}, "primitive:1"},             // 00268f50
        {"Count", {"object:Array"}, "primitive:1"},                                  // 00268f20: the count +8
        {"Print", {"object:Array"}, "primitive:4"},                                  // 00268e80: does nothing
        {"FindElem", {"object:Array", "primitive:2"}, "primitive:1"},                // 00268ea0
        {"GetAndRemoveIndex", {"object:Array", "primitive:1"}, "primitive:2"},       // 00268bf0
        {"Sort", {"object:Array"}, "primitive:4"},                                   // 00268dd0
        {"game::RemoveArray", {"object:Array"}, "primitive:4"},                      // 002686e0 -> 00268740
    };
    return table;
}

// cNodeList natives (registrations 004d0a7c..004d0c8c). Each method takes the
// list's Group (+0x60) and calls the Group primitive; nodes pass through the
// identity conversions 0015c100/0015c120.
const std::vector<GroupNative>& node_list_natives() {
    static const std::vector<GroupNative> table{
        {"game::CreateNodeList", {"object:Group"}, "object:NodeList"},     // 00269400 -> 00269430
        {"Add", {"object:NodeList", "object:Node"}, "primitive:4"},        // 00269750 -> 003d9bf0
        {"AddDup", {"object:NodeList", "object:Node"}, "primitive:4"},     // 00269700 -> 003d9ba0
        {"Remove", {"object:NodeList", "object:Node"}, "primitive:4"},     // 002696b0 -> 003d9ad0
        {"Randomize", {"object:NodeList"}, "primitive:4"},                 // 00269680 -> 003d9a00
        {"Find", {"object:NodeList", "object:Node"}, "primitive:1"},       // 00269630 -> 003d9c80
        {"Contains", {"object:NodeList", "object:Node"}, "primitive:1"},   // 002695e0 -> 003d9c80 != -1
        {"GetSize", {"object:NodeList"}, "primitive:1"},                   // 002695b0: Group +4
        {"Get", {"object:NodeList", "primitive:1"}, "object:Node"},        // 00269560 -> 003d9990
    };
    return table;
}

bool matches(const std::vector<GroupNative>& table, const Script& script, const Function& function) {
    for (const auto& native : table) {
        if (function.name != native.name || function.parameters.size() != native.parameters.size()) continue;
        if (script.type_key(function.return_type) != native.result) return false;
        for (std::size_t i = 0; i < native.parameters.size(); ++i)
            if (script.type_key(function.parameters[i]) != native.parameters[i]) return false;
        return true;
    }
    return false;
}

// 003e04c0: an object's ID; the null object's is -1 (nullopt here). Groups
// store identities, so equal handles are the same member.
std::optional<Value> group_identity(const Value& value) {
    if (const auto* word = std::get_if<Word>(&value); word && *word == 0) return std::nullopt;
    if (const auto* runtime = std::get_if<RuntimeObjectHandle>(&value)) {
        if (runtime->generation == 0) return std::nullopt;
        return value;
    }
    if (std::holds_alternative<ObjectHandle>(value)) return value;
    throw std::runtime_error("Group members must be objects");
}

}

Value& ScriptContext::EngineCells::dereference(const Address& address) {
    if (address.frame != 0 || address.slot < 0 || static_cast<std::uint64_t>(address.slot) >= cells.size())
        throw std::runtime_error("Invalid engine variable address");
    return cells[static_cast<std::size_t>(address.slot)].second;
}

ScriptContext::ScriptContext(Host& natives, Loader loader, std::uint64_t step_limit, std::size_t depth_limit)
    : natives_(natives), loader_(std::move(loader)), step_limit_(step_limit), depth_limit_(depth_limit) {
    define_engine_variable("game::scriptInterpreter", interpreter);
}

ScriptContext::Module* ScriptContext::find_module(const std::string& name) const {
    const auto key = base_name(name);
    for (const auto& module : modules_)
        if (module->name == key) return module.get();
    return nullptr;
}

bool ScriptContext::load(const std::string& name) {
    const auto key = base_name(name);
    if (key.empty()) throw std::runtime_error("Empty script module name");
    // 003e12d0: any listed module counts, including one whose load failed.
    if (find_module(key)) return true;
    auto script = loader_(key);
    auto module = std::make_unique<Module>();
    module->name = key;
    if (!script) {
        // 003e1720 lists the failed module too, so a later load reports success.
        modules_.push_back(std::move(module));
        log_.push_back("couldn't load script " + key + "...");
        return false;
    }
    module->script = std::move(*script);
    for (const auto& variable : module->script.variables) {
        if (!variable.external && variable_address(variable.name))
            throw std::runtime_error("Script variable defined twice: " + variable.name);
    }
    module->vm = std::make_unique<Vm>(module->script, static_cast<Host&>(*this), schedules_, step_limit_, depth_limit_);
    modules_.push_back(std::move(module));
    log_.push_back("loaded " + key + "...");
    return true;
}

void ScriptContext::execute_init_code() {
    // Index loop: an initializer may load further modules.
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        auto& vm = modules_[i]->vm;
        if (vm && !vm->initialized()) vm->initialize();
    }
}

bool ScriptContext::check_all_references() {
    bool ok = true;
    std::set<std::string> reported;
    auto report = [&](const std::string& message) {
        ok = false;
        if (reported.insert(message).second) log_.push_back(message);
    };
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        const auto& script = module->script;
        // A module gives an import a type exactly when its code references it.
        for (const auto& variable : script.variables) {
            if (variable.external && variable.type != untyped && !variable_address(variable.name))
                report("var " + variable.name + " not defined");
        }
        for (const auto& function : script.functions) {
            if (function.external && function.return_type != untyped && !provides(script, function))
                report("func " + function.name + " not defined");
        }
    }
    return ok;
}

bool ScriptContext::start_level() {
    if (!load("init") || !load("utils")) return false;
    execute_init_code();
    bool found = false;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& function : module->script.functions)
            found = found || (!function.external && function.name == "init::Init" && function.parameters.empty());
    }
    if (!found) {
        log_.push_back("init::Init not found");
        return false;
    }
    invoke("init::Init");
    return check_all_references();
}

void ScriptContext::define_engine_variable(const std::string& name, Value value) {
    if (variable_address(name)) throw std::runtime_error("Engine variable already defined: " + name);
    engine_.cells.emplace_back(name, std::move(value));
}

const Value& ScriptContext::engine_variable(const std::string& name) const {
    for (const auto& [cell, value] : engine_.cells)
        if (cell == name) return value;
    throw std::runtime_error("Unknown engine variable: " + name);
}

std::optional<Value> ScriptContext::invoke(const std::string& name, const std::vector<Value>& arguments) {
    Module* owner = nullptr;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& function : module->script.functions) {
            if (function.external || function.name != name || function.parameters.size() != arguments.size()) continue;
            if (owner && owner != module.get()) throw std::runtime_error("Script entry defined by two modules: " + name);
            owner = module.get();
        }
    }
    if (!owner) throw std::runtime_error("No script module defines " + name);
    return owner->vm->invoke(name, arguments);
}

bool ScriptContext::defines(const std::string& name, std::size_t arguments) const {
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& function : module->script.functions)
            if (!function.external && function.name == name && function.parameters.size() == arguments) return true;
    }
    return false;
}

std::optional<std::string> ScriptContext::entry_name(const std::string& name, std::size_t arguments) const {
    const auto key = lower(name);
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& function : module->script.functions)
            if (!function.external && function.parameters.size() == arguments && lower(function.name) == key)
                return function.name;
    }
    return std::nullopt;
}

Vm& ScriptContext::module(const std::string& name) {
    const auto* found = find_module(name);
    if (!found || !found->vm) throw std::runtime_error("Script module not loaded: " + name);
    return *found->vm;
}

std::vector<std::string> ScriptContext::module_names() const {
    std::vector<std::string> names;
    for (const auto& module : modules_) names.push_back(module->name);
    return names;
}

std::optional<ScriptContext::NativeCallSite> ScriptContext::native_call_site(const Script& script) const {
    for (const auto& module : modules_) {
        if (&module->script != &script || !module->vm) continue;
        const auto& site = module->vm->call_site();
        if (!site || !site->caller) return std::nullopt;
        return NativeCallSite{module->name, site->caller->name, site->word};
    }
    return std::nullopt;
}

std::optional<ScriptContext::Definition> ScriptContext::definition(const Script& script, const Function& import) const {
    std::optional<Definition> found;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        const auto& functions = module->script.functions;
        for (std::size_t i = 0; i < functions.size(); ++i) {
            const auto& candidate = functions[i];
            if (candidate.external || candidate.name != import.name ||
                !same_parameters(module->script, candidate, script, import)) continue;
            if (found) throw std::runtime_error("Ambiguous script import: " + import.name);
            found = Definition{module.get(), i};
        }
    }
    return found;
}

bool ScriptContext::manager_native(const Script& script, const Function& function) const {
    std::vector<std::string> expected;
    if (function.name == "LoadScript") expected = {"object:ScriptInterpreter", "primitive:3"};
    else if (function.name == "CheckAllReferences" || function.name == "ExecuteInitCode")
        expected = {"object:ScriptInterpreter"};
    else return false;
    if (function.parameters.size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (script.type_key(function.parameters[i]) != expected[i]) return false;
    return true;
}

std::optional<Address> ScriptContext::variable_address(const std::string& name) const {
    std::optional<Address> found;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& variable : module->script.variables) {
            if (variable.external || variable.name != name) continue;
            if (found) throw std::runtime_error("Script variable defined twice: " + name);
            found = Address{0, variable.storage, module->vm.get()};
        }
    }
    for (std::size_t i = 0; i < engine_.cells.size(); ++i) {
        if (engine_.cells[i].first != name) continue;
        if (found) throw std::runtime_error("Script variable shadows an engine variable: " + name);
        found = Address{0, static_cast<std::int64_t>(i), const_cast<EngineCells*>(&engine_)};
    }
    return found;
}

std::optional<Value> ScriptContext::call(const Script& script, const Function& function,
                                         const std::vector<Value>& arguments) {
    if (const auto target = definition(script, function)) {
        const auto& defined = target->module->script.functions[target->index];
        if (target->module->script.type_key(defined.return_type) != script.type_key(function.return_type))
            throw std::runtime_error("Script import return type differs from its definition: " + function.name);
        return target->module->vm->invoke_definition(target->index, arguments);
    }
    // 003ddfc0's native path (003de3fc..003de48c) and 003e0ab0 for scheduled
    // calls: a native with an object or string argument below 1000 is not
    // called; a void one pushes nothing, any other 0. Typed nulls here are a
    // Word below 1000 (signed, as the original compares the raw word), a
    // generation-0 runtime handle and the objective handle 0.
    for (std::size_t i = 0; i < function.parameters.size() && i < arguments.size(); ++i) {
        const auto type = function.parameters[i];
        if (type != 3 && type < 100) continue;
        const auto& argument = arguments[i];
        const auto* word = std::get_if<Word>(&argument);
        if ((word && word_signed(*word) < 1000) || null_handle(argument)) {
            skipped_.push_back(function.name);
            if (function.return_type == 4) return std::nullopt;
            return Word{0};
        }
    }
    if (manager_native(script, function)) {
        // 003e1470 / 003e14b0 / 003e14e0 take the interpreter object first.
        const auto* handle = std::get_if<ObjectHandle>(&arguments.at(0));
        if (!handle || *handle != interpreter) throw std::runtime_error(function.name + " needs the script interpreter");
        std::optional<Value> result;
        if (function.name == "LoadScript") {
            const auto* name = std::get_if<std::string>(&arguments.at(1));
            if (!name) throw std::runtime_error("LoadScript needs a module name");
            result = Word(load(*name));
        } else if (function.name == "CheckAllReferences") {
            result = Word(check_all_references());
        } else {
            execute_init_code();
            result = Word{0};
        }
        if (function.return_type == 4) return std::nullopt;
        if (script.type_key(function.return_type) != "primitive:1")
            throw std::runtime_error("Unsupported script manager signature: " + function.name);
        return result;
    }
    if (group_native(script, function)) return call_group_native(function, arguments);
    if (array_native(script, function)) return call_array_native(function, arguments);
    if (node_list_native(script, function)) return call_node_list_native(function, arguments);
    if (!arguments.empty())
        if (const auto* handle = std::get_if<ObjectHandle>(&arguments[0]);
            handle && handle->kind == ObjectHandle::Kind::message)
            return call_message_getter(script, function, *handle);
    return natives_.call(script, function, arguments);
}

bool ScriptContext::group_native(const Script& script, const Function& function) const {
    return matches(group_natives(), script, function);
}

bool ScriptContext::array_native(const Script& script, const Function& function) const {
    return matches(array_natives(), script, function);
}

ScriptContext::Array& ScriptContext::array(const Value& value) {
    const auto* handle = std::get_if<ObjectHandle>(&value);
    if (!handle || handle->kind != ObjectHandle::Kind::array || handle->id >= arrays_.size())
        throw std::runtime_error("Not a script array");
    if (arrays_[handle->id].removed)
        throw std::runtime_error("Script array used after game::RemoveArray (00268740 freed it)");
    return arrays_[handle->id];
}

std::vector<Word> ScriptContext::array_elements(const Value& value) {
    const auto& target = array(value);
    return {target.data.begin(), target.data.begin() + static_cast<std::ptrdiff_t>(target.count)};
}

std::optional<Value> ScriptContext::call_array_native(const Function& function, const std::vector<Value>& arguments) {
    auto integer = [&](std::size_t i) {
        const auto* raw = std::get_if<Word>(&arguments.at(i));
        if (!raw) throw std::runtime_error(function.name + " needs a number");
        return word_signed(*raw);
    };
    if (function.name == "game::CreateArray") {
        // 002689f0: 1..999 elements, else null.
        const auto size = integer(0);
        if (size <= 0 || size >= 1000) return Word{0};
        arrays_.push_back({std::vector<Word>(static_cast<std::size_t>(size)), 0});
        return ObjectHandle{ObjectHandle::Kind::array, Word(arrays_.size() - 1)};
    }
    // 003de464: a null pointer-like argument skips the callback; non-void gives 0.
    if (const auto* raw = std::get_if<Word>(&arguments.at(0)); raw && *raw == 0)
        return function.return_type == 4 ? std::nullopt : std::optional<Value>(Word{0});
    auto& target = array(arguments.at(0));
    if (function.name == "Count") return Word{static_cast<std::uint32_t>(target.count)};   // 00268f20: lw +8
    if (function.name == "Print") return std::nullopt;                                     // 00268e80: no effect
    const auto value = [&](std::size_t i) {
        const auto* raw = std::get_if<Word>(&arguments.at(i));
        if (!raw) throw std::runtime_error(function.name + " needs a float");
        return *raw;
    };
    if (function.name == "AddElem") {
        if (target.count >= target.data.size()) return Word{0};
        target.data[target.count++] = value(1);
        return Word{1};
    }
    // The EE FPU (c.eq.s / c.ole.s) has no NaN or infinity: an exponent-0
    // operand (zero or denormal) reads as zero, everything else orders by
    // sign and magnitude.
    const auto ee_key = [](Word w) -> std::int64_t {
        if ((w & 0x7f800000U) == 0) return 0;
        const auto magnitude = static_cast<std::int64_t>(w & 0x7fffffffU);
        return (w & 0x80000000U) ? -magnitude : magnitude;
    };
    if (function.name == "FindElem") {
        // 00268ea0: the first index whose element c.eq.s the value, else -1.
        const auto wanted = ee_key(value(1));
        for (std::size_t i = 0; i < target.count; ++i)
            if (ee_key(target.data[i]) == wanted) return Word(i);
        return Word(-1);
    }
    if (function.name == "Sort") {
        // 00268dd0: for each i below count-1, every j from i on swaps
        // [i] and [j] unless [i] c.ole.s [j] (ascending exchange sort).
        for (std::size_t i = 0; i + 1 < target.count; ++i)
            for (std::size_t j = i; j < target.count; ++j)
                if (!(ee_key(target.data[i]) <= ee_key(target.data[j]))) std::swap(target.data[i], target.data[j]);
        return std::nullopt;
    }
    if (function.name == "game::RemoveArray") {
        // 00268740: unlinks the array from the list 004f83e0 and frees it;
        // the handle dangles (the port refuses any later use).
        target.removed = true;
        return std::nullopt;
    }
    const auto index = integer(1);
    const bool inside = index >= 0 && static_cast<std::size_t>(index) < target.count;
    if (function.name == "GetIndex") return inside ? target.data[static_cast<std::size_t>(index)] : Word{0};
    if (function.name == "GetAndRemoveIndex") {
        // 00268bf0: the element (0.0 outside the count), then RemoveIndex's shift.
        if (!inside) return Word{0};
        const auto taken = target.data[static_cast<std::size_t>(index)];
        target.data.erase(target.data.begin() + index);
        target.data.push_back(0);
        --target.count;
        return taken;
    }
    if (function.name == "ChangeIndex") {
        if (inside) target.data[static_cast<std::size_t>(index)] = value(2);
        return std::nullopt;
    }
    if (!inside) return Word{0};
    target.data.erase(target.data.begin() + index);
    target.data.push_back(0);
    --target.count;
    return Word{1};
}

std::vector<std::optional<Value>>& ScriptContext::group(const Value& value) {
    const auto* handle = std::get_if<ObjectHandle>(&value);
    if (!handle || handle->kind != ObjectHandle::Kind::group || handle->id >= groups_.size())
        throw std::runtime_error("Not a script group");
    return groups_[handle->id];
}

const std::vector<std::optional<Value>>& ScriptContext::group_members(const Value& value) {
    return group(value);
}

void ScriptContext::drop_from_groups(const Value& object, const std::optional<Value>& component) {
    const auto drop = [](std::vector<std::optional<Value>>& members, const Value& who) {
        const auto id = group_identity(who);
        for (std::size_t i = 0; i < members.size(); ++i)
            if (members[i] == id) { members.erase(members.begin() + static_cast<std::ptrdiff_t>(i)); return; }
    };
    for (auto& members : groups_) {
        drop(members, object);
        if (component) drop(members, *component);
    }
}

std::optional<Value> ScriptContext::call_group_native(const Function& function, const std::vector<Value>& arguments) {
    return group_method(function.name, group(arguments.at(0)), arguments);
}

std::optional<Value> ScriptContext::group_method(const std::string& name, std::vector<std::optional<Value>>& members,
                                                 const std::vector<Value>& arguments) {
    // Warnings go through 003d7930, which is empty in this build.
    auto find = [&](const Value& object) -> Word {
        const auto id = group_identity(object);
        for (std::size_t i = 0; i < members.size(); ++i)
            if (members[i] == id) return Word(i);
        return Word(-1);
    };
    if (name == "GetSize") return Word(members.size());
    if (name == "Print") return std::nullopt;
    if (name == "RemoveAll") { members.clear(); return std::nullopt; }
    if (name == "Randomize") {
        // 003d9a00: for each index i below the (re-read) size, one 00127dd0 draw r and
        // j = cvt.w.s(cvt.s.w(size) * (cvt.s.w(r) / 2^31)); entries i and j swap.
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto draw = natives_.game_random();
            if (draw > 0x7fffffffU) throw std::runtime_error("Group Randomize: RNG value outside 0..2^31-1");
            const float unit = game_int_to_float(static_cast<std::int32_t>(draw)) / 2147483648.0F;
            const float scaled = game_int_to_float(static_cast<std::int32_t>(members.size())) * unit;
            const auto j = static_cast<std::size_t>(static_cast<std::int32_t>(scaled));
            if (j >= members.size()) throw std::runtime_error("Group Randomize: index outside the group");
            std::swap(members[i], members[j]);
        }
        return std::nullopt;
    }
    if (name == "Get") {
        // 003d9990: out of range warns and gives null. A valid entry goes back
        // through the object table (003e0400), so liveness is the object
        // host's to check on use.
        const auto* raw = std::get_if<Word>(&arguments.at(1));
        if (!raw) throw std::runtime_error("Group Get needs an index");
        const auto index = word_signed(*raw);
        if (index < 0 || static_cast<std::size_t>(index) >= members.size()) return Word{0};
        const auto& entry = members[static_cast<std::size_t>(index)];
        if (!entry) return Word{0};
        return *entry;
    }
    const auto& object = arguments.at(1);
    if (name == "Find") return find(object);
    if (name == "Contains") return Word(find(object) != Word(-1));
    if (name == "Add") {
        if (find(object) == Word(-1)) members.push_back(group_identity(object));
        return std::nullopt;
    }
    if (name == "AddDup") { members.push_back(group_identity(object)); return std::nullopt; }
    // Remove (003d9ad0): the first match only.
    if (const auto at = find(object); at != Word(-1)) members.erase(members.begin() + at);
    return std::nullopt;
}

bool ScriptContext::node_list_native(const Script& script, const Function& function) const {
    return matches(node_list_natives(), script, function);
}

std::optional<Value> ScriptContext::call_node_list_native(const Function& function, const std::vector<Value>& arguments) {
    if (function.name == "game::CreateNodeList") {
        // 00269430: a 0x70-byte world object with an ID in category 0xf5,
        // registered (0040c9d0) and holding the Group itself at +0x60 (no
        // copy, so the list and the script's Group share their members). A
        // null Group was already skipped by the call path.
        const auto& wrapped = arguments.at(0);
        group(wrapped);
        const auto handle = natives_.create_world_object(0xf5);
        if (!node_lists_.emplace(handle.id, NodeList{handle, wrapped}).second)
            throw std::runtime_error("NodeList ID registered twice");
        return handle;
    }
    return group_method(function.name, group(node_list(arguments.at(0)).group), arguments);
}

const ScriptContext::NodeList& ScriptContext::node_list(const Value& value) const {
    const auto* handle = std::get_if<RuntimeObjectHandle>(&value);
    if (!handle) throw std::runtime_error("Not a NodeList");
    const auto found = node_lists_.find(handle->id);
    if (found == node_lists_.end() || found->second.object != *handle) throw std::runtime_error("Not a NodeList");
    return found->second;
}

std::optional<Value> ScriptContext::node_list_group(const Value& node_list_object) const {
    const auto* handle = std::get_if<RuntimeObjectHandle>(&node_list_object);
    if (!handle) return std::nullopt;
    const auto found = node_lists_.find(handle->id);
    if (found == node_lists_.end() || found->second.object != *handle) return std::nullopt;
    return found->second.group;
}

Value ScriptContext::create_group(const std::vector<Value>& members) {
    // 003de280: null entries are skipped; the rest go through Add (003d9bf0).
    groups_.emplace_back();
    const ObjectHandle handle{ObjectHandle::Kind::group, Word(groups_.size() - 1)};
    auto& entries = groups_.back();
    for (const auto& member : members) {
        const auto id = group_identity(member);
        if (id && std::find(entries.begin(), entries.end(), id) == entries.end()) entries.push_back(id);
    }
    return handle;
}

Word ScriptContext::enum_value(const std::string& name, Word member) {
    // Engine registrations first, then a loaded module that declares the enum
    // locally (OBJECTIVES imports VOICE's SFX_ACTION by name); otherwise the host.
    if (const auto* values = engine_enum(name)) {
        if (member >= values->size()) throw std::runtime_error("Enum member outside registered table: " + name);
        return (*values)[member];
    }
    const ScriptEnum* local = nullptr;
    for (const auto& module : modules_)
        for (const auto& type : module->script.enums) {
            if (type.external || type.name != name) continue;
            if (local && local->members != type.members) throw std::runtime_error("Ambiguous script enum: " + name);
            local = &type;
        }
    if (local) {
        if (member >= local->members.size()) throw std::runtime_error("Enum member outside script declaration: " + name);
        return local->members[member].second;
    }
    return natives_.enum_value(name, member);
}

Value ScriptContext::object_from_id(Word raw_id) {
    return natives_.object_from_id(raw_id);
}

Word ScriptContext::world_object_id(const RuntimeObjectHandle& object) {
    return natives_.world_object_id(object);
}

Word ScriptContext::object_to_id(const Value& object) {
    // 003e04c0: the null object gives -1 before any virtual call.
    if (!group_identity(object)) return 0xffffffffU;
    return natives_.object_to_id(object);
}

void ScriptContext::relink() {
    if (linked_modules_ == modules_.size()) return;
    std::map<std::string, std::vector<Handler>> map;
    std::map<std::string, std::optional<std::string>> bases;
    std::set<std::string> conflicts;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        // Each module's type table repeats the engine registration (003d7590
        // walks its base link), so the tables must agree.
        for (const auto& type : module->script.types) {
            const auto [found, inserted] = bases.emplace(type.name, type.base);
            if (!inserted && found->second != type.base) conflicts.insert(type.name);
        }
        const auto& functions = module->script.functions;
        for (std::size_t i = 0; i < functions.size(); ++i) {
            const auto& function = functions[i];
            if (function.external || function.parameters.size() < 2) continue;
            const auto& fields = function.dispatch_fields;
            // +0x24: a handler gated by an activation context (byte +0x64)
            // that also wins pass-0 ties. TORONTO1 has none.
            if (fields[0] != untyped)
                throw std::runtime_error("Handler activation contexts are not hosted: " + function.name);
            auto selector = Selector::generic;
            if (word_signed(fields[1]) >= 0) selector = Selector::literal;
            else if (fields[2] != untyped) selector = Selector::variable;
            else if (fields[3] != untyped) selector = Selector::group;
            map[module->script.type_key(function.parameters[1])].push_back(
                {module->name, i, function.name, selector, fields[4]});
        }
    }
    handlers_ = std::move(map);
    type_bases_ = std::move(bases);
    conflicting_bases_ = std::move(conflicts);
    linked_modules_ = modules_.size();
}

const std::map<std::string, std::vector<ScriptContext::Handler>>& ScriptContext::handlers() {
    relink();
    return handlers_;
}

std::vector<std::string> ScriptContext::type_chain(const std::string& type) {
    relink();
    std::vector<std::string> chain{type};
    while (chain.size() < 1000) {
        if (conflicting_bases_.contains(chain.back()))
            throw std::runtime_error("Script type bases disagree: " + chain.back());
        const auto found = type_bases_.find(chain.back());
        if (found == type_bases_.end() || !found->second) return chain;
        chain.push_back(*found->second);
    }
    throw std::runtime_error("Script type base chain does not end: " + type);
}

Value& ScriptContext::selector_variable(const Script& script, Word index) {
    if (index >= script.variables.size()) throw std::runtime_error("Handler selector variable outside table");
    const auto& name = script.variables[index].name;
    const auto address = variable_address(name);
    if (!address) throw std::runtime_error("Handler selector variable not linked: " + name);
    return address->owner->dereference(*address);
}

bool ScriptContext::scan(const Receiver& receiver, const std::string& message_type, Word pass,
                         const std::function<void(const Handler&)>& selected) {
    // Exact message type; no walk to base message types.
    const auto& map = handlers();
    const auto found = map.find("object:" + message_type);
    if (found == map.end()) return false;
    // A copy: a handler run in pass 1 may load modules and rebuild the map.
    const auto list = found->second;
    const auto chain = type_chain(receiver.type);
    std::optional<Handler> by_literal, by_variable, by_group, by_generic;
    std::size_t nearest = 1000;
    bool handled = false;
    for (const auto& handler : list) {
        if (handler.pass != pass) continue;
        const auto* module = find_module(handler.module);
        if (!module || !module->vm) continue;
        const auto& script = module->script;
        const auto& fields = script.functions[handler.function].dispatch_fields;
        std::size_t distance = 0;
        bool match = false;
        switch (handler.selector) {
        case Selector::literal:
            match = word_signed(receiver.id) >= 0 && receiver.id == fields[1];
            break;
        case Selector::variable:
            if (receiver.id != untyped) {
                const auto* raw = std::get_if<Word>(&selector_variable(script, fields[2]));
                if (!raw) throw std::runtime_error("Handler variable selector does not hold an ID word: " + handler.name);
                match = (*raw & 0x7fffffffU) == receiver.id;
            }
            break;
        case Selector::group: {
            // 003d9c50 (Contains) on the group variable's current group.
            const auto& members = group(selector_variable(script, fields[3]));
            match = std::find(members.begin(), members.end(), group_identity(receiver.object)) != members.end();
            break;
        }
        case Selector::generic: {
            if (pass == 0 && (by_literal || by_variable || by_group)) continue;
            const auto& parameter = script.functions[handler.function].parameters[0];
            const auto key = script.type_key(parameter);
            for (; distance < chain.size() && distance <= nearest; ++distance)
                if (key == "object:" + chain[distance]) { match = true; break; }
            // Pass 0 keeps the nearest, the first on ties.
            if (pass == 0) match = match && distance < nearest;
            break;
        }
        }
        if (!match) continue;
        if (pass != 0) {
            selected(handler);
            handled = true;
            continue;
        }
        switch (handler.selector) {
        case Selector::literal: by_literal = handler; break;
        case Selector::variable: if (!by_variable) by_variable = handler; break;
        case Selector::group: if (!by_group) by_group = handler; break;
        case Selector::generic: by_generic = handler; nearest = distance; break;
        }
    }
    if (pass != 0) return handled;
    const auto& winner = by_group ? by_group : by_variable ? by_variable : by_literal ? by_literal : by_generic;
    if (!winner) return false;
    selected(*winner);
    return true;
}

void ScriptContext::invoke_handler(const Handler& handler, const Receiver& receiver, const Value& message) {
    auto* module = find_module(handler.module);
    if (!module || !module->vm) throw std::runtime_error("Handler module not loaded: " + handler.module);
    const auto& script = module->script;
    const auto parameter = script.functions[handler.function].parameters[0];
    module->vm->invoke_definition(handler.function, {ai_policy(script, parameter) ? Value(receiver.id) : receiver.object,
                                                      message});
}

bool ScriptContext::ai_policy(const Script& script, Word type) {
    // 003e0a70: in a version-8 script, a value of a type that is, or derives
    // from, the type named AI (case-insensitive) is the ID word.
    if (script.version < 8 || type < 100 || type - 100 >= script.types.size()) return false;
    for (const auto& name : type_chain(script.types[type - 100].name))
        if (lower(name) == "ai") return true;
    return false;
}

Host::ScheduleCapture ScriptContext::schedule_capture(const Script& script, Word type) {
    if (type < 100 || type - 100 >= script.types.size()) throw std::runtime_error("Deferred argument type outside table");
    // The engine type flags (+0x4c, +0x45) are not in the script tables; the
    // families follow the registrations (SCRIPT_HOSTING.md 2.4).
    const auto& name = script.types[type - 100].name;
    if (ai_policy(script, type)) return ScheduleCapture::keep;
    const auto chain = type_chain(name);
    if (std::find(chain.begin(), chain.end(), "System") != chain.end()) return ScheduleCapture::id;
    const auto key = lower(name);
    if (key == "group") return ScheduleCapture::retain;
    if ((key.size() > 3 && key.ends_with("msg")) || key == "elevatornotice" || key == "ninotice" ||
        key == "pathednotice")
        return ScheduleCapture::clone;
    for (const char* refused : {"array", "scriptinterpreter", "agentdata", "tohitsettings", "targsettings"})
        if (key == refused) return ScheduleCapture::refuse;
    // (Checkpoint 132ms4) A registered engine type whose script declaration has no
    // System parent (YEMEN1's GameCamera, 004c6a80: 003d8c50("cGameCamera",
    // 0017a190, 0, 1)): 003df54c +4c, then 003df580 +45, then the name compare
    // with "group" 003df5b0 (above), else 003df5c4 refuses.
    if (const auto flags = engine_script_type_flags(name)) {
        if (flags->id) return ScheduleCapture::id;
        if (flags->clone) return ScheduleCapture::clone;
        return ScheduleCapture::refuse;
    }
    throw std::runtime_error("Deferred argument type has no established capture: " + name);
}

std::optional<EngineScriptTypeFlags> engine_script_type_flags(const std::string& name) {
    // The 78 records of the booted registry (tests/script_type_flags_goldens.hpp).
    static constexpr const char* clone[] = {
        "WeaponVisChangeMsg", "DamageMsg", "EnableMsg", "DisableMsg", "Message", "BeamMsg", "AIActivatedMsg",
        "AIDeactivatedMsg", "AIDeadMsg", "AITimeElapsedMsg", "AIGOBJMsg", "AINodeMsg", "AIWaypointReachedMsg",
        "AIDestinationReachedMsg", "AINewAwarenessMsg", "AIWeaponFiredMsg", "AIPickupableAwarenessMsg",
        "AIBodyMovedMsg", "AIDisguiseTakenMsg", "AIUnconsciousMsg", "CrateInteractMsg", "InventoryMsg",
        "MenuChoiceMsg", "NINotice", "TimerExpiredMsg", "AddCheckpointMsg", "NotifyCheckpointMsg", "RespawnMsg",
        "GeneratorMessageBase", "SpawnedNPCMsg", "DespawnedNPCMsg", "GOBJTriggerEventMsg", "ElevatorNotice",
        "PathedNotice", "StartLevelMsg", "EndLevelMsg", "HumanSeenMsg", "HotboxMsg", "AIModeChangeMsg",
        "OutOfAmmoMsg"};
    static constexpr const char* id[] = {
        "GOBJ", "SubScript", "System", "SKYBOX_GOBJ", "VUM_GOBJ", "PARTICLE_GOBJ", "Tank", "NodeList", "FireObj",
        "Backpack", "NIEventOBJ", "NIEventLight", "GameGobjController", "Checkpoint", "ObjectiveMan", "Objective",
        "Generator", "GameGOBJ", "PathedGOBJ", "ElevatorGOBJ", "InteractGOBJ", "SoundGOBJ", "GameCamera",
        "GameLight", "GameSceneLight", "GameDirectionalLight", "EnvSettings", "Player", "Hotbox", "AI", "Node",
        "NPC"};
    static constexpr const char* neither[] = {"ScriptInterpreter", "Group", "AgentData", "Array", "ToHitSettings",
                                              "TargSettings"};
    const auto key = lower(name);
    for (const char* n : clone)
        if (lower(n) == key) return EngineScriptTypeFlags{true, false};
    for (const char* n : id)
        if (lower(n) == key) return EngineScriptTypeFlags{false, true};
    for (const char* n : neither)
        if (lower(n) == key) return EngineScriptTypeFlags{};
    return std::nullopt;
}

std::vector<ScriptContext::Handler> ScriptContext::select_handlers(const Receiver& receiver,
                                                                   const std::string& message_type, Word pass) {
    std::vector<Handler> result;
    scan(receiver, message_type, pass, [&](const Handler& handler) { result.push_back(handler); });
    return result;
}

bool ScriptContext::dispatch(const Receiver& receiver, const Value& message, const std::string& message_type) {
    const auto run = [&](const Handler& handler) { invoke_handler(handler, receiver, message); };
    const bool first = scan(receiver, message_type, 0, run);
    const bool second = scan(receiver, message_type, 1, run);
    return first || second;
}

bool ScriptContext::deliver(const Receiver& receiver, Message message) {
    const ObjectHandle handle{ObjectHandle::Kind::message, next_message_++};
    const auto type = message.type;
    messages_.emplace(handle.id, std::move(message));
    struct Expire {
        std::map<Word, Message>& messages;
        Word id;
        ~Expire() { messages.erase(id); }
    } expire{messages_, handle.id};
    return dispatch(receiver, handle, type);
}

std::optional<Value> ScriptContext::call_message_getter(const Script& script, const Function& function,
                                                        const ObjectHandle& handle) {
    const auto found = messages_.find(handle.id);
    if (found == messages_.end()) throw std::runtime_error("Message used after its delivery: " + function.name);
    if (function.parameters.size() != 1 || function.return_type == 4)
        throw std::runtime_error("Message native is not hosted: " + function.name);
    // The getter's overload must take this message's type or one of its bases.
    const auto chain = type_chain(found->second.type);
    const auto key = script.type_key(function.parameters[0]);
    if (std::none_of(chain.begin(), chain.end(), [&](const std::string& type) { return key == "object:" + type; }))
        throw std::runtime_error("Message getter parameter type differs from the message: " + function.name);
    const auto answer = found->second.getters.find(function.name);
    if (answer == found->second.getters.end())
        throw std::runtime_error("Message getter is not answered: " + function.name + "(" + found->second.type + ")");
    return answer->second;
}

std::optional<Address> ScriptContext::link_variable(const Script&, const Variable& variable) {
    return variable_address(variable.name);
}

std::optional<Address> ScriptContext::external_member(const ScriptType& type, Word member, const Value& object) {
    // The importing module's type table repeats the declaration; the link slot
    // (003df370: context +20, types[extra] +20, [member]) is the member's place
    // in the defining module's type, after its script base's members (type
    // +48, 003d6d84). A script type whose base is an engine type has none.
    if (member >= type.members.size()) throw std::runtime_error("Entity member outside the declaration: " + type.name);
    const auto* defined = defined_type(type.name);
    if (!defined) throw std::runtime_error("Entity script type is not defined by a loaded module (003de680): " + type.name);
    const auto& name = type.members[member].first;
    const auto slot = std::find_if(defined->members.begin(), defined->members.end(),
                                   [&](const auto& entry) { return entry.first == name; });
    if (slot == defined->members.end()) throw std::runtime_error("Entity member is not defined: " + type.name + "::" + name);
    return natives_.entity_member(object, static_cast<Word>(slot - defined->members.begin()));
}

bool ScriptContext::script_cast(const ScriptType& type, const Value& object) {
    // 003de540..003de588: a0 = holder +8 (or -1 without a holder); while a0 > 0
    // and a0 is not the type, a0 = the record's base (003d7590 +40).
    const auto bound = natives_.entity_type(object);
    if (!bound || bound->empty()) return false;
    // Checkpoint 132zf12: the walk is script_class_derives (interpreter
    // goldens of 003de4f8) over one id per name of the bound type's chain
    // (type_chain, numbered from 100); the root's base link is not positive.
    const auto chain = type_chain(*bound);
    std::int32_t target = 0;
    for (std::size_t i = 0; i < chain.size() && !target; ++i)
        if (lower(chain[i]) == lower(type.name)) target = static_cast<std::int32_t>(100 + i);
    return script_class_derives(100, target, [&](std::int32_t id) {
        const auto next = static_cast<std::size_t>(id - 100) + 1;
        return next < chain.size() ? static_cast<std::int32_t>(100 + next) : 0;
    });
}

const ScriptType* ScriptContext::defined_type(const std::string& name) const {
    const ScriptType* defined = nullptr;
    for (const auto& module : modules_) {
        if (!module->vm) continue;
        for (const auto& candidate : module->script.types)
            if (!candidate.external && lower(candidate.name) == lower(name)) {
                if (defined && defined->members != candidate.members)
                    throw std::runtime_error("Script type defined twice: " + name);
                defined = &candidate;
            }
    }
    if (defined && defined->base && defined_type(*defined->base) && !defined_type(*defined->base)->members.empty())
        throw std::runtime_error("Script type base members (003d6d84) are not hosted: " + name);
    return defined;
}

std::optional<std::size_t> ScriptContext::script_type_members(const std::string& name) {
    const auto* defined = defined_type(name);
    if (!defined) return std::nullopt;
    return defined->members.size();
}

bool ScriptContext::provides(const Script& script, const Function& function) {
    return definition(script, function) || manager_native(script, function) || group_native(script, function) ||
           array_native(script, function) || node_list_native(script, function) || natives_.provides(script, function);
}

}
