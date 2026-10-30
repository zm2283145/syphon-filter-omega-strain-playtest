#include "script.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace omega {
Value Host::object_from_id(Word) {
    throw std::runtime_error("Runtime object lookup is not hosted");
}

Word Host::object_to_id(const Value&) {
    throw std::runtime_error("Runtime object IDs are not hosted");
}

Word Host::world_object_id(const RuntimeObjectHandle&) {
    throw std::runtime_error("Object text (0040c1f0): a world object's vt+10 is not hosted");
}

std::optional<Address> Host::link_variable(const Script&, const Variable&) {
    return std::nullopt;
}

bool Host::provides(const Script&, const Function&) {
    return true;
}

Value Host::create_group(const std::vector<Value>&) {
    throw std::runtime_error("Script groups are not hosted");
}

Word Host::group_size(const Value&) {
    throw std::runtime_error("Script groups are not hosted");
}

Value Host::group_entry(const Value&, std::int32_t) {
    throw std::runtime_error("Script groups are not hosted");
}

RuntimeObjectHandle Host::create_world_object(Word) {
    throw std::runtime_error("World object creation (0040c240) is not hosted");
}

std::uint32_t Host::game_random() {
    throw std::runtime_error("The game RNG (00127dd0) is not hosted");
}

Host::ScheduleCapture Host::schedule_capture(const Script&, Word) {
    throw std::runtime_error("Deferred object argument type is unsupported");
}

std::optional<Address> Host::external_member(const ScriptType& type, Word, const Value&) {
    throw std::runtime_error("Entity script member (003de680) of " + type.name + " is not hosted");
}

std::optional<Address> Host::entity_member(const Value&, Word) {
    throw std::runtime_error("Entity script storage (003d9d20) is not hosted");
}

bool Host::script_cast(const ScriptType& type, const Value&) {
    throw std::runtime_error("Script type cast (003de4f8) to " + type.name + " is not hosted");
}

std::optional<std::string> Host::entity_type(const Value&) {
    throw std::runtime_error("Entity script holder type (+18 +8) is not hosted");
}

bool script_class_derives(std::int32_t id, std::int32_t target, const std::function<std::int32_t(std::int32_t)>& base) {
    while (id > 0 && target != id) id = base(id);   // 003de578 blez, 003de580 bne, 003de568 003d7590 -> +40
    return id > 0;                                  // 003de588 bgtz
}

bool null_handle(const Value& value) {
    if (const auto* runtime = std::get_if<RuntimeObjectHandle>(&value)) return runtime->generation == 0;
    if (const auto* handle = std::get_if<ObjectHandle>(&value))
        return handle->kind == ObjectHandle::Kind::objective && handle->id == 0;
    return false;
}

namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::size_t remaining() const { return bytes_.size() - offset_; }
    Word word() {
        const auto data = take(4);
        return Word(data[0]) | (Word(data[1]) << 8) | (Word(data[2]) << 16) | (Word(data[3]) << 24);
    }
    Word count() {
        const Word size = word();
        if (size > remaining() / 4) throw std::runtime_error("Script count exceeds remaining input");
        return size;
    }
    std::vector<Word> words() {
        const Word size = count();
        std::vector<Word> result;
        result.reserve(size);
        for (Word i = 0; i < size; ++i) result.push_back(word());
        return result;
    }
    std::optional<std::string> text() {
        const Word length = word();
        if (length == 0) return std::nullopt;
        const auto data = take(length);
        if (data.back() != 0) throw std::runtime_error("Script string is not terminated");
        std::string value;
        value.reserve(length - 1);
        for (std::size_t i = 0; i + 1 < data.size(); ++i) {
            if (data[i] == 0 || data[i] >= 128) throw std::runtime_error("Invalid ASCII script string");
            value.push_back(static_cast<char>(data[i]));
        }
        take((4 - (length % 4)) % 4);
        return value;
    }
    std::string name() {
        auto value = text();
        if (!value || value->empty()) throw std::runtime_error("Missing script metadata name");
        return *value;
    }
    void finish() {
        if (remaining() > 15) throw std::runtime_error("Unparsed script trailing data");
        for (const auto byte : take(remaining())) {
            if (byte != 0) throw std::runtime_error("Nonzero script trailing data");
        }
    }
private:
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > remaining()) throw std::runtime_error("Truncated script input");
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

bool has_extra(Word opcode) {
    switch (opcode) {
    case 23: case 24: case 47: case 48: case 57: case 58: case 66: return true;
    default: return false;
    }
}

Word number(const Value& value) {
    const auto* word = std::get_if<Word>(&value);
    if (!word) throw std::runtime_error("Expected a numeric script word");
    return *word;
}

bool is_handle(const Value& value) {
    return std::holds_alternative<ObjectHandle>(value) || std::holds_alternative<RuntimeObjectHandle>(value);
}

// Opcodes 3 and 5 test the raw word against zero; an object's word is its
// pointer, so only a null object is zero.
bool nonzero(const Value& value) {
    if (is_handle(value)) return !null_handle(value);
    return number(value) != 0;
}

