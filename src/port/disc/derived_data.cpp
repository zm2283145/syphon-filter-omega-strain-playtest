#include "derived_data.hpp"

#include "windows_hash.hpp"
#include "number_parse.hpp"  // port-only (132ps5c): from_chars of floats on the PS5

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace omega::derived {
namespace {

std::string hex32(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "%#x", value);
    return text;
}

std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("Derived data: truncated word");
    return std::uint32_t(data[offset]) | std::uint32_t(data[offset + 1]) << 8 | std::uint32_t(data[offset + 2]) << 16 |
           std::uint32_t(data[offset + 3]) << 24;
}
std::uint16_t u16(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("Derived data: truncated half");
    return std::uint16_t(data[offset] | data[offset + 1] << 8);
}

float bits_float(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
std::uint32_t float_bits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
// motion_graph_extract.py word_float / float_word.
double word_float(std::uint32_t bits) { return double(bits_float(bits)); }
std::uint32_t float_word(double value) { return float_bits(float(f32(value))); }

std::int64_t signed_word(std::uint32_t value) { return value < 0x80000000u ? std::int64_t(value) : std::int64_t(value) - 0x100000000LL; }

std::string sha256_bytes(std::span<const std::uint8_t> bytes) {
    std::istringstream input(std::string(bytes.begin(), bytes.end()));
    return sha256_stream(input);
}

// --- animation_root_reference.py quaternion helpers and motion_graph_extract.py constructors ---
using Quat = std::array<double, 4>;
using Vec3 = std::array<double, 3>;

std::array<double, 3> product_xyz(const Quat& a, const Quat& b) {
    std::array<double, 3> result{};
    for (int axis = 0; axis < 3; ++axis) {
        const int j = (axis + 1) % 3, k = (axis + 2) % 3;
        double value = f32(a[j] * b[k]);
        value = f32(value + f32(a[axis] * b[3]));
        value = f32(value + f32(b[axis] * a[3]));
        result[axis] = f32(value - f32(b[j] * a[k]));
    }
    return result;
}

Quat quaternion_product(const Quat& a, const Quat& b) {
    const auto xyz = product_xyz(a, b);
    double scalar = f32(f32(a[3] * b[3]) - f32(a[2] * b[2]));
    scalar = f32(scalar - f32(a[1] * b[1]));
    return {xyz[0], xyz[1], xyz[2], f32(scalar - f32(a[0] * b[0]))};
}

double clamped_product(double a, double b, double lower, double upper) {
    const double wide = a * b;
    if (wide >= upper) return upper;
    if (wide <= lower) return lower;
    return std::min(std::max(f32(wide), lower), upper);
}

Quat euler_orientation(const Vec3& value) {
    double halves[3];
    for (int i = 0; i < 3; ++i) halves[i] = f32(value[i] * 0.5);
    const double cx = f32(std::cos(halves[0])), cy = f32(std::cos(halves[1])), cz = f32(std::cos(halves[2]));
    const double sx = f32(std::sin(halves[0])), sy = f32(std::sin(halves[1])), sz = f32(std::sin(halves[2]));
    const double cycz = f32(cy * cz), sysz = f32(sy * sz), cysz = f32(cy * sz), sycz = f32(sy * cz);
    return {f32(f32(sx * cycz) - f32(cx * sysz)), f32(f32(cx * sycz) + f32(sx * cysz)),
            f32(f32(cx * cysz) + f32(sx * sycz)), f32(f32(cx * cycz) - f32(sx * sysz))};
}

// --- the constructor tracer (motion_graph_extract.py ConstructorMachine) ---
constexpr std::uint32_t stack_base = 0x70000000, stack_end = 0x70200000;
using Reg = std::optional<std::uint32_t>;
using Memory = std::unordered_map<std::uint32_t, std::uint8_t>;  // an absent byte is unknown

class Machine {
public:
    using Hook = std::function<void(Machine&, std::uint32_t)>;
    Machine(const ElfImage& image, Hook hook, std::shared_ptr<Memory> memory)
        : image(image), hook_(std::move(hook)), memory(std::move(memory)) {
        reg[0] = 0;
        reg[29] = stack_end - 0x100;
        reg[31] = 0xffffffffu;
    }
    const ElfImage& image;
    std::array<Reg, 32> reg{}, fp{};
    std::shared_ptr<Memory> memory;
    std::uint32_t pc = 0;

    std::uint32_t r(int index) const {
        if (!reg[index]) throw std::runtime_error("Unknown register r" + std::to_string(index) + " at " + hex32(pc));
        return *reg[index];
    }
    double f(int index) const {
        if (!fp[index]) throw std::runtime_error("Unknown FP register f" + std::to_string(index) + " at " + hex32(pc));
        const double result = word_float(*fp[index]);
        if (!std::isfinite(result)) throw std::runtime_error("Nonfinite constructor float");
        return result;
    }
    void store(std::uint32_t address, const std::vector<std::optional<std::uint8_t>>& data) {
        if (!(stack_base <= address && std::uint64_t(address) + data.size() <= stack_end))
            throw std::runtime_error("Constructor write outside stack: " + hex32(address));
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (data[i]) (*memory)[address + std::uint32_t(i)] = *data[i];
            else memory->erase(address + std::uint32_t(i));
        }
    }
    static std::vector<std::optional<std::uint8_t>> word_bytes(Reg value) {
        std::vector<std::optional<std::uint8_t>> data(4);
        if (value)
            for (int i = 0; i < 4; ++i) data[i] = std::uint8_t(*value >> (8 * i));
        return data;
    }
    std::vector<std::uint8_t> read(std::uint32_t address, std::uint32_t count) const {
        if (stack_base <= address && address < stack_end) {
            std::vector<std::uint8_t> values(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto at = memory->find(address + i);
                if (at == memory->end()) throw std::runtime_error("Uninitialized constructor stack: " + hex32(address));
                values[i] = at->second;
            }
            return values;
        }
        const auto bytes = image.read(address, count);
        return {bytes.begin(), bytes.end()};
    }
    Reg stack_word(std::uint32_t address) const {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto at = memory->find(address + i);
            if (at == memory->end()) return std::nullopt;
            value |= std::uint32_t(at->second) << (8 * i);
        }
        return value;
    }
    void volatile_reset() {
        for (int index = 1; index < 16; ++index) reg[index].reset();
        reg[24].reset();
        reg[25].reset();
        for (int index = 0; index < 20; ++index) fp[index].reset();
    }

    void ordinary(std::uint32_t word) {
        const std::uint32_t op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31, rd = (word >> 11) & 31;
        const std::int32_t immediate = std::int32_t(std::int16_t(word & 0xffff));
        const std::uint32_t funct = word & 63;
        if (word == 0) {
        } else if (op == 15) {
            reg[rt] = (word & 0xffff) << 16;
        } else if (op == 9 || op == 25) {
            reg[rt] = r(rs) + std::uint32_t(immediate);
        } else if (op == 13) {
            reg[rt] = r(rs) | (word & 0xffff);
        } else if (op == 0 && (funct == 33 || funct == 45 || funct == 37)) {
            const auto a = r(rs), b = r(rt);
            reg[rd] = funct == 37 ? (a | b) : (a + b);
        } else if (op == 43 || op == 63 || op == 31) {
            const std::size_t size = op == 43 ? 4 : op == 63 ? 8 : 16;
            const std::uint32_t address = r(rs) + std::uint32_t(immediate);
            auto data = word_bytes(reg[rt]);
            // Only lower 32 bits participate in the supported address tracer.
            for (std::size_t i = 4; i < size; ++i)
                data.push_back(rt == 0 ? std::optional<std::uint8_t>(0) : std::nullopt);
            store(address, data);
        } else if (op == 35 || op == 55 || op == 30) {
            const std::uint32_t address = r(rs) + std::uint32_t(immediate);
            if (!(stack_base <= address && address < stack_end))
                throw std::runtime_error("Unsupported non-stack constructor load");
            reg[rt] = stack_word(address);
        } else if (op == 57) {
            const std::uint32_t address = r(rs) + std::uint32_t(immediate);
            store(address, word_bytes(fp[rt]));
        } else if (op == 49) {
            const std::uint32_t address = r(rs) + std::uint32_t(immediate);
            if (!(stack_base <= address && address < stack_end))
                throw std::runtime_error("Unsupported non-stack constructor FP load");
            fp[rt] = stack_word(address);
        } else if (op == 17 && rs == 4) {
            fp[rd] = r(rt);
        } else if (op == 17 && rs == 16 && funct == 6) {
            if (!fp[rd]) throw std::runtime_error("Unknown constructor FP move");
            fp[(word >> 6) & 31] = fp[rd];
        } else if (op == 17 && rs == 16 && funct <= 3) {
            const double left = f(int(rd)), right = f(int(rt));
            if (funct == 3 && right == 0) throw std::runtime_error("Zero divisor in constructor arithmetic");
            const double value = funct == 0 ? left + right : funct == 1 ? left - right : funct == 2 ? left * right : left / right;
            fp[(word >> 6) & 31] = float_word(value);
        } else if (op == 17 && rs == 16 && funct == 7) {
            fp[(word >> 6) & 31] = float_word(-f(int(rd)));
        } else {
            char text[96];
            std::snprintf(text, sizeof text, "Unsupported constructor instruction %08x at %#x", unsigned(word), unsigned(pc));
            throw std::runtime_error(text);
        }
        reg[0] = 0;
    }

    std::uint64_t run(std::uint32_t start, std::optional<std::uint32_t> model = 0x60000000u,
                      std::uint64_t maximum_instructions = 100000) {
        if (model) reg[4] = *model;
        reg[31] = 0xffffffffu;
        std::uint32_t address = start;
        std::uint64_t executed = 0;
        while (executed < maximum_instructions) {
            pc = address;
            const std::uint32_t word = image.word(address);
            ++executed;
            const std::uint32_t op = word >> 26;
            if (op == 4 || op == 5) {
                const int rs = (word >> 21) & 31, rt = (word >> 16) & 31;
                const bool taken = (r(rs) == r(rt)) == (op == 4);
                const std::int64_t immediate = std::int16_t(word & 0xffff);
                const std::int64_t target = std::int64_t(address) + 4 + immediate * 4;
                if (!(std::int64_t(start) <= target && target < std::int64_t(start) + std::int64_t(maximum_instructions) * 4))
                    throw std::runtime_error("Constructor branch leaves bounded function region");
                pc = address + 4;
                ordinary(image.word(address + 4));
                ++executed;
                address = taken ? std::uint32_t(target) : address + 8;
            } else if (op == 3 || word == 0x03e00008u) {
                const bool is_return = word == 0x03e00008u;
                const std::uint32_t target =
                    is_return ? r(31) : (((address + 4) & 0xf0000000u) | ((word & 0x3ffffffu) << 2));
                if (!is_return) reg[31] = address + 8;
                pc = address + 4;
                ordinary(image.word(address + 4));
                ++executed;
                pc = address;
                if (is_return) {
                    if (target != 0xffffffffu) throw std::runtime_error("Constructor return address was not restored");
                    return executed;
                }
                hook_(*this, target);
                address += 8;
            } else {
                ordinary(word);
                address += 4;
            }
        }
        throw std::runtime_error("Constructor instruction budget exhausted");
    }

