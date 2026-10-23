#include "animation_graph_manifest.hpp"
#include <charconv>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace omega {
namespace {
constexpr std::size_t maximum_bytes = 16 * 1024 * 1024;
constexpr std::size_t maximum_entries = 100000;
class Reader {
public:
    explicit Reader(std::string text) : input_(std::move(text)) { input_.imbue(std::locale::classic()); }
    void tag(std::string_view expected) {
        if (token() != expected) throw std::runtime_error("Animation graph manifest: expected " + std::string(expected));
    }
    std::string token() {
        std::string result;
        if (!(input_ >> result) || result.size() > 128)
            throw std::runtime_error("Animation graph manifest: missing or oversized token");
        return result;
    }
    template<class Number> Number number() {
        const auto text = token();
        Number result{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("Animation graph manifest: invalid number");
        return result;
    }
    std::size_t count(std::size_t maximum) {
        const auto result = number<std::uint64_t>();
        if (result > maximum) throw std::runtime_error("Animation graph manifest: count exceeds supported limit");
        return std::size_t(result);
    }
    float real() {
        const auto value = number<float>();
        if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
            throw std::runtime_error("Animation graph manifest: unsupported floating value");
        return value;
    }
    bool flag() {
        const auto value = token();
        if (value != "0" && value != "1") throw std::runtime_error("Animation graph manifest: flag must be 0 or 1");
        return value == "1";
    }
    std::string quoted(bool empty = false) {
        input_ >> std::ws;
        if (input_.peek() != '"') throw std::runtime_error("Animation graph manifest: quoted string required");
        std::string result;
        if (!(input_ >> std::quoted(result)) || result.size() > 255 || (!empty && result.empty()))
            throw std::runtime_error("Animation graph manifest: invalid string extent");
        for (unsigned char value : result)
            if (value < 32 || value == 127) throw std::runtime_error("Animation graph manifest: control character in string");
        return result;
    }
    std::string hash() {
        auto result = quoted();
        if (result.size() != 64) throw std::runtime_error("Animation graph manifest: SHA-256 length is invalid");
        for (char value : result)
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
                throw std::runtime_error("Animation graph manifest: SHA-256 must use lowercase hexadecimal");
        return result;
    }
    void end() {
        input_ >> std::ws;
        if (input_.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Animation graph manifest: trailing content");
    }
private:
    std::istringstream input_;
};
void budget(std::size_t& used, std::size_t amount) {
    if (amount > maximum_entries - used)
        throw std::runtime_error("Animation graph manifest: aggregate entry budget exceeded");
    used += amount;
}
}

const GraphNamedRoot& AnimationGraphManifest::root(std::string_view name) const {
    for (const auto& entry : roots) if (entry.name == name) return entry;
    throw std::runtime_error("Animation graph manifest: named root is absent");
}

AnimationGraphManifest decode_animation_graph_manifest(std::istream& input) {
    std::string text;
    std::array<char, 8192> buffer;
    while (input) {
        input.read(buffer.data(), std::streamsize(buffer.size()));
        const auto read = std::size_t(input.gcount());
        if (read > maximum_bytes - text.size())
            throw std::runtime_error("Animation graph manifest: file exceeds supported size");
        text.append(buffer.data(), read);
    }
    if (input.bad()) throw std::runtime_error("Animation graph manifest: input read failed");
    Reader reader(std::move(text));
    reader.tag("OMEGA_GRAPH_MANIFEST");
    if (reader.number<unsigned>() != 1) throw std::runtime_error("Animation graph manifest: unsupported version");
    reader.tag("COMPLETE");
    if (!reader.flag()) throw std::runtime_error("Animation graph manifest: extraction is incomplete");
    reader.tag("SOURCE"); const auto source = reader.quoted();
    reader.tag("DEFAULT_CROSSFADE"); const auto crossfade = reader.real();
    if (crossfade < 0) throw std::runtime_error("Animation graph manifest: negative default crossfade");
    reader.tag("PROVENANCE");
    const auto iso = reader.hash(), executable = reader.hash(), skas = reader.hash();
    reader.tag("ANCHORS"); const auto anchor_count = reader.count(256);
    if (!anchor_count) throw std::runtime_error("Animation graph manifest: no anchor keys");
    std::vector<std::array<std::uint8_t, 4>> anchors;
    for (std::size_t index = 0; index != anchor_count; ++index) {
        reader.tag("ANCHOR"); std::array<std::uint8_t, 4> key;
        for (auto& value : key) value = std::uint8_t(reader.count(255));
        if (std::find(anchors.begin(), anchors.end(), key) != anchors.end())
            throw std::runtime_error("Animation graph manifest: duplicate anchor key");
        anchors.push_back(key);
    }
    reader.tag("BUILDERS");
    const auto expected = reader.count(256), completed = reader.count(256);
    if (!expected || completed != expected)
        throw std::runtime_error("Animation graph manifest: builders are incomplete");
    reader.tag("CHANNELS"); const auto channels = reader.count(4096);
    reader.tag("SELECTORS"); const auto selectors = reader.count(4096);
    reader.tag("NODES"); const auto count = reader.count(4096);
    std::vector<AnimationGraphNode> nodes;
    nodes.reserve(count);
    std::size_t entries = 0;
    for (std::size_t index = 0; index != count; ++index) {
        reader.tag("NODE");
        if (reader.count(4095) != index) throw std::runtime_error("Animation graph manifest: nonsequential node index");
        const auto type = reader.count(3);
        AnimationGraphNode node;
        node.kind = static_cast<AnimationGraphKind>(type);
        node.name = reader.quoted(true);
        for (auto& part : node.rotation) part = reader.real();
        if (type <= 1) {
            reader.tag("LEAF");
            auto& leaf = node.leaf;
            leaf.clip_key = reader.quoted();
            leaf.source_word20 = reader.number<std::uint32_t>();
            leaf.reverse = reader.flag(); leaf.mirror = reader.flag();
            leaf.range_start = reader.real(); leaf.range_end = reader.real();
            leaf.phase_offset = reader.real(); leaf.rate_scale = reader.real(); leaf.duration = reader.real();
            leaf.group = reader.number<std::int32_t>();
            leaf.motion_bucket = std::uint8_t(reader.count(255));
            leaf.looping = reader.flag(); leaf.align_sign = reader.flag();
            if (type == 1) {
                reader.tag("CONTROL"); node.channel = reader.count(4095);
                node.angle_start = reader.real(); node.angle_end = reader.real();
                for (auto& part : node.axis) part = reader.real();
            }
        } else if (type == 2) {
            reader.tag("SLIDER"); node.channel = reader.count(4095);
            node.smooth = reader.flag(); node.source_flag35 = std::uint8_t(reader.count(255));
            const auto children = reader.count(256); budget(entries, children);
            for (std::size_t child = 0; child != children; ++child) {
                reader.tag("CHILD");
                node.slider_children.push_back({reader.count(4095), reader.real(), reader.real()});
            }
        } else {
            reader.tag("SELECTOR"); node.channel = reader.count(4095);
            const auto children = reader.count(256); budget(entries, children);
            for (std::size_t child = 0; child != children; ++child) {
                reader.tag("CHILD"); GraphSelectorChild entry;
                entry.node_index = reader.count(4095);
                const auto rules = reader.count(256); budget(entries, rules);
                for (std::size_t rule_index = 0; rule_index != rules; ++rule_index) {
                    reader.tag("RULE"); GraphSelectorRule rule;
                    rule.duration_start = reader.real(); rule.duration_end = reader.real();
                    rule.channel = reader.number<std::int32_t>();
                    rule.reversible = reader.flag(); rule.smooth = reader.flag();
                    rule.redirect = reader.number<std::int32_t>();
                    const auto waypoints = reader.count(256); budget(entries, waypoints);
                    for (std::size_t waypoint = 0; waypoint != waypoints; ++waypoint) {
                        reader.tag("WAYPOINT");
                        rule.intermediates.push_back({reader.count(4095), reader.real(), reader.real()});
                    }
                    if (rule.channel >= 0 && std::size_t(rule.channel) >= channels)
                        throw std::runtime_error("Animation graph manifest: rule channel exceeds declared context");
                    entry.rules.push_back(std::move(rule));
                }
                node.selector_children.push_back(std::move(entry));
            }
        }
        if (((type == 1 || type == 2) && node.channel >= channels) || (type == 3 && node.channel >= selectors))
            throw std::runtime_error("Animation graph manifest: node channel exceeds declared context");
        reader.tag("ENDNODE"); nodes.push_back(std::move(node));
    }
    reader.tag("ROOTS"); const auto roots = reader.count(10000);
    if (!roots) throw std::runtime_error("Animation graph manifest: no named roots");
    std::vector<GraphNamedRoot> named;
    std::unordered_set<std::string> names;
    for (std::size_t index = 0; index != roots; ++index) {
        reader.tag("ROOT"); GraphNamedRoot root;
        root.name = reader.quoted(); root.node_index = reader.count(4095);
        root.group_pair = {reader.number<std::uint32_t>(), reader.number<std::uint32_t>()};
        if (root.node_index >= count || !names.insert(root.name).second)
            throw std::runtime_error("Animation graph manifest: invalid or duplicate named root");
        named.push_back(std::move(root));
    }
    reader.tag("END_MANIFEST"); reader.end();
    AnimationGraphManifest result{AnimationGraph(std::move(nodes))};
    result.source_skas = source; result.iso_sha256 = iso;
    result.default_crossfade_duration = crossfade;
    result.executable_sha256 = executable; result.skas_sha256 = skas;
    result.anchors = std::move(anchors);
    result.expected_builders = expected; result.completed_builders = completed;
    result.channel_count = channels; result.selector_count = selectors; result.roots = std::move(named);
    return result;
}

AnimationGraphManifest load_animation_graph_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Animation graph manifest: cannot open input");
    return decode_animation_graph_manifest(input);
}
}
