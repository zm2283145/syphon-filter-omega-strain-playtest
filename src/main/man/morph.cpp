#include "morph.hpp"
#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }
bool zero(std::span<const std::uint8_t> data) {
    return std::all_of(data.begin(), data.end(), [](auto value){return value==0;});
}
std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at) {
    if(at>bytes.size()||bytes.size()-at<4) invalid("Truncated morph word");
    return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|
        (std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);
}
void byte_array(std::ostream& out,std::span<const std::uint8_t> bytes) {
    out<<'[';for(std::size_t i=0;i<bytes.size();++i){if(i)out<<',';out<<unsigned(bytes[i]);}out<<']';
}
}
ShapeNames decode_shape_names(std::span<const std::uint8_t> bytes) {
    if(bytes.empty()||bytes.size()%16||bytes.size()>4096) invalid("Invalid shape-name extent");
    ShapeNames result;
    for(std::size_t at=0;at<bytes.size();at+=16) {
        if(zero(bytes.subspan(at,8))) {
            if(result.empty()||!zero(bytes.subspan(at))) invalid("Invalid shape-name terminator/padding");
            return result;
        }
        std::array<std::uint8_t,16> key{};
        std::copy_n(bytes.begin()+at,16,key.begin());result.push_back(key);
    }
    invalid("Missing shape-name terminator");
}
MorphData decode_morph(std::span<const std::uint8_t> bytes,MorphKind kind,
                      std::uint32_t shape_count,std::uint32_t chunk_count) {
    if(kind!=MorphKind::face&&kind!=MorphKind::hand) invalid("Unknown morph kind");
    if(!shape_count||shape_count>256||!chunk_count||chunk_count>127) invalid("Invalid morph counts");
    if(bytes.empty()||bytes.size()>16*1024*1024) invalid("Invalid morph extent");
    MorphData result{kind,chunk_count,{}};
    std::size_t cursor=0;
    const auto words=(chunk_count+31)/32;
    const std::size_t stride=4*(1+words), table_end=shape_count*stride;
    if(kind==MorphKind::face) {
        cursor=(table_end+15)/16*16;
        // Table alignment bytes are unconsumed (one source asset uses 0x77).
        if(cursor>bytes.size()) invalid("Truncated morph table");
    }
    for(std::uint32_t shape=0;shape<shape_count;++shape) {
        MorphShape record;
        if(kind==MorphKind::face) {
            if(word(bytes,shape*stride)!=cursor) invalid("Noncontiguous sparse morph offset");
            if(chunk_count%32 && (word(bytes,shape*stride+4*words)>>(chunk_count%32))) invalid("Morph mask exceeds chunks");
        }
        for(std::uint32_t chunk=0;chunk<chunk_count;++chunk) {
            const bool present=kind==MorphKind::hand||
                (word(bytes,shape*stride+4*(1+chunk/32))&(std::uint32_t(1)<<(chunk%32)));
            if(!present) {record.chunks.push_back(std::nullopt);continue;}
            const std::size_t count=kind==MorphKind::hand?48:32;
            if(cursor>bytes.size()||bytes.size()-cursor<count) invalid("Truncated morph payload");
            record.chunks.emplace_back(std::vector<std::uint8_t>(bytes.begin()+cursor,bytes.begin()+cursor+count));
            cursor+=count;
        }
        result.shapes.push_back(std::move(record));
    }
    if(bytes.size()!=cursor&&(bytes.size()!=(cursor+63)/64*64||!zero(bytes.subspan(cursor)))) invalid("Invalid morph trailing extent/padding");
    return result;
}
void write_shape_names(std::ostream& out,const ShapeNames& names) {
    out<<"{\"keys\":[";for(std::size_t i=0;i<names.size();++i){if(i)out<<',';byte_array(out,names[i]);}out<<"]}";
}
void write_morph(std::ostream& out,const MorphData& data) {
    out<<"{\"kind\":\""<<(data.kind==MorphKind::face?"face":"hand")<<"\",\"chunk_count\":"<<data.chunk_count<<",\"shapes\":[";
    for(std::size_t s=0;s<data.shapes.size();++s) {
        if(s)out<<',';out<<"{\"chunks\":[";
        const auto& chunks=data.shapes[s].chunks;
        for(std::size_t c=0;c<chunks.size();++c){if(c)out<<',';if(chunks[c])byte_array(out,*chunks[c]);else out<<"null";}
        out<<"]}";
    }
    out<<"]}";
}
}