// Opcodes 6 and 16 compare raw words. Two objects are equal when they are the
// same object; a null object equals the number 0 and no other number.
std::optional<bool> same_object(const Value& left, const Value& right) {
    if (!is_handle(left) && !is_handle(right)) return std::nullopt;
    const auto null = [](const Value& value) {
        if (is_handle(value)) return null_handle(value);
        if (number(value) != 0) throw std::runtime_error("An object was compared with a nonzero number");
        return true;
    };
    const bool left_null = null(left), right_null = null(right);
    if (left_null || right_null) return left_null && right_null;
    return left == right;
}

float finite_float(const Value& value) {
    const float result = word_float(number(value));
    if (!std::isfinite(result) || std::fpclassify(result) == FP_SUBNORMAL)
        throw std::runtime_error("Exceptional PS2 floating-point semantics are unsupported");
    return result;
}

// 003ddfc0 formats text in a 0x210-byte frame area at sp + 0x1c0; longer text
// would overrun the original, so it is refused.
constexpr std::size_t interpreter_text_limit = 512;

// A string operand: owned text, or the null word.
std::optional<std::string> string_operand(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* word = std::get_if<Word>(&value); word && *word == 0) return std::nullopt;
    throw std::runtime_error("Expected owned text or a null string");
}

Value pop(std::vector<Value>& stack) {
    if (stack.empty()) throw std::runtime_error("Operand stack underflow");
    Value value = std::move(stack.back());
    stack.pop_back();
    return value;
}

// cvt.w.s truncates toward zero and saturates (EE goldens).
std::int32_t cvt_w_s(double value) {
    if (value >= 2147483648.0) return std::numeric_limits<std::int32_t>::max();
    if (value <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}

// 003df660..003df680 (opcode 62) and 003dfca0..003dfcb8 (Reschedule): binary32
// 1000 * delay, then cvt.w.s (EE goldens tools/ee/gen_script_schedule_delay.py).
std::int32_t delay_ticks(const Value& value) {
    const float ticks = finite_float(value) * 1000.0F;
    if (!std::isfinite(ticks)) throw std::runtime_error("Exceptional PS2 floating-point semantics are unsupported");
    return cvt_w_s(ticks);
}

}

Word float_bits(float value) { return std::bit_cast<Word>(value); }
float word_float(Word value) { return std::bit_cast<float>(value); }
std::int32_t word_signed(Word value) { return std::bit_cast<std::int32_t>(value); }

std::string Script::type_key(Word type) const {
    if (type < 10) return "primitive:" + std::to_string(type);
    if (type < 100) {
        if (type - 10 >= enums.size()) throw std::runtime_error("Enum type index outside script table");
        return "enum:" + enums[type - 10].name;
    }
    if (type - 100 >= types.size()) throw std::runtime_error("Object type index outside script table");
    return "object:" + types[type - 100].name;
}

Script parse_script(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    Script script;
    script.version = reader.word();
    if (script.version != 7 && script.version != 8)
        throw std::runtime_error("Only script versions 7 and 8 are supported");
    script.code = reader.words();
    for (Word i = 0, count = reader.count(); i < count; ++i) script.strings.push_back(reader.text());
    for (Word i = 0, count = reader.count(); i < count; ++i) {
        ScriptType type;
        type.name = reader.name();
        type.external = reader.word() != 0;
        type.base = reader.text();
        for (Word j = 0, members = reader.count(); j < members; ++j) {
            auto name = reader.name();
            type.members.emplace_back(std::move(name), reader.word());
        }
        script.types.push_back(std::move(type));
    }
    for (Word i = 0, count = reader.count(); i < count; ++i) {
        ScriptEnum type;
        type.name = reader.name();
        type.external = reader.word() != 0;
        if (!type.external) {
            for (Word j = 0, members = reader.count(); j < members; ++j) {
                auto name = reader.name();
                type.members.emplace_back(std::move(name), reader.word());
            }
        }
        script.enums.push_back(std::move(type));
    }
    for (Word i = 0, count = reader.count(); i < count; ++i) {
        reader.word();
        reader.name();
    }
    for (Word i = 0, count = reader.count(); i < count; ++i) {
        Variable variable;
        variable.name = reader.name();
        variable.type = reader.word();
        variable.external = reader.word() != 0;
        variable.storage = reader.word();
        variable.initializer = reader.words();
        script.variables.push_back(std::move(variable));
    }
    for (Word i = 0, count = reader.count(); i < count; ++i) {
        Function function;
        function.name = reader.name();
        Word fields[10]{};
        for (auto& field : fields) field = reader.word();
        function.return_type = fields[0];
        function.external = fields[1] != 0;
        function.entry = fields[4];
        std::copy_n(fields + 5, 5, function.dispatch_fields.begin());
        function.parameters = reader.words();
        if (!function.external && function.entry >= script.code.size())
            throw std::runtime_error("Function entry outside script code");
        script.functions.push_back(std::move(function));
    }
    reader.finish();
    const auto boundaries = instruction_boundaries(script.code);
    for (const auto& function : script.functions) {
        if (!function.external && !boundaries[function.entry])
            throw std::runtime_error("Function entry points into an instruction operand");
    }
    for (const auto& variable : script.variables) instruction_boundaries(variable.initializer);
    return script;
}

