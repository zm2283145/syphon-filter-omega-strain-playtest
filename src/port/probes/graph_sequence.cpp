#include "graph_sequence.hpp"
#include "animation_graph_manifest.hpp"
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <span>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace omega {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("Graph cases: ")+message); }
std::filesystem::path local_path(const std::filesystem::path& value) {
    const auto path=std::filesystem::absolute(value).lexically_normal();
    const auto relative=path.lexically_relative(std::filesystem::absolute("local").lexically_normal());
    if(relative.empty() || relative=="." || relative.is_absolute()) fail("input must be inside workspace/local");
    for(const auto& part:relative)
        if(part==".." || part.native().find(static_cast<std::filesystem::path::value_type>(':'))!=std::filesystem::path::string_type::npos)
            fail("input escapes workspace/local or contains alternate streams");
    std::filesystem::path prefix;
    for(const auto& part:path) {
        prefix/=part;
        if(!prefix.has_relative_path()) continue;
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(prefix.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) fail("cannot inspect input path");
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT) fail("redirected inputs are unsupported");
#else
        std::error_code error;
        const auto status=std::filesystem::symlink_status(prefix,error);
        if(error || std::filesystem::is_symlink(status)) fail("cannot inspect input or redirected input");
#endif
    }
    return path;
}
class Reader {
    std::istringstream input_;
public:
    explicit Reader(const std::filesystem::path& path) {
        const auto size=std::filesystem::file_size(path);
        if(size>16*1024*1024) fail("case input exceeds byte bound");
        std::string text(static_cast<std::size_t>(size),'\0');
        std::ifstream input(path,std::ios::binary);
        if(!input || !input.read(text.data(),static_cast<std::streamsize>(size))) fail("cannot read cases");
        input_.str(std::move(text));input_.imbue(std::locale::classic());
    }
    std::string token() {
        std::string value;
        if(!(input_>>value) || value.size()>128) fail("missing or oversized token");
        for(unsigned char c:value) if(c<32 || c>126) fail("case tokens must be printable ASCII");
        return value;
    }
    void tag(std::string_view value) { if(token()!=value) fail("unexpected header"); }
    template<class T> T number() {
        const auto word=token();T value{};
        const auto parsed=std::from_chars(word.data(),word.data()+word.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=word.data()+word.size()) fail("invalid number");
        return value;
    }
    std::size_t count(std::size_t maximum) {
        const auto n=number<std::uint64_t>();if(n>maximum) fail("count exceeds bound");
        return static_cast<std::size_t>(n);
    }
    float real() {
        const auto value=number<float>();
        if(!std::isfinite(value) || std::fpclassify(value)==FP_SUBNORMAL) fail("unsupported floating value");
        return value;
    }
    std::string quoted() {
        input_>>std::ws;if(input_.peek()!='"') fail("quoted manifest path required");
        std::string value;
        if(!(input_>>std::quoted(value)) || value.empty() || value.size()>4096) fail("invalid manifest path");
        for(unsigned char c:value) if(c<32 || c==127) fail("control character in path");
        return value;
    }
    void end() { input_>>std::ws;if(input_.peek()!=std::char_traits<char>::eof()) fail("trailing data"); }
};
template<class Range> void array(std::ostream& out,const Range& values) {
    out<<'[';bool first=true;
    for(const auto value:values) { if(!first) out<<',';first=false;out<<value; }
    out<<']';
}
void snapshot(std::ostream& out,const AnimationGraphContext& context,std::span<const GraphLeafContribution> leaves) {
    out<<"{\"channels\":";array(out,context.channels);out<<",\"selectors\":[";
    for(std::size_t i=0;i<context.selectors.size();++i) {
        if(i) out<<',';
        out<<"{\"requested\":"<<context.selectors[i].requested<<",\"previous\":"<<context.selectors[i].previous<<'}';
    }
    out<<"],\"transitions\":[";
    for(std::size_t i=0;i<context.transitions.size();++i) {
        const auto& s=context.transitions[i];if(i) out<<',';
        out<<"{\"node_index\":"<<s.node_index<<",\"source_index\":"<<s.source_index<<",\"target_index\":"<<s.target_index
            <<",\"elapsed\":"<<s.elapsed<<",\"blend\":"<<s.blend
            <<",\"reversing\":"<<(s.reversing?"true":"false")<<",\"interpolating\":"<<(s.interpolating?"true":"false")
            <<",\"ready\":"<<(s.ready?"true":"false")<<",\"source_node\":"<<s.source_node<<",\"target_node\":"<<s.target_node<<'}';
    }
    out<<"],\"contributions\":[";
    for(std::size_t i=0;i<leaves.size();++i) {
        const auto& leaf=leaves[i];if(i) out<<',';
        out<<"{\"node_index\":"<<leaf.node_index<<",\"weight\":"<<leaf.weight<<",\"rotation\":";
        array(out,leaf.rotation);out<<'}';
    }
    out<<"]}";
}
}
void write_graph_cases(std::ostream& output,const std::filesystem::path& path) {
    Reader reader(local_path(path));
    reader.tag("OMEGA_GRAPH_CASES");reader.tag("1");const auto count=reader.count(256);
    if(!count) fail("at least one case required");
    output.imbue(std::locale::classic());output<<std::setprecision(9)<<"{\"cases\":[";
    std::size_t total_frames=0;
    for(std::size_t c=0;c<count;++c) {
        const auto name=reader.token();const auto manifest_path=local_path(std::filesystem::u8path(reader.quoted()));
        auto manifest=load_animation_graph_manifest(manifest_path);
        const auto root=reader.count(4095), channels=reader.count(4096), selectors=reader.count(4096),frames=reader.count(4096);
        if(channels!=manifest.channel_count || selectors!=manifest.selector_count || !frames || total_frames+frames>10000)
            fail("context count or frame budget mismatch");
        total_frames+=frames;
        AnimationGraphContext context;context.channels.resize(channels);context.selectors.resize(selectors);
        for(auto& channel:context.channels) channel=reader.real();
        for(auto& selector:context.selectors) { selector.requested=reader.count(255);selector.previous=reader.count(255); }
        if(c) output<<',';
        output<<"{\"name\":"<<std::quoted(name)<<",\"frames\":[";
        for(std::size_t f=0;f<frames;++f) {
            const auto dt=reader.real(),weight=reader.real();GraphQuaternion rotation;
            for(auto& x:rotation) x=reader.real();
            for(auto& channel:context.channels) channel=reader.real();
            for(auto& selector:context.selectors) selector.requested=reader.count(255);
            const auto leaves=manifest.graph.evaluate_frame(root,weight,rotation,context,dt);
            if(f) output<<',';snapshot(output,context,leaves);
        }
        output<<"]}";
    }
    reader.end();output<<"]}";
}
}