private:
    Hook hook_;
};

// --- GraphCollector: semantic objects at constructor temporary addresses ---
struct Child {
    std::int64_t node{};
    double start{}, end{};
};
struct Rule {
    double duration_start{}, duration_end{};
    bool reversible{}, smooth{};  // channel -1, redirect -1 and no intermediates: the only shape constructed
};
struct SelectorChild {
    std::int64_t node{};
    std::vector<Rule> rules;
};
struct Node {
    int type = 0;
    std::string name;
    Quat rotation{0, 0, 0, 1};
    std::string clip;
    std::int64_t source_word20 = 0;
    bool reverse = false, mirror = false;
    double range_start = 0, range_end = 1, phase_offset = 0, rate_scale = 1, duration = 0;
    std::int64_t group = -1, motion_bucket = 0;
    bool looping = false, align_sign = false;
    std::int64_t channel = 0;
    double angle_start = 0, angle_end = 0;
    Vec3 axis{};
    double axis_w = 0;
    std::int64_t smooth = 0, flag35 = 0;
    std::vector<Child> children;
    std::vector<SelectorChild> selector_children;
};
enum class Kind {
    String, NodeName, MotionName, Threshold, Quaternion, Vector3, Vector4, NodeT, NodeReference, Children, Rules,
    SelectorChildren, SelectorRule, SelectorChildT, ChildT, Range, GroupPair, Motion
};
struct Object {
    Kind kind{};
    std::string text;                    // String value, NodeName / MotionName name
    std::array<double, 4> values{};      // Threshold / Range (start, end), Quaternion, Vector3, Vector4
    Node node;                           // NodeT
    std::int64_t index{};                // NodeReference, Motion's node
    std::vector<std::uint32_t> entries;  // Children / Rules / SelectorChildren (constructor addresses)
    Rule rule;
    SelectorChild selector_child;
    Child child;
    std::array<std::uint32_t, 2> words{};  // GroupPair, Motion's group pair
};

class GraphCollector {
public:
    std::map<std::uint32_t, Object> objects;
    std::vector<Node> nodes;
    std::unordered_map<std::string, std::int64_t> names;
    std::vector<std::pair<std::string, Object>> motions;
    std::set<std::string> motion_names;
    std::map<std::string, double> clip_durations;
    std::int64_t scalar_channels = 0, selector_channels = 0;
    int depth = 0;