std::vector<bool> instruction_boundaries(std::span<const Word> code) {
    std::vector<bool> boundaries(code.size(), false);
    std::vector<std::int64_t> branches;
    for (std::size_t pc = 0; pc < code.size();) {
        boundaries[pc] = true;
        const Word opcode = code[pc] & 0xffff;
        const auto immediate = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(code[pc] >> 16));
        if (opcode == 4 || opcode == 5) branches.push_back(static_cast<std::int64_t>(pc) + 1 + immediate);
        const std::size_t length = has_extra(opcode) ? 2 : 1;
        if (length > code.size() - pc) throw std::runtime_error("Truncated instruction operand");
        pc += length;
    }
    for (const auto target : branches) {
        if (target < 0 || static_cast<std::uint64_t>(target) >= code.size() || !boundaries[static_cast<std::size_t>(target)])
            throw std::runtime_error("Branch targets a non-instruction word");
    }
    return boundaries;
}

Vm::Vm(const Script& script, Host& host, std::uint64_t step_limit, std::size_t depth_limit)
    : Vm(script, host, nullptr, step_limit, depth_limit) {}

Vm::Vm(const Script& script, Host& host, ScriptScheduler& schedules, std::uint64_t step_limit,
       std::size_t depth_limit)
    : Vm(script, host, &schedules, step_limit, depth_limit) {}

Vm::Vm(const Script& script, Host& host, ScriptScheduler* shared, std::uint64_t step_limit,
       std::size_t depth_limit)
    : script_(script), host_(host), main_boundaries_(instruction_boundaries(script.code)),
      step_limit_(step_limit), depth_limit_(depth_limit),
      schedules_(shared ? shared : &own_schedules_) {
    if (script.version != 7 && script.version != 8) throw std::runtime_error("Unsupported VM script version");
    for (const auto& variable : script.variables) {
        if (!variable.external) globals_.try_emplace(variable.storage, Word{0});
    }
}

void Vm::initialize() {
    if (initialized_ == InitState::ready) return;
    if (initialized_ != InitState::pending) throw std::runtime_error("Script initialization is reentrant or previously failed");
    initialized_ = InitState::running;
    try {
        for (const auto& variable : script_.variables) {
            if (variable.external || variable.initializer.empty()) continue;
            const Function descriptor{"<initializer:" + variable.name + ">", 4, false, 0, {}};
            const auto boundaries = instruction_boundaries(variable.initializer);
            execute(descriptor, {}, variable.initializer, boundaries, 0, variable.initializer.size());
        }
        initialized_ = InitState::ready;
    } catch (...) {
        initialized_ = InitState::failed;
        throw;
    }
}

std::optional<Value> Vm::invoke(const std::string& name, const std::vector<Value>& arguments) {
    if (initialized_ != InitState::ready) throw std::runtime_error("Initialize the script before invoking an entry");
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < script_.functions.size(); ++i) {
        const auto& function = script_.functions[i];
        if (!function.external && function.name == name && function.parameters.size() == arguments.size()) {
            if (index) throw std::runtime_error("Ambiguous script entry: " + name);
            index = i;
        }
    }
    if (!index) throw std::runtime_error("No matching local script entry: " + name);
    return invoke_index(*index, arguments);
}

std::optional<Value> Vm::invoke_definition(std::size_t index, const std::vector<Value>& arguments) {
    if (initialized_ != InitState::ready) throw std::runtime_error("Initialize the script before invoking an entry");
    if (index >= script_.functions.size() || script_.functions[index].external)
        throw std::runtime_error("Not a defined script function");
    return invoke_index(index, arguments);
}

const Value& Vm::global(Word storage) const {
    const auto found = globals_.find(storage);
    if (found == globals_.end()) throw std::runtime_error("Unknown global storage");
    return found->second;
}

Value& Vm::dereference(const Address& address) {
    if (address.owner && address.owner != this) return address.owner->dereference(address);
    return local_dereference(address);
}

Address AddressSpace::member(const Address&, const ScriptType& type, Word) {
    throw std::runtime_error("Member access to " + type.name + " requires a host struct");
}

EngineStruct::EngineStruct(std::string type_name, std::vector<Field> fields)
    : type_name_(std::move(type_name)), fields_(std::move(fields)) {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (fields_[j].name == fields_[i].name) throw std::runtime_error("Duplicate struct field " + fields_[i].name);
        }
    }
}

const EngineStruct::Field& EngineStruct::field(const std::string& name) const {
    for (const auto& field : fields_) {
        if (field.name == name) return field;
    }
    throw std::runtime_error(type_name_ + " has no field " + name);
}

