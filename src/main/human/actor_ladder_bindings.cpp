#include "actor_ladder_bindings.hpp"
#include "number_parse.hpp"  // port-only (132ps5c): from_chars of floats on the PS5
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
[[noreturn]] void fail(const char* reason) {
    throw std::runtime_error(std::string("Actor ladder bindings: ") + reason);
}
class Reader {
public:
    explicit Reader(std::istream& input) {
        std::string source; char byte;
        while (input.get(byte)) {
            if (source.size() == 4096) fail("input exceeds bounded extent");
            source.push_back(byte);
        }
        if (!input.eof()) fail("input read failed");
        stream_.str(source);
    }
    void expect(const char* expected) {
        if (token() != expected) fail("unexpected field or version");
    }
    template<class T> T number() {
        const auto text = token(); T value{};
        const auto result = omega::from_chars_number(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) fail("invalid number");
        return value;
    }
    float scalar() {
        const float value = number<float>();
        if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL) fail("invalid scalar");
        return value;
    }
    void end() { stream_ >> std::ws; if (!stream_.eof()) fail("trailing input"); }
private:
    std::istringstream stream_;
    std::string token() { std::string result; if (!(stream_ >> result)) fail("incomplete input"); return result; }
};
}
ActorLadderBindings read_actor_ladder_bindings(std::istream& input, const AnimationGraph& graph) {
    Reader reader(input); reader.expect("OMEGA_LADDER_CONFIG");
    const auto version=reader.number<unsigned>();
    if(version<1||version>3)fail("unsupported version");
    ActorLadderBindings result;
    result.geometry_.rung_spacing = reader.scalar();
    result.geometry_.lower_offset = reader.scalar();
    result.dismount_rungs_ = reader.scalar();
    if (result.geometry_.rung_spacing <= 0 || result.dismount_rungs_ < 0) fail("invalid geometry tuning");
    reader.expect("BINDINGS"); reader.expect("7");
    std::array<bool, 7> seen{};
    for (std::size_t row = 0; row < seen.size(); ++row) {
        const auto role = reader.number<std::size_t>();
        const auto node = reader.number<std::size_t>();
        const auto selector = reader.number<std::int64_t>();
        const auto group = reader.number<std::int32_t>();
        if (role >= seen.size() || seen[role]) fail("invalid or duplicate role");
        if (node >= graph.nodes().size()) fail("motion node outside graph");
        if (selector < -1 || (selector >= 0 && static_cast<std::uint64_t>(selector) >= graph.nodes().size()))
            fail("selector node outside graph");
        if (group < -1) fail("invalid selector group");
        auto request = make_motion_request(node, 0);
        request.selector.group = group;
        if (selector >= 0) {
            const auto id = static_cast<std::size_t>(selector);
            const auto kind = graph.nodes()[id].kind;
            if (kind != AnimationGraphKind::Leaf && kind != AnimationGraphKind::ControlledLeaf)
                fail("selector identity is not a leaf");
            if (group != -1 || graph.nodes()[id].leaf.group != -1)
                fail("node selector conflicts with a numbered group");
            request.selector.node = id;
        }
        result.motions_[role] = std::move(request); seen[role] = true;
    }
    if(version>=2) {
        reader.expect("EXIT_BINDINGS");reader.expect("8");
        std::array<std::size_t,8> exits{};std::array<bool,8> exit_seen{};
        for(std::size_t row=0;row<exits.size();++row) {
            const auto role=reader.number<std::size_t>();const auto node=reader.number<std::size_t>();
            if(role>=exits.size()||exit_seen[role]||node>=graph.nodes().size())fail("invalid exit binding");
            exits[role]=node;exit_seen[role]=true;
        }
        result.exits_=exits;
        if(version==3) {
            reader.expect("EXIT_CROSSFADE");const auto value=reader.scalar();
            if(value<0)fail("negative exit transition");result.exit_crossfade_=value;
        }
    }
    reader.expect("END_LADDER_CONFIG"); reader.end(); return result;
}
MotionRequest ActorLadderBindings::resolve(ActorLadderMotion motion) const {
    const auto role = static_cast<std::size_t>(motion);
    if (role >= motions_.size()) fail("unknown motion role");
    return motions_[role];
}
std::size_t ActorLadderBindings::resolve_exit(ActorLadderExitMotion motion) const {
    const auto role=static_cast<std::size_t>(motion);
    if(!exits_||role>=exits_->size())fail("exit binding unavailable");
    return (*exits_)[role];
}
float ActorLadderBindings::exit_crossfade() const {
    if(!exit_crossfade_)fail("exit crossfade unavailable");return *exit_crossfade_;
}
}