    Object& get(std::uint32_t address, Kind kind) {
        const auto at = objects.find(address);
        if (at == objects.end() || at->second.kind != kind)
            throw std::runtime_error("Expected constructor object at " + hex32(address));
        return at->second;
    }
    std::int64_t register_node(const std::string& name, const Node& node) {
        const std::string key = name.substr(0, 47);
        // Original type-specific copy raises model counts before the named insertion can reject a duplicate.
        if (node.type == 1 || node.type == 2) scalar_channels = std::max(scalar_channels, node.channel + 1);
        else if (node.type == 3) selector_channels = std::max(selector_channels, node.channel + 1);
        if (names.count(key)) return -1;
        const auto index = std::int64_t(nodes.size());
        names[key] = index;
        nodes.push_back(node);
        nodes.back().name = key;
        return index;
    }
    std::int64_t resolve(const std::string& name) const {
        const std::string key = name.substr(0, 47);
        const auto at = names.find(key);
        if (at == names.end())
            throw std::runtime_error("Unresolved graph node '" + key + "'; original index-zero fallback is not substituted");
        return at->second;
    }
    void seed(const SkasConfig& config, const std::vector<ClipHeader>& clips) {
        std::map<std::string, const ClipHeader*> available;
        auto lower = [](std::string s) {
            for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        for (const auto& item : clips) available[lower(item.filename)] = &item;
        if (available.size() != clips.size()) throw std::runtime_error("Ambiguous case-insensitive clip archive names");
        for (const auto& item : clips) clip_durations[item.filename] = item.duration;
        auto register_clip = [&](const std::string& name, const std::string& filename) {
            const auto at = available.find(lower(filename));
            if (at == available.end()) throw std::runtime_error("Missing SKAS clip '" + filename + "'");
            Node node;
            node.type = 0;
            node.clip = at->second->filename;
            node.duration = at->second->duration;
            node.looping = at->second->looping;
            register_node(name, node);
        };
        for (const auto& item : config.clips) register_clip(item.name, item.filename);
        // Original post-loader visits loaded asset order and lowercases names.
        for (const auto& item : clips) {
            const auto filename = lower(item.filename);
            const auto extension = filename.rfind(".ska");
            if (extension != std::string::npos) register_clip(filename.substr(0, extension), item.filename);
        }
    }

    std::string name_from(Machine& machine, std::uint32_t source, std::size_t limit) {
        std::string raw;
        if (source)
            for (std::size_t index = 0; index < limit; ++index) {
                const auto value = machine.read(source + std::uint32_t(index), 1)[0];
                if (!value) break;
                if (!(32 <= value && value < 127)) throw std::runtime_error("Unsupported non-ASCII graph name");
                raw.push_back(char(value));
            }
        return raw;
    }

    void operator()(Machine& machine, std::uint32_t target) {
        Reg result;
        Reg float_result;
        auto put = [&](std::uint32_t address, Object object) { objects[address] = std::move(object); };
        auto string_object = [](std::string value) {
            Object o;
            o.kind = Kind::String;
            o.text = std::move(value);
            return o;
        };
        auto named = [](Kind kind, std::string name) {
            Object o;
            o.kind = kind;
            o.text = std::move(name);
            return o;
        };
        if (target == 0x001f12d0) {
            const auto destination = machine.r(4), source = machine.r(5);
            put(destination, string_object(source ? machine.image.string(source) : std::string()));
            result = destination;
        } else if (target == 0x00207310) {
            const auto destination = machine.r(4);
            Object copy = get(machine.r(5), Kind::String);
            put(destination, copy);
            result = destination;
        } else if (target == 0x002072b0 || target == 0x001c00d0) {
            const auto destination = machine.r(4);
            const std::string first = get(machine.r(5), Kind::String).text;
            const std::string second =
                target == 0x002072b0 ? get(machine.r(6), Kind::String).text : machine.image.string(machine.r(6));
            if (first.size() + second.size() > 4096) throw std::runtime_error("Constructor string exceeds bounded length");
            put(destination, string_object(first + second));
            result = 0;
        } else if (target == 0x00209620) {
            auto& first = get(machine.r(4), Kind::String);
            const std::string second = get(machine.r(5), Kind::String).text;
            if (first.text.size() + second.size() > 4096) throw std::runtime_error("Constructor string exceeds bounded length");
            first.text += second;
        } else if (target == 0x00209570 || target == 0x002070f0) {
            const auto destination = machine.r(4);
            const bool node_name = target == 0x00209570;
            put(destination, named(node_name ? Kind::NodeName : Kind::MotionName,
                                   get(machine.r(5), Kind::String).text.substr(0, node_name ? 47 : 31)));
            result = destination;
        } else if (target == 0x00209510) {
            const auto destination = machine.r(4);
            Object copy = get(machine.r(5), Kind::NodeName);
            put(destination, copy);
            result = destination;
        } else if (target == 0x0018a890) {
            const auto destination = machine.r(4);
            put(destination, named(Kind::NodeName, ""));
            result = destination;
        } else if (target == 0x001ae1a0 || target == 0x002005b0) {
            const auto destination = machine.r(4), source = machine.r(5);
            const bool node_name = target == 0x001ae1a0;
            put(destination, named(node_name ? Kind::NodeName : Kind::MotionName, name_from(machine, source, node_name ? 47 : 31)));
            result = destination;
        } else if (target == 0x001f4460) {
            const auto destination = machine.r(4);
            Object o;
            o.kind = Kind::Threshold;
            o.values = {machine.f(12), machine.f(13), 0, 0};
            put(destination, o);
            result = destination;
        } else if (target == 0x0017f840) {
            const auto destination = machine.r(4);
            Object o;
            o.kind = Kind::Quaternion;
            o.values = {0.0, 0.0, 0.0, 1.0};
            put(destination, o);
            // Original identity initializer has no returned-pointer assignment.
        } else if (target == 0x0017de50) {
            const auto destination = machine.r(4);
            Object o;
            o.kind = Kind::Vector3;
            o.values = {machine.f(12), machine.f(13), machine.f(14), 0};
            put(destination, o);
            result = destination;
        } else if (target == 0x00201ef0) {
            const auto destination = machine.r(4);
            const auto& v = get(machine.r(5), Kind::Vector3).values;
            Object o;
            o.kind = Kind::Quaternion;
            o.values = euler_orientation({v[0], v[1], v[2]});
            put(destination, o);
            result = destination;
        } else if (target == 0x002019d0) {
            const auto destination = machine.r(4), key = machine.r(5), rotation = machine.r(7);
            const Node& source = nodes[std::size_t(resolve(get(key, Kind::NodeName).text))];
            if (source.type != 2) throw std::runtime_error("Rotated slider wrapper received non-slider source");
            Object o;
            o.kind = Kind::NodeT;
            o.node = source;
            o.node.name.clear();
            o.node.rotation = quaternion_product(get(rotation, Kind::Quaternion).values, source.rotation);
            put(destination, o);
            result = destination;
        } else if (target == 0x0018c5a0) {
            const auto index = resolve(get(machine.r(5), Kind::NodeName).text);
            const std::uint32_t address = 0x61000000u + std::uint32_t(index) * 0x100u;
            Object o;
            o.kind = Kind::NodeReference;
            o.index = index;
            put(address, o);
            result = address;
        } else if (target == 0x00132000) {
            const auto destination = machine.r(4);
            Object o;
            o.kind = Kind::Vector4;
            o.values = {machine.f(12), machine.f(13), machine.f(14), machine.f(15)};
            put(destination, o);
            result = destination;
        } else if (target == 0x001f0540) {
            const auto destination = machine.r(4);
            const Node& source = nodes[std::size_t(get(machine.r(6), Kind::NodeReference).index)];
            if (source.type != 0 && source.type != 1) throw std::runtime_error("Controlled leaf received non-leaf source");
            Object o;
            o.kind = Kind::NodeT;
            o.node = source;
            o.node.name.clear();
            const auto axis = get(machine.r(7), Kind::Vector4).values;
            o.node.type = 1;
            o.node.channel = machine.r(5);
            o.node.angle_start = machine.f(12);
            o.node.angle_end = machine.f(13);
            o.node.axis = {axis[0], axis[1], axis[2]};
            o.node.axis_w = axis[3];
            o.node.motion_bucket = 0;
            o.node.align_sign = (machine.r(8) & 255) != 0;
            put(destination, o);
            result = destination;
        } else if (target == 0x001f4470 || target == 0x001f3900 || target == 0x001f39f0) {
            // 4470 -> 44b0 -> 44f0 -> 39c0 -> 139070 initializes an empty circular list.
            const auto destination = machine.r(4);
            Object o;
            o.kind = target == 0x001f4470 ? Kind::Children : target == 0x001f3900 ? Kind::Rules : Kind::SelectorChildren;
            put(destination, o);
            result = destination;
        } else if (target == 0x001f3ab0) {
            const auto destination = machine.r(4);
            const double duration = machine.f(12);
            Object o;
            o.kind = Kind::SelectorRule;
            o.rule.duration_start = o.rule.duration_end = duration;
            o.rule.reversible = (machine.r(5) & 255) != 0;
            o.rule.smooth = (machine.r(6) & 255) != 0;
            put(destination, o);
            result = destination;
        } else if (target == 0x001f3880 || target == 0x001f3200) {
            const auto destination = machine.r(4), entry = machine.r(5);
            const bool rules = target == 0x001f3880;
            get(entry, rules ? Kind::SelectorRule : Kind::SelectorChildT);
            get(destination, rules ? Kind::Rules : Kind::SelectorChildren).entries.push_back(entry);
            result = destination;
        } else if (target == 0x001f32e0) {
            const auto destination = machine.r(4), key = machine.r(5), rules = machine.r(7);
            Object o;
            o.kind = Kind::SelectorChildT;
            o.selector_child.node = resolve(get(key, Kind::NodeName).text);
            for (const auto rule : get(rules, Kind::Rules).entries)
                o.selector_child.rules.push_back(get(rule, Kind::SelectorRule).rule);
            put(destination, o);
            result = destination;
        } else if (target == 0x001f2b70) {
            const auto destination = machine.r(4);
            Object o;
            o.kind = Kind::NodeT;
            o.node.type = 3;
            o.node.channel = machine.r(5);
            o.node.rotation = get(machine.r(7), Kind::Quaternion).values;
            for (const auto child : get(machine.r(6), Kind::SelectorChildren).entries)
                o.node.selector_children.push_back(get(child, Kind::SelectorChildT).selector_child);
            put(destination, o);
            result = destination;
        } else if (target == 0x001f43f0) {
            const auto destination = machine.r(4), key = machine.r(5), pair = machine.r(7);
            const auto child = resolve(get(key, Kind::NodeName).text);
            const auto& thresholds = get(pair, Kind::Threshold).values;
            Object o;
            o.kind = Kind::ChildT;
            o.child = {child, thresholds[0], thresholds[1]};
            put(destination, o);
            result = destination;
        } else if (target == 0x001f4370) {
            const auto destination = machine.r(4), child = machine.r(5);
            get(child, Kind::ChildT);
            get(destination, Kind::Children).entries.push_back(child);
            result = destination;
        } else if (target == 0x001f3ef0) {
            const auto destination = machine.r(4);
            const auto children = get(machine.r(6), Kind::Children).entries;
            Object o;
            o.kind = Kind::NodeT;
            o.node.type = 2;
            o.node.channel = machine.r(5);
            o.node.smooth = machine.r(7) & 255;
            o.node.flag35 = machine.r(8) & 255;
            o.node.rotation = get(machine.r(9), Kind::Quaternion).values;
            for (const auto child : children) o.node.children.push_back(get(child, Kind::ChildT).child);
            put(destination, o);
            result = destination;
        } else if (target == 0x003b43c0 || target == 0x003b3cc0 || target == 0x003b3b20 || target == 0x003b41e0 ||
                   target == 0x003b4010 || target == 0x003b3e40) {
            leaf_wrapper(machine, target);
            result = machine.r(4);
        } else if (target == 0x003b49a0) {
            const auto destination = machine.r(4), key = machine.r(6), node = machine.r(7);
            const auto index = register_node(get(key, Kind::NodeName).text, get(node, Kind::NodeT).node);
            machine.store(destination, Machine::word_bytes(std::uint32_t(index)));
        } else if (target == 0x0018a700) {
            const auto destination = machine.r(4), group = machine.r(5);
            Object o;
            o.kind = Kind::GroupPair;
            o.words = {0, group};
            put(destination, o);
            result = destination;
        } else if (target == 0x00200660) {
            const auto destination = machine.r(4), key = machine.r(5), group = machine.r(7);
            Object o;
            o.kind = Kind::Motion;
            o.index = resolve(get(key, Kind::NodeName).text);
            o.words = get(group, Kind::GroupPair).words;
            put(destination, o);
            result = destination;
        } else if (target == 0x003b4640) {
            const std::string name = get(machine.r(5), Kind::MotionName).text;
            if (!motion_names.count(name)) {
                motions.emplace_back(name, get(machine.r(6), Kind::Motion));
                motion_names.insert(name);
            }
        } else if (target == 0x002006b0 || target == 0x00206f90 || target == 0x00207340) {
            if (depth >= 8) throw std::runtime_error("Constructor nesting limit exceeded");
            Machine nested(machine.image, std::ref(*this), machine.memory);
            nested.reg = machine.reg;
            nested.fp = machine.fp;
            ++depth;
            try {
                nested.run(target, std::nullopt);
            } catch (...) {
                --depth;
                throw;
            }
            --depth;
            result = nested.reg[2];
        } else if (target == 0x003b28f0 || target == 0x001f3e80 || target == 0x001f3e10 || target == 0x001f3dc0 ||
                   target == 0x001f3c70 || target == 0x00200550 || target == 0x001f2990 || target == 0x001f2800 ||
                   target == 0x001f26b0 || target == 0x001f2540 || target == 0x001f23c0 || target == 0x00138350 ||
                   target == 0x001f04e0) {
            // These calls destroy only builder temporaries after registered node/slider copies.
            const auto destination = machine.r(4);
            objects.erase(destination);
            result = destination;
        } else if (target == 0x001f05d0) {
            const auto upper = machine.image.word(target), lower = machine.image.word(target + 4);
            if (upper >> 16 != 0x3c02 || lower >> 16 != 0x3442)
                throw std::runtime_error("Unsupported angular helper constant construction");
            const double factor = word_float(((upper & 0xffff) << 16) | (lower & 0xffff));
            float_result = float_word(f32(machine.f(12) * factor));
        } else if (target == 0x0017dd00) {
            float_result = machine.fp[12];
            (void)machine.f(12);
        } else if (target == 0x001efe10 || target == 0x001f1300) {
            const auto destination = machine.r(4);
            const bool unit = target == 0x001efe10;
            double start = std::max(0., machine.f(12));
            if (unit) start = std::min(start, 1.);
            double end = std::max(start, machine.f(13));
            if (unit) end = std::min(end, 1.);
            Object o;
            o.kind = Kind::Range;
            o.values = {start, end, 0, 0};
            put(destination, o);
            result = destination;
        } else {
            char text[96];
            std::snprintf(text, sizeof text, "Unsupported graph constructor hook %08x at %#x", unsigned(target), unsigned(machine.pc));
            throw std::runtime_error(text);
        }
        machine.volatile_reset();
        if (result) machine.reg[2] = result;
        if (float_result) machine.fp[0] = float_result;
    }

    void leaf_wrapper(Machine& machine, std::uint32_t target) {
        const auto destination = machine.r(4), key = machine.r(5);
        const Node source = nodes[std::size_t(resolve(get(key, Kind::NodeName).text))];
        if (source.type != 0) throw std::runtime_error("Leaf wrapper received non-leaf source");
        Node node = source;
        node.name.clear();
        if (target == 0x003b43c0) {
            node.group = signed_word(machine.r(7));
            node.motion_bucket = machine.r(8) & 255;
        } else if (target == 0x003b3cc0) {
            const auto mode = machine.r(7) & 255;
            if (mode == 1) {
                node.reverse = !node.reverse;
            } else if (mode == 2 || mode == 3) {
                node.mirror = !node.mirror;
                if (mode == 3) node.phase_offset = f32(std::fmod(f32(node.phase_offset + 0.5), 1.0));
            }
        } else if (target == 0x003b4010 || target == 0x003b3e40) {
            const auto selected = get(machine.r(9), Kind::Range).values;
            double start = selected[0], end = selected[1];
            const auto& v = get(machine.r(10), Kind::Vector3).values;
            const auto rotation = euler_orientation({v[0], v[1], v[2]});
            node.rotation = quaternion_product(rotation, source.rotation);
            node.reverse = source.reverse != (machine.r(7) != 0);
            node.mirror = source.mirror != (machine.r(8) != 0);
            const double scale = machine.f(12);
            node.rate_scale = f32(source.rate_scale * scale);
            double lower, upper;
            if (target == 0x003b4010) {
                if (source.reverse) {
                    const double s = f32(1. - end), e = f32(1. - start);
                    start = std::min(std::max(s, 0.), 1.);
                    end = std::min(std::max(e, start), 1.);
                }
                const double width = f32(source.range_end - source.range_start);
                lower = f32(source.range_start + f32(width * start));
                upper = f32(source.range_start + f32(width * end));
                lower = std::min(std::max(lower, 0.), 1.);
                upper = std::min(std::max(upper, lower), 1.);
                node.duration = f32(f32(source.duration * scale) * f32(selected[1] - selected[0]));
            } else {
                const auto at = clip_durations.find(source.clip);
                if (at == clip_durations.end()) throw std::runtime_error("Missing clip duration " + source.clip);
                const double original_duration = at->second;
                if (original_duration == 0) {
                    lower = 0.;
                    upper = 1.;
                } else {
                    const double reciprocal = f32(1. / original_duration);
                    lower = clamped_product(reciprocal, start, 0., 1.);
                    upper = clamped_product(reciprocal, end, lower, 1.);
                }
                node.duration = f32(f32(node.rate_scale * original_duration) * f32(upper - lower));
            }
            node.range_start = lower;
            node.range_end = upper;
            node.group = signed_word(machine.r(11));
            node.motion_bucket = machine.read(machine.r(29), 1)[0];
            if (machine.read(machine.r(29) + 8, 1)[0] == 1) node.looping = true;
        } else {
            const int vector_index = target == 0x003b3b20 ? 8 : 9;
            const auto& v = get(machine.r(vector_index), Kind::Vector3).values;
            const auto rotation = euler_orientation({v[0], v[1], v[2]});
            node.rotation = quaternion_product(rotation, node.rotation);
            if (target == 0x003b3b20) {
                node.motion_bucket = machine.r(9) & 255;
            } else {
                double offset = machine.f(12);
                const double scale = machine.f(13);
                if (source.reverse) offset = f32(1.0 - offset);
                offset = f32(std::fmod(f32(source.phase_offset + offset), 1.0));
                node.reverse = source.reverse != (machine.r(7) != 0);
                node.mirror = source.mirror != (machine.r(8) != 0);
                node.phase_offset = offset;
                node.rate_scale = f32(source.rate_scale * scale);
                node.duration = f32(f32(source.duration * scale) * f32(source.range_end - source.range_start));
                node.group = signed_word(machine.r(10));
                node.motion_bucket = machine.r(11) & 255;
                if (machine.read(machine.r(29), 1)[0] == 1) node.looping = true;
            }
        }
        Object o;
        o.kind = Kind::NodeT;
        o.node = std::move(node);
        objects[destination] = std::move(o);
    }
};

std::vector<std::uint32_t> builder_entries(const ElfImage& image) {
    std::vector<std::uint32_t> entries;
    Machine dispatcher(
        image,
        [&](Machine& machine, std::uint32_t target) {
            entries.push_back(target);
            machine.volatile_reset();
        },
        std::make_shared<Memory>());
    dispatcher.run(0x001efb60, 0x60000000u, 512);
    const std::set<std::uint32_t> unique(entries.begin(), entries.end());
    if (entries.empty() || entries.size() > 64 || unique.size() != entries.size())
        throw std::runtime_error("Unsupported constructor dispatcher shape");
    return entries;
}

bool is_python_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c' || (c >= '\x1c' && c <= '\x1f');
}

}  // namespace

