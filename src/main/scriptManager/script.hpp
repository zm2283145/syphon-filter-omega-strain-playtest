#pragma once

#include "script_scheduler.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace omega {

using Word = std::uint32_t;

struct ScriptType {
    std::string name;
    bool external{};
    std::optional<std::string> base;
    std::vector<std::pair<std::string, Word>> members;
};

struct ScriptEnum {
    std::string name;
    bool external{};
    std::vector<std::pair<std::string, Word>> members;
};

struct Variable {
    std::string name;
    Word type{};
    bool external{};
    Word storage{};
    std::vector<Word> initializer;
};

struct Function {
    std::string name;
    Word return_type{};
    bool external{};
    Word entry{};
    std::vector<Word> parameters;
    // Raw fixed metadata words 5..9: receiver/group selectors and dispatch flag.
    std::array<Word, 5> dispatch_fields{0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU, 0};
};

struct Script {
    Word version{};
    std::vector<Word> code;
    std::vector<std::optional<std::string>> strings;
    std::vector<ScriptType> types;
    std::vector<ScriptEnum> enums;
    std::vector<Variable> variables;
    std::vector<Function> functions;

    std::string type_key(Word type) const;
};

Script parse_script(std::span<const std::uint8_t> bytes);
std::vector<bool> instruction_boundaries(std::span<const Word> code);
Word float_bits(float value);
float word_float(Word value);
std::int32_t word_signed(Word value);

class AddressSpace;

struct Address {
    std::uint64_t frame{};
    std::int64_t slot{};
    // Null means the VM that dereferences it; linked modules set the owner.
    AddressSpace* owner{};
    bool operator==(const Address&) const = default;
};

struct ObjectHandle {
    // message: a delivered message, valid only while its delivery runs.
    enum class Kind { objective_manager, objective, script_interpreter, group, array, message };
    Kind kind{};
    Word id{};
    bool operator==(const ObjectHandle&) const = default;
};

// Runtime identities are separate from diagnostic objective-manager handles.
// Generation zero represents null; generations otherwise belong to one registry.
struct RuntimeObjectHandle {
    enum class Kind { world_object, ai, player };
    Kind kind{};
    Word id{};
    std::uint64_t generation{};
    bool operator==(const RuntimeObjectHandle&) const = default;
};

using Value = std::variant<Word, std::string, Address, ObjectHandle, RuntimeObjectHandle>;

// A typed null object: the generation-0 runtime handle or the Objective handle
// 0 (the ObjectiveMan handle 0 is the live manager). The original's null is a
// zero pointer; a numeric Word 0 is checked separately by each caller.
bool null_handle(const Value& value);

// Storage an address belongs to: a VM (frames and its own globals) or a host
// cell table such as a script context's engine variables.
class AddressSpace {
public:
    virtual Value& dereference(const Address& address) = 0;
    // 003de608 (opcode 47): the address of member `index` of `type` in the
    // struct at `base`. Only host structs have members; VM frames and globals
    // never take address arithmetic, so the default throws.
    virtual Address member(const Address& base, const ScriptType& type, Word index);
protected:
    ~AddressSpace() = default;
};

// An engine-registered struct (003d8c50/003d88e0): named word fields at
// 4-byte offsets. Address{0, 0, this} is the struct; member i is slot i.
class EngineStruct final : public AddressSpace {
public:
    struct Field {
        std::string name;
        Word type{};
        Value value;
    };
    EngineStruct(std::string type_name, std::vector<Field> fields);
    EngineStruct(const EngineStruct&) = delete;
    EngineStruct& operator=(const EngineStruct&) = delete;

    Address address() { return Address{0, 0, this}; }
    const std::string& type_name() const { return type_name_; }
    const Field& field(const std::string& name) const;
    // Port-only (checkpoint 132md16, a mod's [tohit] data overrides): the field's value; throws if there is none.
    void set_field(const std::string& name, Value value);
    Value& dereference(const Address& address) override;
    Address member(const Address& base, const ScriptType& type, Word index) override;
private:
    std::string type_name_;
    std::vector<Field> fields_;
};

