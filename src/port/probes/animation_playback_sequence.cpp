#include "animation_playback_sequence.hpp"
#include "animation_graph_manifest.hpp"
#include "animation_player.hpp"
#include "animation_controller.hpp"
#include "actor_ladder_bindings.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace omega {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("Animation playback sequence: ") + message); }
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a == b) return true;
    std::error_code error;
    return std::filesystem::equivalent(a,b,error) && !error;
}
std::filesystem::path local_path(const std::filesystem::path& input) {
    const auto result = std::filesystem::absolute(input).lexically_normal();
    const auto root = std::filesystem::absolute("local").lexically_normal();
    const auto relative = result.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute()) fail("path must be a file inside workspace/local");
    for (const auto& component : relative)
        if (component == ".." || component.native().find(static_cast<std::filesystem::path::value_type>(':')) != std::filesystem::path::string_type::npos)
            fail("path escapes workspace/local or contains an alternate stream");
    std::filesystem::path prefix;
    for (const auto& component : result) {
        prefix /= component;
        if (!prefix.has_relative_path()) continue;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            fail("cannot inspect path component");
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("redirected paths are unsupported");
#else
        std::error_code error;
        const auto status = std::filesystem::symlink_status(prefix,error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) fail("cannot inspect path component");
        if (std::filesystem::is_symlink(status)) fail("redirected paths are unsupported");
#endif
    }
    return result;
}
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path, std::uint64_t maximum) {
    const auto size = std::filesystem::file_size(path);
    if (size > maximum) fail("input exceeds byte limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path,std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) fail("cannot read input");
    return bytes;
}
class Reader {
    std::istringstream input_;
public:
    explicit Reader(const std::vector<std::uint8_t>& text)
        : input_(std::string(text.begin(),text.end())) { input_.imbue(std::locale::classic()); }
    std::string token() {
        std::string value;
        if (!(input_ >> value) || value.size()>128) fail("missing or oversized token");
        return value;
    }
    void tag(std::string_view value) { if (token()!=value) fail("unexpected field or version"); }
    template<class T> T number() {
        const auto text=token(); T value{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) fail("invalid number");
        return value;
    }
    std::size_t count(std::size_t maximum) {
        const auto n=number<std::uint64_t>();
        if (n>maximum) fail("count exceeds limit");
        return static_cast<std::size_t>(n);
    }
    float real() {
        const auto value=number<float>();
        if (!std::isfinite(value) || std::fpclassify(value)==FP_SUBNORMAL) fail("unsupported floating value");
        return value;
    }
    bool flag() { const auto v=token(); if(v!="0" && v!="1") fail("flag must be 0 or 1"); return v=="1"; }
    std::string quoted(std::size_t maximum=255) {
        input_ >> std::ws;
        if(input_.peek()!='"') fail("quoted string required");
        std::string value;
        if(!(input_ >> std::quoted(value)) || value.empty() || value.size()>maximum) fail("invalid string extent");
        for(unsigned char c:value) if(c<32 || c==127) fail("control character in string");
        return value;
    }
    void end() { input_ >> std::ws; if(input_.peek()!=std::char_traits<char>::eof()) fail("trailing data"); }
};
void text(std::ostream& output,std::string_view value) {
    constexpr char hex[]="0123456789abcdef";
    output << '"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') output << '\\' << static_cast<char>(c);
        else if(c<32 || c>=127) output << "\\u00" << hex[c>>4] << hex[c&15];
        else output << static_cast<char>(c);
    }
    output << '"';
}
template<class Range> void array(std::ostream& output,const Range& value) {
    output << '['; bool first=true;
    for(const auto x:value) { if(!first) output << ','; first=false; output << +x; }
    output << ']';
}
void snapshot(std::ostream& output,const AnimationPlayerState& state) {
    output << "\"groups\":[";
    for(std::size_t i=0;i<state.groups.size();++i) {
        const auto& g=state.groups[i]; if(i) output << ',';
        output << "{\"host\":" << g.host << ",\"number\":" << g.number
            << ",\"active\":" << (g.active?"true":"false") << ",\"paused\":" << (g.paused?"true":"false")
            << ",\"previous\":" << g.phase.previous << ",\"current\":" << g.phase.current << ",\"advance\":" << g.phase.advance
            << ",\"independent_bounds\":" << (g.phase.independent_bounds?"true":"false")
            << ",\"looping\":" << (g.phase.looping?"true":"false") << ",\"finished\":" << (g.phase.finished?"true":"false")
            << ",\"boundary\":" << (g.phase.boundary?"true":"false") << ",\"total_weight\":" << g.totals.total_weight
            << ",\"weighted_duration\":" << g.totals.weighted_duration << ",\"members\":[";
        for(std::size_t j=0;j<g.members.size();++j) {
            const auto& m=g.members[j]; if(j) output << ',';
            output << "{\"node\":" << m.node << ",\"weight\":" << m.weight << ",\"raw_rotation\":" << (m.raw_rotation?"true":"false") << ",\"rotation\":";
            array(output,m.rotation); output << '}';
        }
        output << "]}";
    }
    output << "],\"orientation\":"; array(output,state.orientation);
    output << ",\"position\":"; array(output,state.position);
    output << ",\"base_orientation\":"; array(output,state.base_orientation);
    output << ",\"base_position\":"; array(output,state.base_position);
    output << ",\"root_local\":"; array(output,state.root_local);
    output << ",\"world\":[";
    for(std::size_t i=0;i<state.world.size();++i) { if(i) output << ','; array(output,state.world[i]); }
    output << "],\"pose\":"; write_nonroot_pose(output,state.pose);
    output << ",\"channels\":"; array(output,state.graph.channels);
    output << ",\"selectors\":[";
    for(std::size_t i=0;i<state.graph.selectors.size();++i) {
        if(i) output << ',';
        output << '[' << state.graph.selectors[i].requested << ',' << state.graph.selectors[i].previous << ']';
    }
    output << "],\"pending_selector_transitions\":" << state.graph.transitions.size();
}
}
namespace {
AnimationClipBank read_clips(Reader& reader,const SkeletonDefinition& skeleton,const std::filesystem::path& target) {
    reader.tag("CLIPS");
    const auto clip_count=reader.count(256);
    AnimationClipBank clips;
    std::uint64_t total_bytes{},decoded_slots{};
    for(std::size_t i=0;i<clip_count;++i) {
        reader.tag("CLIP"); const auto key=reader.quoted(); const auto path=local_path(std::filesystem::u8path(reader.quoted(4096)));
        if(clips.contains(key)) fail("duplicate clip key");
        if(same_file(target,path)) fail("output aliases a clip input");
        const auto size=std::filesystem::file_size(path);
        if(size>64*1024*1024 || total_bytes+size>256*1024*1024) fail("clip byte budget exceeded");
        total_bytes+=size;
        auto clip=decode_animation_clip(read_bytes(path,64*1024*1024),static_cast<std::uint32_t>(skeleton.bones.size()));
        decoded_slots+=clip.frames.size()*static_cast<std::uint64_t>(clip.bone_count);
        if(decoded_slots>2000000) fail("decoded clip budget exceeded");
        clips.emplace(key,std::move(clip));
    }
    return clips;
}
void initial_context(Reader& reader,AnimationPlayerState& state) {
    reader.tag("INITIAL_CHANNELS");
    if(reader.count(4096)!=state.graph.channels.size()) fail("initial channel count mismatch");
    for(auto& channel:state.graph.channels) channel=reader.real();
    reader.tag("INITIAL_SELECTORS");
    if(reader.count(4096)!=state.graph.selectors.size()) fail("initial selector count mismatch");
    for(auto& selector:state.graph.selectors) { selector.requested=reader.count(255); selector.previous=reader.count(255); }
}
}
void write_animation_playback_sequence(std::ostream& output,const SkeletonDefinition& skeleton,
    const std::filesystem::path& manifest_path,const std::filesystem::path& sequence_path,
    const std::filesystem::path& destination) {
    const auto target=local_path(destination), manifest_file=local_path(manifest_path), sequence_file=local_path(sequence_path);
    if(same_file(target,manifest_file) || same_file(target,sequence_file)) fail("output aliases an input");
    auto manifest=load_animation_graph_manifest(manifest_file);
    Reader reader(read_bytes(sequence_file,16*1024*1024));
    reader.tag("OMEGA_ANIMATION_PLAYBACK"); reader.tag("1");
    const auto clips=read_clips(reader,skeleton,target);
    auto state=make_animation_player(skeleton,manifest.anchors,manifest.channel_count,manifest.selector_count);
    initial_context(reader,state);
    reader.tag("FRAMES"); const auto frame_count=reader.count(4096);
    if(!frame_count) fail("at least one frame is required");
    output.imbue(std::locale::classic()); output << std::setprecision(9);
    output << "{\"scope\":\"Recovered graph and clip playback with explicit diagnostic controls; outer motion controller bypassed, named-root group metadata retained but not applied; no actor input or collision hosting\",\"source_skas\":";
    text(output,manifest.source_skas);
    output << ",\"bone_count\":" << skeleton.bones.size() << ",\"clip_count\":" << clips.size() << ",\"root_count\":" << manifest.roots.size()
        << ",\"node_count\":" << manifest.graph.nodes().size() << ",\"frames\":[";
    for(std::size_t frame=0;frame<frame_count;++frame) {
        reader.tag("FRAME"); const auto label=reader.quoted(); const auto dt=reader.real();
        reader.tag("CHANNELS"); if(reader.count(4096)!=state.graph.channels.size()) fail("frame channel count mismatch");
        for(auto& channel:state.graph.channels) channel=reader.real();
        reader.tag("SELECTORS"); if(reader.count(4096)!=state.graph.selectors.size()) fail("frame selector count mismatch");
        for(auto& selector:state.graph.selectors) selector.requested=reader.count(255);
        reader.tag("ROOTS"); const auto count=reader.count(256);
        std::vector<AnimationRootRequest> roots; roots.reserve(count);
        std::vector<std::string> names; names.reserve(count);
        for(std::size_t i=0;i<count;++i) {
            reader.tag("ROOT"); AnimationRootRequest root;
            root.host=reader.number<std::uint64_t>(); names.push_back(reader.quoted());
            const auto& named=manifest.root(names.back());
            root.node=named.node_index;
            root.weight=reader.real(); root.initial_phase=reader.real(); root.paused=reader.flag();
            for(auto& x:root.rotation) x=reader.real(); roots.push_back(root);
        }
        reader.tag("END_FRAME");
        update_animation_player(state,manifest.graph,skeleton,clips,roots,dt);
        if(frame) output << ',';
        output << "{\"label\":"; text(output,label); output << ",\"dt\":" << dt << ",\"roots\":[";
        for(std::size_t i=0;i<names.size();++i) { if(i) output << ','; text(output,names[i]); }
        output << "],\"root_group_pairs\":[";
        for(std::size_t i=0;i<names.size();++i) { if(i) output << ','; array(output,manifest.root(names[i]).group_pair); }
        output << "],"; snapshot(output,state); output << '}';
    }
    reader.tag("END_SEQUENCE"); reader.end();
    output << "]}";
}
namespace {
void request_json(std::ostream& out,const MotionRequest& r) {
    out<<"{\"node\":"<<r.node<<",\"width\":"<<r.width<<",\"bias\":"<<r.bias<<",\"rate\":"<<r.rate
        <<",\"secondary_rate\":"<<r.secondary_rate<<",\"limit\":"<<r.limit<<",\"initial_phase\":"<<r.initial_phase<<",\"selector\":{\"node\":";
    if(r.selector.node) out<<*r.selector.node;else out<<"null";
    out<<",\"group\":"<<r.selector.group<<"},\"messages\":[";
    for(std::size_t i=0;i<r.messages.size();++i) {
        const auto& m=r.messages[i];if(i)out<<',';
        out<<"{\"id\":"<<m.id<<",\"receiver\":"<<m.receiver<<",\"flags\":"<<m.flags<<",\"sent\":"<<(m.sent?"true":"false")<<'}';
    }
    out<<"],\"notifications_active\":"<<(r.notifications_active?"true":"false")<<",\"notifications\":[";
    for(std::size_t i=0;i<r.notifications.size();++i) {
        const auto& t=r.notifications[i];if(i)out<<',';
        out<<"{\"id\":"<<t.id<<",\"receiver\":"<<t.receiver<<'}';
    }
    out<<"],\"phase_hooks\":[";
    for(std::size_t i=0;i<r.phase_hooks.size();++i) {
        const auto& h=r.phase_hooks[i];if(i)out<<',';
        out<<"{\"start\":"<<h.start<<",\"end\":"<<h.end<<",\"token\":{\"id\":"<<h.token.id<<",\"receiver\":"<<h.token.receiver<<"}}";
    }
    out<<"]}";
}
void records_json(std::ostream& out,const std::vector<MotionRecord>& records) {
    out<<'[';
    for(std::size_t i=0;i<records.size();++i) {
        const auto& r=records[i];const auto& b=r.blend;if(i)out<<',';
        out<<"{\"host\":"<<r.host<<",\"request\":";request_json(out,r.request);
        out<<",\"blend\":{\"progress\":"<<b.progress<<",\"rate\":"<<b.rate<<",\"width\":"<<b.width<<",\"bias\":"<<b.bias
            <<",\"elapsed\":"<<b.elapsed<<",\"limit\":"<<b.limit<<"},\"starting\":"<<(r.starting?"true":"false")
            <<",\"leaving\":"<<(r.leaving?"true":"false")<<'}';
    }
    out<<']';
}
void controller_json(std::ostream& out,const AnimationController& c) {
    out<<"{\"next_host\":"<<c.next_host<<",\"terminal_threshold_enabled\":"<<(c.terminal_threshold_enabled?"true":"false")
        <<",\"terminal_width\":"<<c.terminal_width<<",\"terminal_bias\":"<<c.terminal_bias<<",\"active\":";
    records_json(out,c.active);out<<",\"queued\":";records_json(out,c.queued);out<<",\"effects\":[";
    for(std::size_t i=0;i<c.effects.size();++i) {
        const auto& e=c.effects[i];if(i)out<<',';
        out<<"{\"kind\":";text(out,e.kind==MotionEffect::Kind::embedded?"embedded":"token");
        out<<",\"host\":"<<e.host<<",\"receiver\":"<<e.receiver<<",\"id\":"<<e.id<<",\"slot\":"<<e.slot
            <<",\"action\":"<<e.action<<",\"flags\":"<<e.flags<<",\"step\":"<<e.step<<'}';
    }
    out<<"]}";
}
}
void write_animation_controller_sequence(std::ostream& output,const SkeletonDefinition& skeleton,
    const std::filesystem::path& manifest_path,const std::filesystem::path& sequence_path,
    const std::filesystem::path& destination) {
    const auto target=local_path(destination), manifest_file=local_path(manifest_path), sequence_file=local_path(sequence_path);
    if(same_file(target,manifest_file) || same_file(target,sequence_file)) fail("output aliases an input");
    auto manifest=load_animation_graph_manifest(manifest_file);
    Reader reader(read_bytes(sequence_file,16*1024*1024));reader.tag("OMEGA_ANIMATION_CONTROLLER");reader.tag("1");
    const auto clips=read_clips(reader,skeleton,target);
    auto player=make_animation_player(skeleton,manifest.anchors,manifest.channel_count,manifest.selector_count);
    initial_context(reader,player);AnimationController controller;
    std::optional<ActorLadderBindings> ladder;
    reader.tag("TERMINAL");controller.terminal_threshold_enabled=reader.flag();
    controller.terminal_width=reader.real();controller.terminal_bias=reader.real();
    reader.tag("OPERATIONS");const auto operations=reader.count(4096);if(!operations)fail("at least one controller operation required");
    output.imbue(std::locale::classic());output<<std::setprecision(9);
    output<<"{\"scope\":\"Connected motion controller with explicit diagnostic commands; effects are undelivered requests; no actor input or collision hosting\",\"source_skas\":";
    text(output,manifest.source_skas);output<<",\"bone_count\":"<<skeleton.bones.size()<<",\"operations\":[";
    for(std::size_t i=0;i<operations;++i) {
        const auto operation=reader.token();
        if(operation=="LADDER_CONFIG") {
            const auto path=local_path(reader.quoted(4096));
            if(same_file(target,path))fail("output aliases ladder configuration");
            std::ifstream config(path,std::ios::binary);
            if(!config)fail("cannot read ladder configuration");
            ladder=read_actor_ladder_bindings(config,manifest.graph);
        } else if(operation=="INSTALL" || operation=="INSTALL_FULL" || operation=="INSTALL_NODE" || operation=="INSTALL_LADDER" || operation=="INSTALL_LADDER_EXIT") {
            const auto mode=reader.count(3);
            MotionRequest request;
            if(operation=="INSTALL_LADDER_EXIT") {
                if(!ladder)fail("ladder configuration must be loaded before installation");
                const auto node=ladder->resolve_exit(static_cast<ActorLadderExitMotion>(reader.count(7)));
                request=make_motion_request(node,manifest.default_crossfade_duration);
                const auto& selected=manifest.graph.nodes()[node];
                if(selected.kind==AnimationGraphKind::Leaf||selected.kind==AnimationGraphKind::ControlledLeaf) {
                    request.selector.group=selected.leaf.group;
                    if(selected.leaf.group==-1)request.selector.node=node;
                }
            } else if(operation=="INSTALL_LADDER") {
                if(!ladder)fail("ladder configuration must be loaded before installation");
                request=ladder->resolve(static_cast<ActorLadderMotion>(reader.count(6)));
            } else if(operation=="INSTALL_NODE") {
                const auto node=reader.count(manifest.graph.nodes().size()-1);
                request=make_motion_request(node,manifest.default_crossfade_duration);
            } else {
                const auto& root=manifest.root(reader.quoted());
                request=make_motion_request(root.node_index,manifest.default_crossfade_duration);
                if(operation=="INSTALL") {
                    if(root.group_pair[0]!=0) fail("unresolved named-root node selector");
                    request.selector.group=std::bit_cast<std::int32_t>(root.group_pair[1]);
                }
            }
            if(operation!="INSTALL") {
                request.width=reader.real();request.bias=reader.real();request.rate=reader.real();request.secondary_rate=reader.real();
                request.limit=reader.real();request.initial_phase=reader.real();
                if(operation!="INSTALL_LADDER"&&operation!="INSTALL_LADDER_EXIT") {
                    const auto node=reader.number<std::int64_t>();
                    if(node < -1 || (node>=0 && static_cast<std::uint64_t>(node)>=manifest.graph.nodes().size())) fail("invalid selector node");
                    if(node>=0)request.selector.node=static_cast<std::size_t>(node);
                    request.selector.group=reader.number<std::int32_t>();
                }
                request.notifications_active=reader.flag();
                for(auto& message:request.messages) {
                    reader.tag("MESSAGE");message.id=reader.number<std::int32_t>();message.receiver=reader.number<std::uint64_t>();
                    message.flags=reader.number<std::uint32_t>();message.sent=reader.flag();
                }
                for(auto& token:request.notifications) {
                    reader.tag("TOKEN");token.id=reader.number<std::int32_t>();token.receiver=reader.number<std::uint64_t>();
                }
                reader.tag("HOOKS");const auto hooks=reader.count(4096);
                for(std::size_t h=0;h<hooks;++h) {
                    reader.tag("HOOK");MotionPhaseHook hook;hook.start=reader.real();hook.end=reader.real();
                    hook.token.id=reader.number<std::int32_t>();hook.token.receiver=reader.number<std::uint64_t>();request.phase_hooks.push_back(hook);
                }
            }
            install_motion(controller,request,static_cast<MotionInstallMode>(mode));
        } else if(operation=="CLEAR") clear_motions(controller);
        else if(operation=="PLAYBACK_RATE") {
            const auto rate=reader.real();if(rate<0)fail("negative model playback rate");player.playback_rate=rate;
        }
        else if(operation=="STEP") {
            const auto dt=reader.real();reader.tag("CHANNELS");
            if(reader.count(4096)!=player.graph.channels.size())fail("controller channel count mismatch");
            for(auto& channel:player.graph.channels)channel=reader.real();
            reader.tag("SELECTORS");if(reader.count(4096)!=player.graph.selectors.size())fail("controller selector count mismatch");
            for(auto& selector:player.graph.selectors)selector.requested=reader.count(255);
            update_animation_controller(controller,player,manifest.graph,skeleton,clips,dt);
        } else fail("unknown controller operation");
        if(i)output<<',';output<<"{\"operation\":";text(output,operation);output<<",\"controller\":";
        controller_json(output,controller);output<<",\"player\":{";snapshot(output,player);output<<"}}";
    }
    reader.tag("END_SEQUENCE");reader.end();output<<"]}";
}
}