double f32(double value) {
    const float result = float(value);
    if (!std::isfinite(result)) throw std::runtime_error("Nonfinite constructor arithmetic");
    return double(result);
}

std::string format_g9(double value) {
    char text[40];
    std::snprintf(text, sizeof text, "%.9g", value);
    return text;
}

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (byte < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\u%04x", unsigned(byte));
            out += escape;
        } else if (byte >= 0x80) {
            throw std::runtime_error("Derived data: non-ASCII name");
        } else {
            out.push_back(c);
        }
    }
    return out + "\"";
}

std::string crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (const char c : text) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

ElfImage::ElfImage(std::vector<std::uint8_t> data) : data_(std::move(data)) {
    static constexpr std::uint8_t magic[]{0x7f, 'E', 'L', 'F', 1, 1};
    if (data_.size() < 52 || !std::equal(std::begin(magic), std::end(magic), data_.begin()))
        throw std::runtime_error("Expected little-endian ELF32");
    const auto phoff = u32(data_, 28);
    const auto phsize = u16(data_, 42), phnum = u16(data_, 44);
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t at = std::size_t(phoff) + std::size_t(i) * phsize;
        segments_.push_back({u32(data_, at), u32(data_, at + 4), u32(data_, at + 8), u32(data_, at + 16)});
    }
}

