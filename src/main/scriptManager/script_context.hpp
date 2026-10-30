#pragma once

#include "script.hpp"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace omega {

// The engine's script type records (*00554f28, registered by the static
// initializers through 003d8c50(name, ctor, a2, a3): +45 = a2, +4c = a3), by
// name, case-insensitive. The scheduler's argument preparation 003df400 reads
// +4c (capture the ID) and then +45 (clone). nullopt for a name the engine does
// not register.
struct EngineScriptTypeFlags {
    bool clone = false; // +45
    bool id = false;    // +4c
};
std::optional<EngineScriptTypeFlags> engine_script_type_flags(const std::string& name);

// The original script manager: modules loaded once by name (003e1720), one
// shared function/variable namespace checked by 003e1cf0, initializers run by
// 003e1f30, and one scheduler. Imports resolve to script definitions by name and
// parameter types; anything else is a native and goes to the host.
class ScriptContext final : private Host {
public:
    // Returns the module for a lower-case base name ("streets"), or nullopt
    // when the archive has no such member.
    using Loader = std::function<std::optional<Script>(const std::string& name)>;

    ScriptContext(Host& natives, Loader loader, std::uint64_t step_limit = 1000000,
                  std::size_t depth_limit = 128);
    ScriptContext(const ScriptContext&) = delete;
    ScriptContext& operator=(const ScriptContext&) = delete;

    // 003e0be0 (003e0c30's case-insensitive walk of the registry, the type's
    // +3c defined flag) then +48: the member count of the script-defined type
    // with this name among the loaded modules; nullopt when none defines it.
    std::optional<std::size_t> script_type_members(const std::string& name);
    // 003e1720: true when the module is already loaded or loads now.
    bool load(const std::string& name);
    // 003e1f30: initializers of each loaded module not yet run, in load order.
    void execute_init_code();
    // 003e1cf0: every import a module references resolves. Each failure is
    // logged as the original's "var %s not defined" / "func %s not defined".
    bool check_all_references();
    // 003e20a0: load init and utils, run their initializers, call init::Init,
    // then the reference check.
    bool start_level();

    // Engine (game::) variables are host cells; scripts link to them by name.
    void define_engine_variable(const std::string& name, Value value);
    const Value& engine_variable(const std::string& name) const;

    // Calls the unique definition with this name and arity among the modules.
    std::optional<Value> invoke(const std::string& name, const std::vector<Value>& arguments = {});
    // Whether a loaded module defines it with this arity (003e0f50's lookup).
    bool defines(const std::string& name, std::size_t arguments) const;
    // 003e1b60's entry lookup 003e0f50: the first defined function with this
    // arity whose name equals under 00128e80 (strcasecmp); its spelling.
    std::optional<std::string> entry_name(const std::string& name, std::size_t arguments) const;
    Vm& module(const std::string& name);
    std::vector<std::string> module_names() const;
    // The call instruction of the module running this script whose callee is
    // in progress (Vm::call_site): module name, calling function and word.
    // Port-only (132ob8).
    struct NativeCallSite {
        std::string module;
        std::string function;
        std::size_t word{};
    };
    std::optional<NativeCallSite> native_call_site(const Script& script) const;
    // A group's entries in order; nullopt is a null object.
    const std::vector<std::optional<Value>>& group_members(const Value& group);
    // 003d9f40(00555070, object): for every Group in creation order (the
    // interpreter's vector +4e4c, count +4e50, data +4e54, filled by 003de280),
    // the first entry with the object's ID is removed (003d9c80 then 003d9ad0),
    // then likewise the first entry with its +58 component's ID when it has one.
    void drop_from_groups(const Value& object, const std::optional<Value>& component);
    // An Array's live elements (float words), count of them in order.
    std::vector<Word> array_elements(const Value& array);
    // The Group a NodeList object wraps (+0x60); nullopt when the value is not
    // a NodeList this context created.
    std::optional<Value> node_list_group(const Value& node_list) const;
    ScriptScheduler& schedules() { return schedules_; }
    const ScriptScheduler& schedules() const { return schedules_; }
    // Loader and reference-check messages in order, worded as the original's.
    const std::vector<std::string>& log() const { return log_; }
    // Natives not called because an object or string argument was null, in order.
    const std::vector<std::string>& skipped_calls() const { return skipped_; }

    // A message receiver as 003e0510 sees it after the filter's projection
    // (vt+0x14): the object, its ID from vt+0x10 (an AI or player component
    // reports its owner's; 0xffffffff when it has none) and its script type
    // name (vt+0x34).
    struct Receiver {
        Value object;
        Word id{0xffffffffU};
        std::string type;
    };
    // A descriptor's selector, checked in this order: literal ID (word 6 >= 0),
    // variable (word 7), group (word 8), else generic by first-parameter type.
    enum class Selector { literal, variable, group, generic };
    struct Handler {
        std::string module;
        std::size_t function{};
        std::string name;
        Selector selector{};
        Word pass{};
    };
    // The handler map (context +0x4e5c, filled by 003dd2d0): every local
    // function with at least two parameters, keyed by the type key of its
    // second parameter, in link order (modules in load order, then table order).
    const std::map<std::string, std::vector<Handler>>& handlers();
    // 003e0510 for one pass without invoking: pass 0's single winner, or every
    // pass-1 match in list order.
    std::vector<Handler> select_handlers(const Receiver& receiver, const std::string& message_type, Word pass);
    // 003e1aa0 after the projection: pass 0 then pass 1 (always both); true
    // when any handler was selected. Handler return values are discarded.
    bool dispatch(const Receiver& receiver, const Value& message, const std::string& message_type);