void EngineStruct::set_field(const std::string& name, Value value) {
    for (auto& field : fields_) {
        if (field.name == name) {
            field.value = std::move(value);
            return;
        }
    }
    throw std::runtime_error(type_name_ + " has no field " + name);
}

Value& EngineStruct::dereference(const Address& address) {
    if (address.owner != this || address.frame != 0 || address.slot < 0 ||
        static_cast<std::uint64_t>(address.slot) >= fields_.size())
        throw std::runtime_error("Invalid " + type_name_ + " address");
    return fields_[static_cast<std::size_t>(address.slot)].value;
}

// The original resolves a script member to the engine's registered offset by
// name when the type links; members here are matched by name and type.
Address EngineStruct::member(const Address& base, const ScriptType& type, Word index) {
    if (base != address()) throw std::runtime_error("Member access needs the " + type_name_ + " base address");
    if (type.name != type_name_ || !type.external)
        throw std::runtime_error("Struct type " + type.name + " does not match " + type_name_);
    if (index >= type.members.size()) throw std::runtime_error("Struct member index outside " + type.name);
    const auto& [name, member_type] = type.members[index];
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].name != name) continue;
        if (fields_[i].type != member_type) throw std::runtime_error("Struct member " + name + " type mismatch");
        return Address{0, static_cast<std::int64_t>(i), this};
    }
    throw std::runtime_error(type_name_ + " has no field " + name);
}

Value& Vm::local_dereference(const Address& address) {
    if (address.frame == 0) {
        if (address.slot < 0 || static_cast<std::uint64_t>(address.slot) > std::numeric_limits<Word>::max())
            throw std::runtime_error("Invalid global address");
        const auto found = globals_.find(static_cast<Word>(address.slot));
        if (found == globals_.end()) throw std::runtime_error("Unknown global address");
        return found->second;
    }
    const auto found = frames_.find(address.frame);
    if (found == frames_.end()) throw std::runtime_error("Expired frame address");
    auto& frame = *found->second;
    if (address.slot > 0) {
        if (static_cast<std::uint64_t>(address.slot) > frame.arguments.size())
            throw std::runtime_error("Argument address outside frame");
        return frame.arguments[static_cast<std::size_t>(address.slot - 1)];
    }
    if (address.slot == std::numeric_limits<std::int64_t>::min())
        throw std::runtime_error("Local address outside frame");
    const auto index = static_cast<std::uint64_t>(-address.slot);
    if (index >= frame.locals.size()) throw std::runtime_error("Local address outside frame");
    return frame.locals[static_cast<std::size_t>(index)];
}

std::optional<Value> Vm::invoke_index(std::size_t index, const std::vector<Value>& arguments) {
    if (index >= script_.functions.size()) throw std::runtime_error("Call index outside function table");
    const auto& function = script_.functions[index];
    if (arguments.size() != function.parameters.size()) throw std::runtime_error("Script argument count mismatch");
    if (function.external) {
        if (function.name == "game::Unschedule" || function.name == "game::Reschedule" ||
            function.name == "game::ClearSchedules") {
            const std::vector<Word> expected = function.name == "game::ClearSchedules" ?
                std::vector<Word>{} : function.name == "game::Unschedule" ?
                std::vector<Word>{1} : std::vector<Word>{1, 2};
            if (function.return_type != 4 || function.parameters != expected)
                throw std::runtime_error("Unsupported scheduler control signature");
            if (function.name == "game::ClearSchedules") schedules_->clear();
            else if (function.name == "game::Unschedule") schedules_->cancel(number(arguments[0]));
            else schedules_->reschedule(number(arguments[0]), delay_ticks(arguments[1]));
            return std::nullopt;
        }
        auto result = host_.call(script_, function, arguments);
        if ((function.return_type == 4) == result.has_value())
            throw std::runtime_error("Host callback return contract mismatch: " + function.name);
        // game::ResetLevel 0012f700 ends with 003dfb20 (the ClearSchedules body) after the host's steps.
        if (function.name == "game::ResetLevel" && function.parameters.empty()) schedules_->clear();
        return result;
    }
    std::size_t end = script_.code.size();
    for (const auto& other : script_.functions) {
        if (!other.external && other.entry > function.entry) end = std::min(end, std::size_t(other.entry));
    }
    return execute(function, arguments, script_.code, main_boundaries_, function.entry, end);
}