std::span<const std::uint8_t> ElfImage::read(std::uint32_t address, std::uint32_t length) const {
    for (const auto& s : segments_) {
        if (s.type == 1 && s.vaddr <= address && std::uint64_t(address) + length <= std::uint64_t(s.vaddr) + s.filesz) {
            const std::uint64_t offset = std::uint64_t(s.offset) + (address - s.vaddr);
            if (offset + length > data_.size()) throw std::runtime_error("ELF segment outside the file");
            return std::span<const std::uint8_t>(data_).subspan(std::size_t(offset), length);
        }
    }
    throw std::runtime_error("Address outside file-backed executable: " + hex32(address));
}

std::uint32_t ElfImage::word(std::uint32_t address) const { return u32(read(address, 4), 0); }

std::string ElfImage::string(std::uint32_t address) const {
    std::string data;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const auto value = read(address + i, 1)[0];
        if (value == 0) return data;
        if (!(32 <= value && value < 127)) throw std::runtime_error("Not a printable registration name at " + hex32(address));
        data.push_back(char(value));
    }
    throw std::runtime_error("Registration name exceeds limit");
}

SkasConfig parse_skas(std::span<const std::uint8_t> data) {
    std::size_t size = data.size();
    while (size && data[size - 1] == 0) --size;
    std::string text;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] >= 0x80) throw std::runtime_error("SKAS is not ASCII");
        if (data[i] == 0) throw std::runtime_error("Embedded NUL in SKAS");
        text.push_back(char(data[i]));
    }
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < text.size();) {
        while (i < text.size() && is_python_space(text[i])) ++i;
        const auto start = i;
        while (i < text.size() && !is_python_space(text[i])) ++i;
        if (i > start) tokens.push_back(text.substr(start, i - start));
    }
    if (tokens.size() < 4 || tokens[0] != "Identifier:" || tokens[1] != "SKA_NAME_LIST" || tokens[2] != "Version_number:" ||
        tokens[3] != "1")
        throw std::runtime_error("Unsupported SKAS identifier/version");
    if (tokens.size() < 6 || tokens[4] != "Default_crossfade_duration:")
        throw std::runtime_error("Missing SKAS default duration");
    SkasConfig config;
    {
        std::string_view number = tokens[5];
        if (!number.empty() && number[0] == '+') number.remove_prefix(1);
        double value = 0;
        const auto [end, error] = omega::from_chars_number(number.data(), number.data() + number.size(), value);
        if (error != std::errc() || end != number.data() + number.size() || number.empty())
            throw std::runtime_error("Invalid SKAS default duration");
        config.default_crossfade_duration = f32(value);
    }
    std::size_t index = 6 + ((tokens.size() > 6 && tokens[6] == "(seconds)") ? 1 : 0);
    while (index < tokens.size()) {
        const auto& key = tokens[index];
        if (key == "Anchor_name:" && index + 1 < tokens.size()) {
            std::array<int, 4> anchor{};
            const auto& name = tokens[index + 1];
            for (std::size_t i = 0; i < 4 && i < name.size(); ++i) anchor[i] = static_cast<unsigned char>(name[i]);
            config.anchors.push_back(anchor);
            index += 2;
        } else if (key == "File_name:" && index + 2 < tokens.size()) {
            const auto& alias = tokens[index + 1];
            const auto& filename = tokens[index + 2];
            for (const char c : alias + filename)
                if (!(0x20 <= c && c < 0x7f)) throw std::runtime_error("Invalid SKAS name");
            config.clips.push_back({alias, filename + ".ska"});
            index += 3;
        } else {
            throw std::runtime_error("Unsupported SKAS token '" + key + "'");
        }
    }
    return config;
}