    // A delivered message: its script type and the answers its getter natives
    // give, by native name (HotboxMsg: Action = byte +0x24, Who = actor +0x30,
    // WhoPlayer; SCRIPT_HOSTING.md 2.3). Only answered getters are hosted.
    struct Message {
        std::string type;
        std::map<std::string, Value> getters;
    };
    // dispatch() with a message handle that expires when the delivery returns
    // (the original deletes the message after 003e1aa0); a getter called on an
    // expired handle throws.
    bool deliver(const Receiver& receiver, Message message);

    static constexpr ObjectHandle interpreter{ObjectHandle::Kind::script_interpreter, 0};

private:
    struct Module {
        std::string name;
        Script script;
        std::unique_ptr<Vm> vm;
    };
    struct Definition {
        Module* module{};
        std::size_t index{};
    };
    class EngineCells final : public AddressSpace {
    public:
        Value& dereference(const Address& address) override;
        std::deque<std::pair<std::string, Value>> cells;
    };

    Host& natives_;
    Loader loader_;
    std::uint64_t step_limit_;
    std::size_t depth_limit_;
    ScriptScheduler schedules_;
    EngineCells engine_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<std::string> log_;
    std::vector<std::string> skipped_;
    // Groups made by opcode 63, owned by the context like the original's list
    // at interpreter+0x4e4c. Entries are object identities; nullopt is the
    // null object's ID (-1).
    std::deque<std::vector<std::optional<Value>>> groups_;
    // Arrays made by game::CreateArray (002689f0): fixed capacity of floats
    // and a live count, kept on the context like the original's list at 004f83e0.
    struct Array {
        std::vector<Word> data;
        std::size_t count{};
        bool removed{};   // game::RemoveArray (00268740)
    };
    std::deque<Array> arrays_;
    // NodeLists made by game::CreateNodeList: registered world objects (ID
    // category 0xf5) that each wrap a context Group, by ID.
    struct NodeList {
        RuntimeObjectHandle object;
        Value group;
    };
    std::map<Word, NodeList> node_lists_;
    std::map<Word, Message> messages_;
    Word next_message_{1};
    std::optional<Value> call_message_getter(const Script& script, const Function& function, const ObjectHandle& handle);
    // Rebuilt when a module is added (descriptors link at load).
    std::map<std::string, std::vector<Handler>> handlers_;
    std::map<std::string, std::optional<std::string>> type_bases_;
    std::set<std::string> conflicting_bases_;
    std::size_t linked_modules_{};

    void relink();
    bool scan(const Receiver& receiver, const std::string& message_type, Word pass,
              const std::function<void(const Handler&)>& selected);
    void invoke_handler(const Handler& handler, const Receiver& receiver, const Value& message);
    Value& selector_variable(const Script& script, Word index);
    std::vector<std::string> type_chain(const std::string& type);

    Module* find_module(const std::string& name) const;
    const ScriptType* defined_type(const std::string& name) const;
    std::optional<Definition> definition(const Script& script, const Function& import) const;
    bool manager_native(const Script& script, const Function& function) const;
    bool group_native(const Script& script, const Function& function) const;
    std::optional<Value> call_group_native(const Function& function, const std::vector<Value>& arguments);
    std::optional<Value> group_method(const std::string& name, std::vector<std::optional<Value>>& members,
                                      const std::vector<Value>& arguments);
    std::vector<std::optional<Value>>& group(const Value& value);
    bool node_list_native(const Script& script, const Function& function) const;
    std::optional<Value> call_node_list_native(const Function& function, const std::vector<Value>& arguments);
    const NodeList& node_list(const Value& value) const;
    bool array_native(const Script& script, const Function& function) const;
    std::optional<Value> call_array_native(const Function& function, const std::vector<Value>& arguments);
    Array& array(const Value& value);
    std::optional<Address> variable_address(const std::string& name) const;

    std::optional<Value> call(const Script& script, const Function& function,
                              const std::vector<Value>& arguments) override;
    Word enum_value(const std::string& name, Word member) override;
    Value object_from_id(Word raw_id) override;
    Word object_to_id(const Value& object) override;
    Word world_object_id(const RuntimeObjectHandle& object) override;
    std::optional<Address> link_variable(const Script& script, const Variable& variable) override;
    bool provides(const Script& script, const Function& function) override;
    Value create_group(const std::vector<Value>& members) override;
    Word group_size(const Value& group) override;
    Value group_entry(const Value& group, std::int32_t index) override;
    ScheduleCapture schedule_capture(const Script& script, Word type) override;
    std::optional<Address> external_member(const ScriptType& type, Word member, const Value& object) override;
    bool script_cast(const ScriptType& type, const Value& object) override;
    bool ai_policy(const Script& script, Word type);
};

}