std::optional<Value> Vm::execute(const Function& function, const std::vector<Value>& arguments,
                                std::span<const Word> code, const std::vector<bool>& boundaries,
                                std::size_t start, std::size_t end) {
    if (frames_.size() >= depth_limit_) throw std::runtime_error("VM call-depth limit reached");
    if (frames_.empty()) invocation_start_ = steps_;
    Frame frame{next_frame_++, arguments, {}, false};
    frames_.emplace(frame.id, &frame);
    struct FrameCleanup {
        std::map<std::uint64_t, Frame*>& frames;
        std::uint64_t id;
        ~FrameCleanup() { frames.erase(id); }
    } cleanup{frames_, frame.id};
    std::vector<Value> stack;
    std::size_t pc = start;
    std::size_t instruction = start;
    auto indexed = [](std::int16_t index, std::size_t size) -> std::size_t {
        if (index < 0 || static_cast<std::size_t>(index) >= size) throw std::runtime_error("Instruction table index out of range");
        return static_cast<std::size_t>(index);
    };
    try {
        for (;;) {
            instruction = pc;
            if (steps_ - invocation_start_ >= step_limit_) throw std::runtime_error("VM instruction limit reached");
            if (pc < start || pc >= end || pc >= code.size() || !boundaries[pc])
                throw std::runtime_error("Instruction pointer outside function");
            ++steps_;
            const Word raw = code[pc++];
            const Word opcode = raw & 0xffff;
            const auto immediate = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw >> 16));
            Word extra = 0;
            if (has_extra(opcode)) {
                if (pc >= end) throw std::runtime_error("Instruction operand outside function");
                extra = code[pc++];
            }
            switch (opcode) {
            case 1:
                if (frame.entered || immediate < 0) throw std::runtime_error("Invalid frame entry");
                frame.entered = true;
                frame.locals.assign(static_cast<std::size_t>(immediate), Word{0});
                break;
            case 2: {
                std::optional<Value> result;
                if (function.return_type != 4) result = pop(stack);
                if (!stack.empty()) throw std::runtime_error("Unbalanced operand stack at return");
                if (immediate < 0 || std::size_t(immediate) != frame.locals.size() + arguments.size())
                    throw std::runtime_error("Return cleanup does not match frame size");
                return result;
            }
            case 3: stack.push_back(Word(!nonzero(pop(stack)))); break;
            case 4: case 5: {
                const bool jump = opcode == 4 || !nonzero(pop(stack));
                if (jump) {
                    const auto target = static_cast<std::int64_t>(instruction) + 1 + immediate;
                    if (target < 0 || static_cast<std::uint64_t>(target) >= code.size())
                        throw std::runtime_error("Branch outside code");
                    pc = static_cast<std::size_t>(target);
                }
                break;
            }
            case 6: case 8: case 10: case 12: case 14: case 16: {
                if (opcode == 6 || opcode == 16) {
                    const auto right_value = pop(stack);
                    const auto left_value = pop(stack);
                    if (const auto same = same_object(left_value, right_value)) {
                        stack.push_back(Word(opcode == 6 ? *same : !*same));
                        break;
                    }
                    stack.push_back(left_value);
                    stack.push_back(right_value);
                }
                const auto right = word_signed(number(pop(stack)));
                const auto left = word_signed(number(pop(stack)));
                bool result = false;
                if (opcode == 6) result = left == right;
                if (opcode == 8) result = left > right;
                if (opcode == 10) result = left >= right;
                if (opcode == 12) result = left < right;
                if (opcode == 14) result = left <= right;
                if (opcode == 16) result = left != right;
                stack.push_back(Word(result));
                break;
            }
            case 7: case 17: {
                const float right = finite_float(pop(stack));
                const float left = finite_float(pop(stack));
                const bool equal = std::fabs(left - right) < 0.001F;
                stack.push_back(Word(opcode == 7 ? equal : !equal));
                break;
            }
            case 9: case 11: case 13: case 15: {
                // 003dedd0/003dee20/003dee70/003deec0: the epsilon compares
                // (gt a-e > b, ge a+e > b, lt a+e < b, le a-e < b). Inputs are
                // normal or zero, so a +- e is never subnormal.
                const float right = finite_float(pop(stack));
                const float left = finite_float(pop(stack));
                const float epsilon = word_float(0x3a83126fU);
                const float shifted = opcode == 9 || opcode == 15 ? left - epsilon : left + epsilon;
                const bool result = opcode == 9 || opcode == 11 ? shifted > right : shifted < right;
                stack.push_back(Word(result));
                break;
            }
            case 18: {
                const auto index = indexed(immediate, script_.functions.size());
                std::vector<Value> values;
                for (std::size_t i = 0; i < script_.functions[index].parameters.size(); ++i)
                    values.push_back(pop(stack));
                struct CallSiteScope {
                    std::optional<CallSite>& site;
                    std::optional<CallSite> saved;
                    ~CallSiteScope() { site = saved; }
                } call_site_scope{call_site_, call_site_};
                call_site_ = CallSite{&function, instruction};
                auto result = invoke_index(index, values);
                if (result) stack.push_back(std::move(*result));
                break;
            }
            case 19: pop(stack); break;
            case 21: {
                const Address address{frame.id, immediate, this};
                dereference(address);
                stack.push_back(address);
                break;
            }
            case 22: {
                const auto& variable = script_.variables[indexed(immediate, script_.variables.size())];
                if (variable.external) {
                    auto linked = host_.link_variable(script_, variable);
                    if (!linked) throw std::runtime_error("Unlinked external variable: " + variable.name);
                    stack.push_back(*linked);
                } else stack.push_back(Address{0, variable.storage, this});
                break;
            }
            case 23: case 24: stack.push_back(extra); break;
            case 25: case 28: case 30: case 32: {
                const Word right = number(pop(stack));
                const Word left = number(pop(stack));
                Word result = 0;
                if (opcode == 25) result = left + right;
                if (opcode == 28) result = left - right;
                if (opcode == 30) result = left * right;
                if (opcode == 32) {
                    const auto divisor = word_signed(right);
                    const auto dividend = word_signed(left);
                    if (divisor == 0 || (dividend == std::numeric_limits<std::int32_t>::min() && divisor == -1))
                        throw std::runtime_error("Exceptional integer division is unsupported");
                    result = static_cast<Word>(dividend / divisor);
                }
                stack.push_back(result);
                break;
            }
            case 26: case 29: case 31: case 33: {
                const float right = finite_float(pop(stack));
                const float left = finite_float(pop(stack));
                float result = 0;
                if (opcode == 26) result = left + right;
                if (opcode == 29) result = left - right;
                if (opcode == 31) result = left * right;
                if (opcode == 33) {
                    if (right == 0) throw std::runtime_error("Exceptional floating-point division is unsupported");
                    result = left / right;
                }
                const Value word = float_bits(result);
                finite_float(word);
                stack.push_back(word);
                break;
            }
            case 35: case 36: {
                auto value = pop(stack);
                const auto address = pop(stack);
                const auto* reference = std::get_if<Address>(&address);
                if (!reference) throw std::runtime_error("Store requires an address");
                dereference(*reference) = std::move(value);
                break;
            }
            case 27: {
                // 003dea70: strcpy(left) + strcat(right) into the interpreter's
                // stack buffer; a null operand gives "" (the warning is a stub).
                const auto right = string_operand(pop(stack));
                const auto left = string_operand(pop(stack));
                if (!left || !right) { stack.push_back(std::string{}); break; }
                if (left->size() + right->size() >= interpreter_text_limit)
                    throw std::runtime_error("String concatenation exceeds the interpreter buffer");
                stack.push_back(*left + *right);
                break;
            }
            case 37: {
                // 003de7a8: the string store. Global slots retain the string in
                // the original's temporary-string table; values own copies here.
                auto value = pop(stack);
                string_operand(value);
                const auto address = pop(stack);
                const auto* reference = std::get_if<Address>(&address);
                if (!reference) throw std::runtime_error("String store requires an address");
                dereference(*reference) = std::move(value);
                break;
            }
            case 49: stack.push_back(std::to_string(word_signed(number(pop(stack))))); break;   // 003de868 "%d"
            case 50: {                                                                             // 003de8a8 "%.2f"
                const double value = finite_float(pop(stack));
                char text[64]{};
                const int length = std::snprintf(text, sizeof text, "%.2f", value);
                if (length < 0 || static_cast<std::size_t>(length) >= interpreter_text_limit)
                    throw std::runtime_error("Float text exceeds the interpreter buffer");
                stack.push_back(std::string(text, static_cast<std::size_t>(length)));
                break;
            }
            case 51: {                                                                             // 003de8f0
                const auto value = pop(stack);
                const auto* word = std::get_if<Word>(&value);
                if ((word && *word == 0) || null_handle(value)) {
                    stack.push_back(std::string("Null Obj ptr"));
                    break;
                }
                // vt+10 then 0040c1f0, which formats the returned ID word "(%d %d)" as (word & 0xff0000) >> 16 and
                // word & 0xffff into 0x571b80; 00128bd0 copies it as a format (no '%' left). A controller's vt+10
                // (player 004d9cf0, AI 004d9380) is 003ea590, which tail-calls its owner's (+30) vt+10: the actor's
                // (004da690) 0012ece0 returns &actor+c, the owner ID the handle carries (never null, so "Null ID"
                // never shows). A world object's vt+10 is the host's.
                const auto* runtime = std::get_if<RuntimeObjectHandle>(&value);
                if (!runtime) throw std::runtime_error("Object text (0040c1f0) of a non-runtime object is not hosted");
                const Word id = runtime->kind == RuntimeObjectHandle::Kind::world_object ? host_.world_object_id(*runtime) : runtime->id;
                stack.push_back("(" + std::to_string((id & 0xff0000U) >> 16) + " " + std::to_string(id & 0xffffU) + ")");
                break;
            }
            case 38: stack.push_back(Word{0} - number(pop(stack))); break;
            case 39: stack.push_back(float_bits(-finite_float(pop(stack)))); break;
            case 40: case 41: {
                const auto address = pop(stack);
                const auto* reference = std::get_if<Address>(&address);
                if (!reference) throw std::runtime_error("Load requires an address");
                stack.push_back(dereference(*reference));
                break;
            }
            case 42: {
                const auto address = pop(stack);
                const auto* reference = std::get_if<Address>(&address);
                if (!reference) throw std::runtime_error("String copy load requires an address");
                const auto& value = dereference(*reference);
                if (const auto* text = std::get_if<std::string>(&value)) stack.push_back(*text);
                else if (const auto* word = std::get_if<Word>(&value); word && *word == 0)
                    stack.push_back(Word{0});
                else throw std::runtime_error("String copy load requires owned text or null");
                break;
            }
            case 47: {
                // 003de608: base + the member offset from types[extra]; a null
                // base pushes 0 (the original warns through 003dff80).
                if (extra >= script_.types.size()) throw std::runtime_error("Struct type index out of range");
                const auto& type = script_.types[extra];
                const auto base = pop(stack);
                if (const auto* word = std::get_if<Word>(&base); word && *word == 0) {
                    stack.push_back(Word{0});
                    break;
                }
                const auto* reference = std::get_if<Address>(&base);
                if (!reference) throw std::runtime_error("Member access requires a struct address");
                if (immediate < 0) throw std::runtime_error("Negative struct member index");
                if (!reference->owner || reference->owner == this)
                    throw std::runtime_error("Member access to " + type.name + " requires a host struct");
                stack.push_back(reference->owner->member(*reference, type, static_cast<Word>(immediate)));
                break;
            }
            case 48: {
                // 003de680: pop the object (003dffe0); a null object warns
                // (003dff80, 004bd5d0) and pushes 0. Otherwise the member's
                // address in the object's entity storage; a null storage warns
                // (004bd5f0) and pushes the 0 address all the same.
                if (extra >= script_.types.size()) throw std::runtime_error("Entity type index out of range");
                const auto& type = script_.types[extra];
                const auto object = pop(stack);
                if (const auto* word = std::get_if<Word>(&object); (word && *word == 0) || null_handle(object)) {
                    stack.push_back(Word{0});
                    break;
                }
                if (immediate < 0) throw std::runtime_error("Negative entity member index");
                const auto address = host_.external_member(type, static_cast<Word>(immediate), object);
                if (address) stack.push_back(*address);
                else stack.push_back(Word{0});
                break;
            }
            case 66: {
                // 003de4f8: the type (003dc4f0 on the extra word), then pop the
                // object (003dffe0). A null object warns (003dff80, 004bd570)
                // and pushes 0; otherwise the object when its holder's type
                // walks (003d7590 +40) to the type, else a warning (003d7930,
                // 004bd590) and 0.
                if (extra < 100 || extra - 100 >= script_.types.size())
                    throw std::runtime_error("Type cast operand is not an object type");
                const auto& type = script_.types[extra - 100];
                auto object = pop(stack);
                if (const auto* word = std::get_if<Word>(&object); (word && *word == 0) || null_handle(object)) {
                    stack.push_back(Word{0});
                    break;
                }
                if (host_.script_cast(type, object)) stack.push_back(std::move(object));
                else stack.push_back(Word{0});
                break;
            }
            case 63: {
                // 003de280: the count, then that many objects popped top first;
                // a negative count loops zero times (signed compare).
                const auto count = word_signed(number(pop(stack)));
                std::vector<Value> members;
                for (std::int32_t i = 0; i < count; ++i) members.push_back(pop(stack));
                stack.push_back(host_.create_group(members));
                break;
            }
            case 64: {
                // 003de1d0 (checkpoint 132ms3c): the function (003df3f0), then pop the
                // group's slot address and read the group through it, then the
                // argument count and that many arguments, top first. The size (+4)
                // is read once; per index, entry 003d9990(group, i) goes first and
                // 003e0ab0 runs the function (its native null skip is the host's,
                // as for opcode 18). Nothing is pushed.
                const auto index = indexed(immediate, script_.functions.size());
                const auto slot = pop(stack);
                const auto* reference = std::get_if<Address>(&slot);
                if (!reference) throw std::runtime_error("Group call requires the group's address");
                const Value group = dereference(*reference);
                const auto count = word_signed(number(pop(stack)));
                std::vector<Value> values{Word{0}};
                for (std::int32_t i = 0; i < count; ++i) values.push_back(pop(stack));
                const auto size = word_signed(host_.group_size(group));
                struct CallSiteScope {
                    std::optional<CallSite>& site;
                    std::optional<CallSite> saved;
                    ~CallSiteScope() { site = saved; }
                } call_site_scope{call_site_, call_site_};
                call_site_ = CallSite{&function, instruction};
                for (std::int32_t i = 0; i < size; ++i) {
                    values[0] = host_.group_entry(group, i);
                    invoke_index(index, values);
                }
                break;
            }
            case 43: case 44: {
                const bool right = number(pop(stack)) != 0;
                const bool left = number(pop(stack)) != 0;
                stack.push_back(Word(opcode == 43 ? left || right : left && right));
                break;
            }
            case 45: stack.push_back(float_bits(static_cast<float>(word_signed(number(pop(stack)))))); break;
            case 46: {
                // 003df218: pop (003dffb0), cvt.w.s, push (003e0020).
                stack.push_back(static_cast<Word>(cvt_w_s(finite_float(pop(stack)))));
                break;
            }
            case 52: {
                // Original 003e02f0 registers a copied temporary; 003d75e0
                // preserves null rather than allocating an empty string.
                // Value owns the copy without reproducing raw-pointer identity.
                const auto& text = script_.strings[indexed(immediate, script_.strings.size())];
                if (text) stack.push_back(*text);
                else stack.push_back(Word{0});
                break;
            }
            case 58: {
                if (immediate < 10) throw std::runtime_error("Invalid enum type operand");
                const auto& type = script_.enums.at(static_cast<std::size_t>(immediate - 10));
                if (type.external) stack.push_back(host_.enum_value(type.name, extra));
                else {
                    if (extra >= type.members.size()) throw std::runtime_error("Enum member outside table");
                    stack.push_back(type.members[extra].second);
                }
                break;
            }
            case 59: break;
            case 60:
                // 003de5e0: pop the object, push 003e04c0's ID word.
                stack.push_back(host_.object_to_id(pop(stack)));
                break;
            case 61: case 65:
                // Preserve bit 31: the original helper selects raw object versus
                // virtual projection. The admitted host owns that decision.
                stack.push_back(host_.object_from_id(number(pop(stack))));
                break;
            case 62: {
                const auto delay = delay_ticks(pop(stack));
                const Word index = number(pop(stack));
                const Word count = number(pop(stack));
                if (index >= script_.functions.size()) throw std::runtime_error("Schedule function index out of range");
                const auto& target = script_.functions[index];
                if (target.dispatch_fields != std::array<Word,5>{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0})
                    throw std::runtime_error("Deferred receiver/group dispatch metadata is unsupported");
                if (count != target.parameters.size() || count > stack.size())
                    throw std::runtime_error("Schedule argument count mismatch");
                std::vector<Value> values;
                std::vector<std::size_t> resolved;
                values.reserve(count);
                bool refused = false;
                for (const auto type : target.parameters) {
                    auto value = pop(stack);
                    // 003df400 returns -1 at the first refused parameter; the
                    // rest were already popped by the opcode.
                    if (refused) continue;
                    if (type == 3 || type == 7) {
                        // Type 7 preserves a linked reference word in the
                        // original. This VM owns its symbolic key for the same
                        // immutable script lifetime; it is not a raw pointer.
                        if (const auto* word = std::get_if<Word>(&value); type == 3 && word && *word == 0) {
                            refused = true; // A null string: warning, no schedule.
                            continue;
                        }
                        if (!std::holds_alternative<std::string>(value))
                            throw std::runtime_error("Schedule requires owned text or a symbolic string reference");
                    } else if (type == 1 || type == 2 || (type >= 10 && type < 100)) {
                        if (type >= 10) script_.type_key(type);
                        number(value);
                        if (type == 2) finite_float(value);
                    } else if (type >= 100) {
                        script_.type_key(type);
                        switch (host_.schedule_capture(script_, type)) {
                        case Host::ScheduleCapture::keep: break;
                        case Host::ScheduleCapture::id:
                            value = host_.object_to_id(value);
                            resolved.push_back(values.size());
                            break;
                        case Host::ScheduleCapture::clone:
                            throw std::runtime_error("Deferred message arguments are not hosted");
                        case Host::ScheduleCapture::retain: {
                            const auto* group = std::get_if<ObjectHandle>(&value);
                            if (!group || group->kind != ObjectHandle::Kind::group)
                                throw std::runtime_error("Deferred group argument is not a group");
                            break;
                        }
                        case Host::ScheduleCapture::refuse: refused = true; continue;
                        }
                    } else throw std::runtime_error("Deferred argument type is unsupported");
                    values.push_back(std::move(value));
                }
                if (refused) {
                    stack.push_back(0xffffffffU);
                    break;
                }
                // Captures are owned by this nonmovable VM and destroyed with it.
                const auto handle = schedules_->enqueue(delay, [this, index, values = std::move(values),
                                                                resolved = std::move(resolved)] {
                    if (initialized_ != InitState::ready) throw std::runtime_error("Scheduled VM is not initialized");
                    auto arguments = values;
                    // 003df710 -> 003e0400: a vanished object comes back null
                    // (the null object's word, -1, never resolves).
                    for (const auto at : resolved) {
                        const Word id = std::get<Word>(values[at]);
                        arguments[at] = id == 0xffffffffU ? Value(Word{0}) : host_.object_from_id(id);
                    }
                    // 003e0ab0's native null skip is the host's, as for opcode 18.
                    invoke_index(index, arguments); // A deferred return value is discarded.
                });
                stack.push_back(Word(handle)); // Both supported versions are >= 7.
                break;
            }
            case 67: {
                const auto& text = script_.strings[indexed(immediate, script_.strings.size())];
                if (!text) throw std::runtime_error("Null string reference is unsupported");
                stack.push_back(*text);
                break;
            }
            default: throw std::runtime_error("Unsupported opcode " + std::to_string(opcode));
            }
            if (stack.size() > 65536) throw std::runtime_error("Operand stack limit reached");
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(function.name + " at word " + std::to_string(instruction) + ": " + error.what());
    }
}

}