ClipHeader clip_header(std::string filename, std::span<const std::uint8_t> header) {
    if (header.size() < 20) throw std::runtime_error("Truncated animation clip header");
    const auto version = u32(header, 0), count = u32(header, 4), loop = u32(header, 16);
    const double period = double(bits_float(u32(header, 12)));
    if (version != 3 || !(0 < count && count <= 10000)) throw std::runtime_error("Unsupported graph clip header");
    return {std::move(filename), f32(f32(double(count)) * period), loop != 0};
}

std::string graph_manifest(const ElfImage& image, const SkasConfig& config, const std::vector<ClipHeader>& clips,
                           const Provenance& provenance, const std::string& source) {
    GraphCollector collector;
    collector.seed(config, clips);
    std::size_t builders = 0;
    for (const auto entry : builder_entries(image)) {
        // Separate C++ builder calls cannot read an earlier function's expired stack temporaries.
        collector.objects.clear();
        Machine machine(image, std::ref(collector), std::make_shared<Memory>());
        machine.run(entry);
        ++builders;
    }
    if (builders != 17) throw std::runtime_error("A native manifest requires all 17 builders to complete");
    const auto number = [](double v) { return format_g9(f32(v)); };
    const auto boolean = [](bool v) { return std::string(v ? "1" : "0"); };
    const double duration = f32(config.default_crossfade_duration);
    if (duration < 0) throw std::runtime_error("Negative default crossfade duration");
    std::ostringstream out;
    out << "OMEGA_GRAPH_MANIFEST 1\nCOMPLETE 1\nSOURCE " << json_quote(source) << "\nDEFAULT_CROSSFADE " << number(duration)
        << "\nPROVENANCE " << json_quote(provenance.iso_sha256) << ' ' << json_quote(provenance.executable_sha256) << ' '
        << json_quote(provenance.skas_sha256) << "\nANCHORS " << config.anchors.size() << '\n';
    for (const auto& key : config.anchors) out << "ANCHOR " << key[0] << ' ' << key[1] << ' ' << key[2] << ' ' << key[3] << '\n';
    out << "BUILDERS 17 17\nCHANNELS " << collector.scalar_channels << "\nSELECTORS " << collector.selector_channels
        << "\nNODES " << collector.nodes.size() << '\n';
    for (std::size_t index = 0; index < collector.nodes.size(); ++index) {
        const auto& node = collector.nodes[index];
        out << "NODE " << index << ' ' << node.type << ' ' << json_quote(node.name);
        for (const auto v : node.rotation) out << ' ' << number(v);
        out << '\n';
        if (node.type == 0 || node.type == 1) {
            out << "LEAF " << json_quote(node.clip) << ' ' << node.source_word20 << ' ' << boolean(node.reverse) << ' '
                << boolean(node.mirror);
            for (const auto v : {node.range_start, node.range_end, node.phase_offset, node.rate_scale, node.duration})
                out << ' ' << number(v);
            out << ' ' << node.group << ' ' << node.motion_bucket << ' ' << boolean(node.looping) << ' ' << boolean(node.align_sign)
                << '\n';
            if (node.type == 1) {
                out << "CONTROL " << node.channel << ' ' << number(node.angle_start) << ' ' << number(node.angle_end);
                for (const auto v : node.axis) out << ' ' << number(v);
                out << '\n';
            }
        } else if (node.type == 2) {
            out << "SLIDER " << node.channel << ' ' << boolean(node.smooth != 0) << ' ' << node.flag35 << ' '
                << node.children.size() << '\n';
            for (const auto& child : node.children)
                out << "CHILD " << child.node << ' ' << number(child.start) << ' ' << number(child.end) << '\n';
        } else if (node.type == 3) {
            out << "SELECTOR " << node.channel << ' ' << node.selector_children.size() << '\n';
            for (const auto& child : node.selector_children) {
                out << "CHILD " << child.node << ' ' << child.rules.size() << '\n';
                for (const auto& rule : child.rules)
                    out << "RULE " << number(rule.duration_start) << ' ' << number(rule.duration_end) << " -1 "
                        << boolean(rule.reversible) << ' ' << boolean(rule.smooth) << " -1 0\n";
            }
        } else {
            throw std::runtime_error("Unsupported node type in native manifest");
        }
        out << "ENDNODE\n";
    }
    out << "ROOTS " << collector.motions.size() << '\n';
    for (const auto& [name, motion] : collector.motions)
        out << "ROOT " << json_quote(name) << ' ' << motion.index << ' ' << motion.words[0] << ' ' << motion.words[1] << '\n';
    out << "END_MANIFEST\n";
    return out.str();
}