class Host {
public:
    virtual ~Host() = default;
    virtual std::optional<Value> call(const Script& script, const Function& function,
                                      const std::vector<Value>& arguments) = 0;
    virtual Word enum_value(const std::string& name, Word member) = 0;
    virtual Value object_from_id(Word raw_id);
    // 003e04c0 (opcode 60): an object's ID word, with bit 31 set when the
    // object is the registered one itself and clear for a controller reporting
    // its owner's ID; the null object gives 0xffffffff. The default throws.
    virtual Word object_to_id(const Value& object);
    // 003de8f0 (opcode 51): the ID word a non-null world object's vt+10
    // returns, which 0040c1f0 formats. The default throws.
    virtual Word world_object_id(const RuntimeObjectHandle& object);
    // Address of an external (imported or engine) variable; nullopt = unlinked.
    virtual std::optional<Address> link_variable(const Script& script, const Variable& variable);
    // Whether an import is registered (the original registers every native, so
    // by default all are; an unhosted one throws when called).
    virtual bool provides(const Script& script, const Function& function);
    // 003de280 (opcode 63): a new group holding `members` in pop order (the
    // last pushed first). The default host has no groups and throws.
    virtual Value create_group(const std::vector<Value>& members);
    // 0040c240 + 003cf0e0 + 0040c9d0: a new engine object's ID in this
    // category (the category's counter, skipping IDs already visible), registered
    // as a plain world object. The default host has no registry and throws.
    virtual RuntimeObjectHandle create_world_object(Word category);
    // 00127dd0: one draw of the shared game RNG (Group / NodeList Randomize,
    // 003d9a00). The default host has no RNG and throws.
    virtual std::uint32_t game_random();
    // 003df400 (opcode 62): how a deferred call captures an argument whose
    // declared type is >= 100. keep: the value as is (003e0a70's AI policy,
    // where the value is already an ID word); id: 003e04c0's ID word, resolved
    // again by 003e0400 at replay (type flag +0x4c); clone: a message copy
    // (flag +0x45); retain: a group; refuse: the whole schedule returns -1.
    // The default host captures no objects and throws.
    enum class ScheduleCapture { keep, id, clone, retain, refuse };
    virtual ScheduleCapture schedule_capture(const Script& script, Word type);
    // 003de680 (opcode 48): the address of member `member` of the script
    // class `type` (an entity script type such as NPC_Patroler) in the
    // non-null `object`'s entity storage: the type's link slot, then 003d9d20
    // (holder +0 plus 4 * slot) on the object's script holder (+18). nullopt:
    // the holder's storage is null. The default host has no entity scripts and
    // throws.
    virtual std::optional<Address> external_member(const ScriptType& type, Word member, const Value& object);
    // 003d9d20 for the object's script holder (+18, bound by 003d9d30): the
    // address of storage word `slot`; nullopt when the storage is null. The
    // default host binds no entity scripts and throws.
    virtual std::optional<Address> entity_member(const Value& object, Word slot);
    // 003de4f8 (opcode 66, checkpoint 132el7): whether the non-null `object`'s
    // script holder (+18) type (+8; -1 for no holder) is `type` or derives
    // from it (the type records' base links, 003d7590 +40). The default host
    // binds no entity scripts and throws.
    virtual bool script_cast(const ScriptType& type, const Value& object);
    // The script type bound into the object's holder (+18 +8): "" when the
    // binding found no type (+8 stays -1), nullopt when the object has no
    // holder (+18 null). The default host throws.
    virtual std::optional<std::string> entity_type(const Value& object);
};

// 003de540..003de58c (opcode 66, checkpoint 132zf12): from a class id (the
// holder's +8, -1 without a holder) along the base links (003d7590(id) +40)
// while the id is positive and not the target; true when it stopped at the
// target (the object is kept), false at an id <= 0 (0 is pushed).
// Interpreter goldens of the case 003de4f8: tests/script_dynamic_cast_goldens.hpp.
bool script_class_derives(std::int32_t id, std::int32_t target, const std::function<std::int32_t(std::int32_t id)>& base);

class Vm final : public AddressSpace {
public:
    Vm(const Script& script, Host& host, std::uint64_t step_limit = 1000000,
       std::size_t depth_limit = 128);
    // Shares one scheduler between the modules of a script context.
    Vm(const Script& script, Host& host, ScriptScheduler& schedules,
       std::uint64_t step_limit = 1000000, std::size_t depth_limit = 128);
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&) = delete;
    Vm& operator=(Vm&&) = delete;
    void initialize();
    bool initialized() const { return initialized_ == InitState::ready; }
    std::optional<Value> invoke(const std::string& name, const std::vector<Value>& arguments = {});
    // A defined function by table index (cross-module calls resolve the index).
    std::optional<Value> invoke_definition(std::size_t index, const std::vector<Value>& arguments);
    const Value& global(Word storage) const;
    Value& dereference(const Address& address) override;
    std::uint64_t steps() const { return steps_; }
    const Script& script() const { return script_; }
    // One VM lifetime and an explicit script-clock delta; not a global mission clock.
    ScriptScheduler& schedules() { return *schedules_; }
    // The call instruction (opcode 18) whose callee is running: the calling
    // function and the instruction's word index in the module's code. nullopt
    // outside such a call (a scheduled call, 0x3e0ab0, has no call site).
    // Port-only bookkeeping for call-site-scoped native answers (132ob8).
    struct CallSite {
        const Function* caller{};
        std::size_t word{};
    };
    const std::optional<CallSite>& call_site() const { return call_site_; }

private:
    struct Frame {
        std::uint64_t id{};
        std::vector<Value> arguments;
        std::vector<Value> locals;
        bool entered{};
    };

    const Script& script_;
    Host& host_;
    std::map<Word, Value> globals_;
    std::map<std::uint64_t, Frame*> frames_;
    std::vector<bool> main_boundaries_;
    std::uint64_t next_frame_{1};
    std::uint64_t steps_{};
    // Port-side runaway guard (the EE VM has none): counted per outermost
    // execute, so a long play session does not accumulate towards it.
    std::uint64_t invocation_start_{};
    std::uint64_t step_limit_;
    std::size_t depth_limit_;
    enum class InitState { pending, running, ready, failed };
    InitState initialized_{InitState::pending};
    ScriptScheduler own_schedules_;
    ScriptScheduler* schedules_;
    std::optional<CallSite> call_site_;

    Vm(const Script& script, Host& host, ScriptScheduler* shared, std::uint64_t step_limit,
       std::size_t depth_limit);
    Value& local_dereference(const Address& address);
    std::optional<Value> execute(const Function& function, const std::vector<Value>& arguments,
                                 std::span<const Word> code, const std::vector<bool>& boundaries,
                                 std::size_t start, std::size_t end);
    std::optional<Value> invoke_index(std::size_t index, const std::vector<Value>& arguments);
};

}