namespace {
void check_curve_damping(const ElfImage& image) {
    struct Setter {
        std::uint32_t start, setter, member, field;
    };
    for (const auto& s : {Setter{0x001b6bec, 0x001b7930, 0x20, 0x14}, Setter{0x001b6bfc, 0x001b7920, 0xc0, 0x50}}) {
        const auto load = image.word(s.start);
        const std::uint32_t expected[3]{(17u << 26) | (4u << 21) | (2u << 16) | (12u << 11), (3u << 26) | (s.setter >> 2),
                                        (9u << 26) | (16u << 21) | (4u << 16) | s.member};
        if ((load & 0xffff0000u) != ((15u << 26) | (2u << 16)) || image.word(s.start + 4) != expected[0] ||
            image.word(s.start + 8) != expected[1] || image.word(s.start + 12) != expected[2])
            throw std::runtime_error("Actor curve constructor literal/setter call differs");
        if (image.word(s.setter) != 0x03e00008u || image.word(s.setter + 4) != ((57u << 26) | (4u << 21) | (12u << 16) | s.field))
            throw std::runtime_error("Actor curve damping setter differs");
        const double value = double(bits_float((load & 0xffffu) << 16));
        if (!std::isfinite(value) || value < 0) throw std::runtime_error("Actor constructor curve damping outside supported domain");
    }
}

std::map<std::uint32_t, std::uint32_t> literal_stores(const ElfImage& image, std::uint32_t start, std::uint32_t end,
                                                       const std::set<std::uint32_t>& wanted,
                                                       std::optional<std::uint32_t> return_at = std::nullopt) {
    std::array<Reg, 32> registers{};
    registers[0] = 0;
    std::map<std::uint32_t, std::uint32_t> values;
    for (std::uint32_t address = start; address < end; address += 4) {
        const auto word = image.word(address);
        const std::uint32_t op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31, immediate = word & 65535;
        const std::uint32_t offset = std::uint32_t(std::int32_t(std::int16_t(immediate)));
        if (return_at && address == *return_at) {
            if (word != 0x03e00008u) throw std::runtime_error("Navigation initializer return differs");
            continue;
        }
        if (op == 15) {
            registers[rt] = immediate << 16;
        } else if (op == 13) {
            registers[rt] = registers[rs] ? Reg(*registers[rs] | immediate) : std::nullopt;
        } else if (op == 9) {
            registers[rt] = registers[rs] ? Reg(*registers[rs] + offset) : std::nullopt;
        } else if (op == 0 && (word & 63) == 45) {
            const auto rd = (word >> 11) & 31;
            registers[rd] = registers[rs] && registers[rt] ? Reg(*registers[rs] + *registers[rt]) : std::nullopt;
        } else if (op == 43) {
            if (!registers[rs]) throw std::runtime_error("Navigation initializer has unknown store destination");
            const std::uint32_t destination = *registers[rs] + offset;
            if (wanted.count(destination)) {
                if (values.count(destination) || !registers[rt])
                    throw std::runtime_error("Navigation tuning store is unknown or duplicated");
                values[destination] = *registers[rt];
            }
        } else {
            throw std::runtime_error("Unsupported navigation initializer instruction");
        }
        registers[0] = 0;
    }
    if (values.size() != wanted.size()) throw std::runtime_error("Navigation tuning stores are incomplete");
    return values;
}
}  // namespace

std::string actor_tuning(const ElfImage& image) {
    std::vector<std::uint32_t> addresses{0x0049ced8};
    for (std::uint32_t a = 0x0049cf48; a < 0x0049cfec; a += 8) addresses.push_back(a);
    const std::set<std::uint32_t> wanted(addresses.begin(), addresses.end());
    std::array<Reg, 32> registers{};
    registers[0] = 0;
    std::map<std::uint32_t, std::uint32_t> writes;
    auto instruction = [&](std::uint32_t address) {
        const auto word = image.word(address);
        const std::uint32_t op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31, imm = word & 65535;
        const std::uint32_t signed_imm = std::uint32_t(std::int32_t(std::int16_t(imm)));
        if (op == 15) {
            registers[rt] = imm << 16;
        } else if (op == 9 || op == 13) {
            const auto source = registers[rs];
            registers[rt] = source ? Reg(op == 9 ? *source + signed_imm : *source | imm) : std::nullopt;
        } else if (op == 43 || op == 57) {
            if (!registers[rs]) throw std::runtime_error("Unknown initializer store address");
            const std::uint32_t destination = *registers[rs] + signed_imm;
            if (wanted.count(destination)) {
                if (op != 43 || !registers[rt] || writes.count(destination))
                    throw std::runtime_error("Tuning initialization is not a unique literal store");
                writes[destination] = *registers[rt];
            }
        } else if (op == 63 && rs == 29) {
            // Save return register to unrelated stack.
        } else if (op == 49) {
            // Unrelated FP global load.
        } else if (op == 17 && (rs == 4 || (rs == 16 && (word & 63) == 2))) {
        } else {
            throw std::runtime_error("Unsupported tuning initializer instruction at " + hex32(address));
        }
        registers[0] = 0;
    };
    const std::uint32_t start = 0x004c6c40;
    for (std::uint32_t offset = 0; offset < 4096; offset += 4) {
        const std::uint32_t address = start + offset;
        const auto word = image.word(address);
        if (word >> 26 == 3) {
            if (((word & 0x3ffffffu) << 2) != 0x00100b30) throw std::runtime_error("Unexpected initialization prefix call");
            instruction(address + 4);
            if (writes.size() != wanted.size()) throw std::runtime_error("Incomplete tuning initialization prefix");
            std::string out = "OMEGA_ACTOR_TUNING 1\n";
            for (std::size_t i = 0; i < addresses.size(); ++i) {
                const double value = double(bits_float(writes.at(addresses[i])));
                if (!std::isfinite(value) || value < 0 || (addresses[i] == 0x0049ced8 && value == 0))
                    throw std::runtime_error("Unsupported actor tuning scalar at " + hex32(addresses[i]));
                out += (i ? " " : "") + format_g9(value);
            }
            check_curve_damping(image);
            return out + "\n";
        }
        instruction(address);
    }
    throw std::runtime_error("Unbounded tuning initializer prefix");
}

std::string navigation_tuning(const ElfImage& image) {
    auto values = literal_stores(image, 0x004cc78c, 0x004cc810, {0x004a0410, 0x004a0420}, 0x004cc808);
    const auto second = literal_stores(image, 0x004c6df4, 0x004c6e0c, {0x0049cff0});
    values.insert(second.begin(), second.end());
    double tuning[3];
    const std::uint32_t order[3]{0x004a0410, 0x004a0420, 0x0049cff0};  // ladder_offset, ladder_stop_distance, distance_scale
    for (int i = 0; i < 3; ++i) {
        tuning[i] = double(bits_float(values.at(order[i])));
        if (!std::isfinite(tuning[i]) || (tuning[i] != 0 && std::fabs(tuning[i]) < std::ldexp(1.0, -126)))
            throw std::runtime_error("Navigation tuning is outside finite normal-or-zero domain");
    }
    if (tuning[1] < 0 || tuning[2] <= 0) throw std::runtime_error("Invalid navigation distance tuning");
    return "OMEGA_NAVIGATION_TUNING 1\n" + format_g9(tuning[0]) + " " + format_g9(tuning[1]) + " " + format_g9(tuning[2]) + "\n";
}

DerivedFiles derived_files(const std::filesystem::path& folder) {
    return {folder / "graph.manifest", folder / "female.manifest", folder / "actor.tuning", folder / "navigation.tuning"};
}

namespace {
constexpr const char* stamp_name = "derived.stamp";
constexpr const char* stamp_version = "OMEGA_DERIVED_DATA 1";  // bump when an extractor port changes its output

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write " + path.string());
    out << text;
    out.close();
    if (!out) throw std::runtime_error("Write failed: " + path.string());
}
}  // namespace

DerivedFiles derive_game_data(DiscImage& disc, const DiscFingerprint& fingerprint, const std::filesystem::path& folder) {
    // Port-only (132md1): derived data always comes from the disc's own files, never a mod's.
    struct OriginalOnly {
        DiscImage& disc;
        bool previous;
        ~OriginalOnly() { disc.set_original_only(previous); }
    } original_only{disc, disc.original_only()};
    disc.set_original_only(true);
    const auto executable = disc.read(disc.file({fingerprint.boot_executable}));
    if (executable.size() != fingerprint.executable_size || sha256_bytes(executable) != fingerprint.executable_sha256)
        throw std::runtime_error("Unsupported embedded executable fingerprint");
    const ElfImage image(executable);
    std::vector<ClipHeader> clips;
    for (const auto& member : disc.archive_members({"GAMEDATA", "COMMON", "SKA.HOG"})) {
        if (member.extent.size < 20) throw std::runtime_error("Truncated animation clip header");
        clips.push_back(clip_header(member.name, disc.read({member.extent.offset, 20})));
    }
    std::string manifests[2];
    const char* sources[2]{"MALE.SKAS", "FEMALE.SKAS"};
    for (int i = 0; i < 2; ++i) {
        const auto skas = disc.archive_member({"GAMEDATA", "COMMON", "SKAS.HOG"}, sources[i]);
        const Provenance provenance{fingerprint.iso_sha256, fingerprint.executable_sha256, sha256_bytes(skas)};
        manifests[i] = graph_manifest(image, parse_skas(skas), clips, provenance, sources[i]);
    }
    const auto actor = actor_tuning(image);
    const auto navigation = navigation_tuning(image);
    std::filesystem::create_directories(folder);
    const auto files = derived_files(folder);
    std::filesystem::remove(folder / stamp_name);
    write_text(files.graph, crlf(manifests[0]));
    write_text(files.female_graph, crlf(manifests[1]));
    write_text(files.actor_tuning, crlf(actor));
    write_text(files.navigation_tuning, crlf(navigation));
    write_text(folder / stamp_name, std::string(stamp_version) + "\n" + fingerprint.iso_sha256 + "\n" + fingerprint.executable_sha256 + "\n");
    return files;
}

bool derived_data_current(const DerivedFiles& files, const DiscFingerprint& fingerprint) {
    namespace fs = std::filesystem;
    for (const auto& f : {files.graph, files.female_graph, files.actor_tuning, files.navigation_tuning})
        if (!fs::is_regular_file(f)) return false;
    std::ifstream in(files.graph.parent_path() / stamp_name, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str() == std::string(stamp_version) + "\n" + fingerprint.iso_sha256 + "\n" + fingerprint.executable_sha256 + "\n";
}

}  // namespace omega::derived
