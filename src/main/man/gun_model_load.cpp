#include "gun_model_load.hpp"
#include "skinned_mesh.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& message){throw std::runtime_error(message);}
void require(bool condition,const char* message){if(!condition)fail(message);}
std::string hex(std::size_t v){
    static const char* digits="0123456789abcdef";
    std::string s;
    do{s.insert(s.begin(),digits[v&15]);v>>=4;}while(v);
    return s;
}

using Lanes=std::array<float,4>;
using Matrix=std::array<Lanes,4>;     // row k = VU qword k (row 3 the translation)

std::uint32_t bits(float f){return std::bit_cast<std::uint32_t>(f);}
float real_of(std::uint32_t b){return std::bit_cast<float>(b);}
bool exponent_max(float f){return (bits(f)&0x7f800000U)==0x7f800000U;}
bool denormal(float f){return (bits(f)&0x7f800000U)==0&&(bits(f)&0x007fffffU)!=0;}
float signed_zero(float f){return real_of(bits(f)&0x80000000U);}
float flush(float f){return denormal(f)?signed_zero(f):f;}

// VU0 micro-mode upper arithmetic (tools/ee/ee_vu0.py): operands denormal -> signed zero,
// binary32 round to nearest, a denormal result -> signed zero. An operand or result with
// exponent 255 (the interpreter clamps an overflow, the hardware differs) is not ported.
float vu_operand(float f,const char* where){
    if(exponent_max(f))fail(std::string(where)+": VU0 operand with exponent 255 (not ported)");
    return flush(f);
}
float vu_result(float f,const char* where){
    if(exponent_max(f))fail(std::string(where)+": VU0 result overflows (the interpreter clamps; not ported)");
    return flush(f);
}
float vmul(float a,float b,const char* where){return vu_result(vu_operand(a,where)*vu_operand(b,where),where);}
float vadd(float a,float b,const char* where){return vu_result(vu_operand(a,where)+vu_operand(b,where),where);}

// EE FPU add.s / VU0 macro VADD: the interpreter (tools/ee/ee.py) honours denormal operands
// and flushes the result; the hardware flushes denormal operands. Only sums on which both
// agree (and that stay finite) are ported.
float ee_add(float a,float b,const char* where){
    if(exponent_max(a)||exponent_max(b))fail(std::string(where)+": add.s / vadd operand with exponent 255 (not ported)");
    const float interpreter=flush(a+b);
    const float hardware=flush(flush(a)+flush(b));
    if(exponent_max(interpreter)||exponent_max(hardware))fail(std::string(where)+": add.s / vadd overflows (not ported)");
    if(bits(interpreter)!=bits(hardware))fail(std::string(where)+": add.s / vadd on a denormal operand (interpreter and hardware differ; not ported)");
    return hardware;
}

// VMINI / VMAX by value; equal values of different encodings (+0 / -0) and denormal or
// exponent-255 operands are not ported.
float vu_minmax(float a,float b,bool maximum,const char* where){
    if(exponent_max(a)||exponent_max(b)||denormal(a)||denormal(b))fail(std::string(where)+": VMINI / VMAX operand not ported (denormal or exponent 255)");
    if(a==b){
        if(bits(a)!=bits(b))fail(std::string(where)+": VMINI / VMAX of +0 and -0 (not ported)");
        return a;
    }
    return maximum?(a<b?b:a):(a<b?a:b);
}

// ((m0*x + m1*y) + m2*z) + m3*w per lane (vmulax / vmadday / vmaddaz / vmaddw).
Lanes combine(const Matrix& m,const Lanes& v,const char* where){
    Lanes out{};
    for(std::size_t k=0;k<4;++k){
        float acc=vmul(m[0][k],v[0],where);
        acc=vadd(acc,vmul(m[1][k],v[1],where),where);
        acc=vadd(acc,vmul(m[2][k],v[2],where),where);
        out[k]=vadd(acc,vmul(m[3][k],v[3],where),where);
    }
    return out;
}

// The VU0 position-query program (microcode set 1) on one chunk: what it leaves at VU0
// qword 5 + 2 i (the structure of actor_hit_volumes.cpp hit_query_positions, with the
// micro-mode rounding of every product and sum checked).
std::vector<Lanes> query_positions(const SkinChunk& chunk,const std::array<Matrix,11>& m){
    constexpr const char* where="0038d720";
    const auto n=chunk.vertices.size();
    require(n>=1&&n<=30,"0038d720: chunk vertex count outside 1..30 (not ported)");
    require(!chunk.groups.empty()&&chunk.groups.size()<=8,"0038d720: chunk group count outside 1..8 (not ported)");
    for(const auto& g:chunk.groups){
        require(g.bone<m.size(),"0038d720: group bone outside the 11 matrices (VU0 qwords past 216 are stale)");
        require(!g.weights.empty()&&g.first_vertex<=n&&g.weights.size()<=n-g.first_vertex,"0038d720: group range outside the chunk (not ported)");
    }
    require(chunk.groups[0].first_vertex==0,"0038d720: the first group does not start at vertex 0 (not ported)");
    auto position=[&](std::size_t i){
        const auto& p=chunk.vertices[i].position;
        return Lanes{p.x,p.y,p.z,1.0F};
    };
    std::vector<Lanes> out(n);
    const auto& g0=chunk.groups[0];
    const auto& b0=m[g0.bone];
    if(chunk.groups.size()==1){
        for(std::size_t i=0;i<g0.weights.size();++i)out[i]=combine(b0,position(i),where);
        for(std::size_t i=g0.weights.size();i<n;++i){
            const auto& v=chunk.vertices[i];
            out[i]={32896.0F+v.uv[0],32896.0F+v.uv[1],32896.0F+v.normal.x,32896.0F+v.normal.y};
        }
        return out;
    }
    for(std::size_t i=0;i<g0.weights.size();++i){
        const auto t=combine(b0,position(i),where);
        const float w=g0.weights[i];
        for(std::size_t k=0;k<4;++k)out[i][k]=vmul(t[k],w,where);
    }
    for(std::size_t i=g0.weights.size();i<n;++i)
        for(std::size_t k=0;k<4;++k)out[i][k]=vmul(b0[3][k],0.0F,where);
    for(std::size_t g=1;g<chunk.groups.size();++g){
        const auto& group=chunk.groups[g];
        for(std::size_t j=0;j<group.weights.size();++j){
            const auto i=group.first_vertex+j;
            const auto t=combine(m[group.bone],position(i),where);
            const float w=group.weights[j];
            for(std::size_t k=0;k<4;++k)out[i][k]=vadd(vmul(t[k],w,where),out[i][k],where);
        }
    }
    return out;
}

// HOG bytes (offsets from the HOG base). Reads that decide something throw on bytes
// outside the HOG or undefined; copies carry them as undefined.
struct HogView {
    const GunHog& hog;
    bool has(std::int64_t o)const{return o>=0&&o<static_cast<std::int64_t>(hog.bytes.size());}
    bool known(std::int64_t o)const{return has(o)&&(hog.defined.empty()||hog.defined[static_cast<std::size_t>(o)]);}
    std::uint8_t byte(std::int64_t o,const char* where)const{
        if(!known(o))fail(std::string(where)+": reads HOG +"+hex(static_cast<std::size_t>(o<0?0:o))+(has(o)?" (undefined)":" (outside the HOG)"));
        return hog.bytes[static_cast<std::size_t>(o)];
    }
    std::uint32_t word(std::int64_t o,const char* where)const{
        std::uint32_t v=0;
        for(int k=3;k>=0;--k)v=(v<<8)|byte(o+k,where);
        return v;
    }
};

// The loader's stack sp+0x160..sp+0x79f: the SKL parse buffer (0..0x43f), the SKM records
// (0x440 + 16 k) and the SKM names (0x540 + 16 k). Sixteen SKM entries fit; the reader
// struct follows at 0x640.
struct Stack {
    static constexpr std::size_t size=0x640,records=0x440,names=0x540;
    std::array<std::uint8_t,size> b{};
    std::array<bool,size> d{};
    void store(std::size_t o,std::uint8_t v,const char* where){
        if(o>=size)fail(std::string(where)+": writes past the loader's stack buffers (reader struct)");
        b[o]=v;d[o]=true;
    }
    void check(std::size_t o,std::size_t n,const char* where)const{
        if(o+n>size)fail(std::string(where)+": reads past the loader's stack buffers (reader struct)");
    }
};

// Whether the lower 64 bits of the 16 bytes at p (with definedness dd) are zero.
bool low64_zero(const std::uint8_t* p,const bool* dd,const char* where){
    bool undefined=false;
    for(int k=0;k<8;++k){
        if(!dd[k]){undefined=true;continue;}
        if(p[k]!=0)return false;
    }
    if(undefined)fail(std::string(where)+": a zero test reads stale bytes");
    return true;
}

// 00406570(buffer, data, size): the SKL text into 32-byte name pairs.
void parse_skl(Stack& s,const HogView& hog,std::int64_t data,std::uint32_t size){
    constexpr const char* where="00406570";
    const std::int64_t end=data+size;
    std::size_t t7=0;
    std::int64_t src=data;
    if(static_cast<std::uint32_t>(data)<static_cast<std::uint32_t>(end)){
        for(;;){
            auto c=static_cast<std::int8_t>(hog.byte(src,where));
            ++src;
            if(c>=0x61&&c<0x7b)c=static_cast<std::int8_t>(c-0x20);
            s.store(t7,static_cast<std::uint8_t>(c),where);
            ++t7;
            if(!(t7<0x440))break;
            if(c==0||c==' '||c=='#'||c=='$'||c=='*'||c=='\t'||c=='\n')--t7;
            if(c=='\r'){
                s.store(t7-1,0,where);
                while(t7&15){s.store(t7,0,where);++t7;}
            }
            if(!(src<end))break;
        }
    }
    s.store(t7,0,where);
    while(t7&15){s.store(t7,0,where);++t7;}
    std::size_t k=0;
    for(;;++k){
        const auto o=0x40+32*k;
        s.check(o,16,where);
        if(low64_zero(&s.b[o],&s.d[o],where))break;
    }
    const auto o=0x40+32*k;
    for(const std::size_t to:{o+0x20,o+0x30}){
        s.check(to,16,where);
        for(std::size_t j=0;j<16;++j){s.b[to+j]=s.b[o+j];s.d[to+j]=s.d[o+j];}
    }
}

// 00407ec0(dst, n, src): signed chars until NUL, '.' or n, zero to n, then dst[n] = 0.
template<class Store> void copy_name(Store&& store,std::size_t n,const std::string& src){
    std::size_t t=0;
    for(;t<n&&t<src.size();++t){
        if(src[t]=='.')break;
        store(t,static_cast<std::uint8_t>(src[t]));
    }
    for(;t<n;++t)store(t,0);
    store(n,0);
}

// 00400090(record, data, size): the SKM record.
void skm_record(Stack& s,std::size_t rec,const HogView& hog,std::int64_t data,std::uint32_t size){
    constexpr const char* where="00400090";
    const auto first=hog.byte(data,where);
    const auto head=static_cast<std::int32_t>(((static_cast<std::int32_t>(static_cast<std::int8_t>(first))*2+0x11))&~0xf);
    const auto r0=static_cast<std::uint32_t>(data)+static_cast<std::uint32_t>(head);
    const auto r8=static_cast<std::uint32_t>(data);
    std::int64_t t2=(data+static_cast<std::int64_t>(size)-16)&~std::int64_t(15);
    std::int32_t t3=0;
    for(;;){
        if(t2<0)fail("00400090: the trailing-zero scan runs before the HOG");
        bool zero=true,undefined=false;
        for(int k=0;k<16;++k){
            if(!hog.known(t2+k)){
                if(!hog.has(t2+k))fail("00400090: the trailing-zero scan reads outside the HOG");
                undefined=true;continue;
            }
            if(hog.hog.bytes[static_cast<std::size_t>(t2+k)]!=0){zero=false;break;}
        }
        if(zero&&undefined)fail("00400090: the trailing-zero scan reads undefined HOG bytes");
        if(!zero)break;
        ++t3;
        t2-=16;
    }
    const auto diff=static_cast<std::int32_t>(r0-r8);
    const std::int32_t qwords=static_cast<std::int32_t>(size)>>4;
    const auto count=static_cast<std::uint16_t>(static_cast<std::uint32_t>(qwords-t3-diff/16));
    auto put32=[&](std::size_t o,std::uint32_t v){for(int k=0;k<4;++k)s.store(rec+o+k,static_cast<std::uint8_t>(v>>(8*k)),where);};
    put32(0,r0);
    s.store(rec+4,static_cast<std::uint8_t>(count),where);
    s.store(rec+5,static_cast<std::uint8_t>(count>>8),where);
    s.store(rec+6,0,where);
    s.store(rec+7,first,where);
    put32(8,r8);
    put32(12,0);
}

// strcmp / strncmp equality (00129218 / 001298b8) of a defined literal with 16 bytes that
// may hold stale bytes.
bool equal_n(const char* literal,const std::uint8_t* b,const bool* d,std::size_t n,const char* where){
    for(std::size_t i=0;i<n;++i){
        if(i>=16||!d[i])fail(std::string(where)+": a name compare reads stale bytes");
        if(static_cast<std::uint8_t>(literal[i])!=b[i])return false;
        if(literal[i]==0)return true;
    }
    return true;
}

struct ModelIo {
    GunModel& m;
    void put(std::size_t o,std::uint8_t v){m.bytes.at(o)=v;m.defined.at(o)=true;}
    void put16(std::size_t o,std::uint16_t v){put(o,static_cast<std::uint8_t>(v));put(o+1,static_cast<std::uint8_t>(v>>8));}
    void put32(std::size_t o,std::uint32_t v){for(int k=0;k<4;++k)put(o+k,static_cast<std::uint8_t>(v>>(8*k)));}
    void zero(std::size_t o,std::size_t n){for(std::size_t k=0;k<n;++k)put(o+k,0);}
    void copy(std::size_t to,std::size_t from,std::size_t n){
        for(std::size_t k=0;k<n;++k){m.bytes.at(to+k)=m.bytes.at(from+k);m.defined.at(to+k)=m.defined.at(from+k);}
    }
    std::uint8_t byte(std::size_t o,const char* where)const{
        if(!m.defined.at(o))fail(std::string(where)+": reads stale model +"+hex(o));
        return m.bytes[o];
    }
    std::uint32_t word(std::size_t o,const char* where)const{
        std::uint32_t v=0;
        for(int k=3;k>=0;--k)v=(v<<8)|byte(o+k,where);
        return v;
    }
    float real(std::size_t o,const char* where)const{return real_of(word(o,where));}
};

struct Member { std::string name; std::int64_t data; std::uint32_t size; };

std::string hog_string(const HogView& hog,std::int64_t o,const char* where){
    std::string s;
    for(;;){
        const auto c=hog.byte(o++,where);
        if(c==0)return s;
        s+=static_cast<char>(c);
    }
}

std::vector<Member> hog_members(const HogView& hog){
    constexpr const char* where="0038cd90 (HOG reader 0036d200 / 0036d380 / 0036d4e0)";
    const auto count=static_cast<std::int32_t>(hog.word(4,where));
    const std::int64_t table=hog.word(8,where),names=hog.word(12,where),data=hog.word(16,where);
    std::vector<Member> out;
    std::int64_t name=names;
    for(std::int32_t i=0;i<count;++i){
        Member m;
        m.name=hog_string(hog,name,where);
        name+=static_cast<std::int64_t>(m.name.size())+1;
        const auto a=hog.word(table+4*i,where),b=hog.word(table+4*i+4,where);
        m.size=b-a;
        m.data=static_cast<std::uint32_t>(static_cast<std::uint32_t>(data)+a);
        out.push_back(std::move(m));
    }
    return out;
}

std::string extension(const std::string& name){
    const auto p=name.rfind('.');
    return p==std::string::npos?name:name.substr(p);
}
}

std::uint32_t GunModel::word(std::size_t offset)const{
    std::uint32_t v=0;
    for(int k=3;k>=0;--k){
        if(!defined.at(offset+k))fail("gun model: stale byte +"+hex(offset+k));
        v=(v<<8)|bytes[offset+k];
    }
    return v;
}
float GunModel::real(std::size_t offset)const{return real_of(word(offset));}
bool GunModel::all_defined(std::size_t offset,std::size_t count)const{
    for(std::size_t k=0;k<count;++k)if(!defined.at(offset+k))return false;
    return true;
}

void gun_model_construct(GunModel& model,std::string_view name,std::int16_t index,
                         std::uint8_t t0,std::uint8_t t1,std::uint8_t t2,std::uint8_t t3,const std::optional<GunShellRecord>& a3){
    model.defined.fill(false);
    ModelIo io{model};
    std::string n;
    for(const char c:name){if(c==0)break;n+=c;}
    copy_name([&](std::size_t o,std::uint8_t v){io.put(o,v);},0x1f,n);
    io.put32(0x400,0);
    io.put(0x40d,t2);
    io.put(0x40e,t3);
    io.put32(0x404,0);
    if(a3){io.put32(0x404,a3->chunk_start);io.put16(0x408,a3->qwords);}
    io.put16(0x40a,static_cast<std::uint16_t>(index));
    io.put(0x40c,0);
    io.put(0x40f,0);
    for(std::size_t o=0x410;o<=0x414;++o)io.put(o,0xff);
    io.put(0x417,0xff);
    io.put(0x415,0);
    io.put(0x416,0xff);
    io.put32(0x41c,0);
    io.put(0x418,t1);
    io.put(0x419,t0);
    // (7f7fffff x3, 1) / (ff7fffff x3, 1) through VMINI / VMAX .xyz with zero: (0, 0, 0, 1) twice.
    for(const std::size_t box:{0x3e0U,0x3f0U}){io.put32(box,0);io.put32(box+4,0);io.put32(box+8,0);io.put32(box+12,0x3f800000U);}
    io.zero(0x140,0x180);
    io.zero(0x120,0x20);
    io.put32(0x13c,0x3f800000U);
}

std::vector<GunModelUpload> gun_model_bounds(GunModel& model,const GunHog& hog_in){
    constexpr const char* where="0038d720";
    ModelIo io{model};
    const HogView hog{hog_in};
    // the matrices: rows 0..2 identity, row 3 = bone[i] + bone[parent] (VADD), w = 1.
    std::array<Matrix,11> matrices{};
    for(std::size_t i=0;i<11;++i){
        const auto bone=0x150+32*i;
        const auto parent=static_cast<std::int32_t>(io.word(bone+12,where));
        if(parent<0||parent>11)fail("0038d720: bone parent index outside the 12 GUN bones (reads the LOD records)");
        const auto pb=0x150+32*static_cast<std::size_t>(parent);
        Matrix& m=matrices[i];
        m[0]={1.0F,0.0F,0.0F,0.0F};m[1]={0.0F,1.0F,0.0F,0.0F};m[2]={0.0F,0.0F,1.0F,0.0F};
        for(std::size_t k=0;k<3;++k)m[3][k]=ee_add(io.real(bone+4*k,where),io.real(pb+4*k,where),where);
        m[3][3]=1.0F;
    }
    std::vector<GunModelUpload> uploads;
    const auto parts=io.byte(0x40c,where);
    for(std::size_t t2=0;t2<parts;++t2){
        const auto rec=0x340+16*t2;
        bool skip=false;
        for(const std::size_t f:{0x410U,0x411U,0x412U,0x414U})
            if(static_cast<std::int8_t>(io.byte(f,where))==static_cast<std::int32_t>(t2))skip=true;
        if(skip)continue;
        std::int64_t t0=io.word(rec,where);
        if(t0==0)continue;
        const auto count=io.byte(rec+7,where);
        if(count==0)continue;
        const std::int64_t data=io.word(rec+8,where);
        // the chunks this record's loop sends, as the SKM decoder reads them
        std::size_t span_end=static_cast<std::size_t>(t0);
        for(std::size_t a=0;a<count;++a)span_end+=std::size_t(hog.byte(data+2+2*a,where))*16;
        if(static_cast<std::int64_t>(span_end)>static_cast<std::int64_t>(hog_in.bytes.size()))
            fail("0038d720: LOD1 chunks run past the HOG");
        for(std::size_t o=static_cast<std::size_t>(data);o<span_end;++o)hog.byte(static_cast<std::int64_t>(o),where);
        if(static_cast<std::uint32_t>(t0-data)!=((2+2*std::uint32_t(count)+15)&~15U))
            fail("0038d720: LOD1 record +0 is not the chunk start after the descriptors (not ported)");
        SkinnedMesh mesh;
        try{
            mesh=decode_skinned_mesh(hog_in.bytes.subspan(static_cast<std::size_t>(data),span_end-static_cast<std::size_t>(data)),11);
        }catch(const std::exception& e){
            fail(std::string("0038d720: LOD1 chunk not decodable as a query packet: ")+e.what());
        }
        require(mesh.chunks.size()==count,"0038d720: LOD1 chunk count differs from the descriptors");
        for(std::size_t a=0;a<count;++a){
            const auto qwc=hog.byte(data+2+2*a,where);
            const auto nv=hog.byte(data+3+2*a,where);
            uploads.push_back({static_cast<std::uint32_t>(t0),qwc,0});
            t0+=std::int64_t(qwc)*16;
            const auto& chunk=mesh.chunks[a];
            if(chunk.microprogram_entries.size()!=1)fail("0038d720: LOD1 chunk with render continuations (MSCAL after the query entry; not ported)");
            // Interpreter-verified pairs: single-bone chunks with entries 8 / 10 / 12 / 14, multi-bone
            // chunks with entry 12 (the disc's only multi-bone gun chunk, synthetic weights).
            const bool single=chunk.groups.size()==1;
            const auto e=chunk.microprogram_entry;
            if(single?(e!=8&&e!=10&&e!=12&&e!=14):(e!=12))
                fail("0038d720: LOD1 "+std::string(single?"single":"multi")+"-bone chunk MSCAL entry "+std::to_string(e)+" not interpreter-verified");
            uploads.back().entry=chunk.microprogram_entry;
            require(chunk.vertices.size()==nv,"0038d720: chunk vertex count differs from its descriptor");
            const auto positions=query_positions(chunk,matrices);
            for(std::size_t v=0;v<nv;++v){
                for(std::size_t k=0;k<3;++k){
                    const auto lo=0x3e0+4*k,hi=0x3f0+4*k;
                    io.put32(lo,bits(vu_minmax(io.real(lo,where),positions[v][k],false,where)));
                    io.put32(hi,bits(vu_minmax(io.real(hi,where),positions[v][k],true,where)));
                }
            }
        }
    }
    return uploads;
}

std::vector<GunModelUpload> gun_model_load(GunModel& model,const GunHog& hog_in,GunTextureHost* textures){
    constexpr const char* where="0038cd90";
    ModelIo io{model};
    const HogView hog{hog_in};
    Stack s;
    io.put32(0x400,0);
    io.put(0x417,0);
    for(std::size_t k=0;k<3;++k){io.put32(0x3e0+4*k,0x7f7fffffU);io.put32(0x3f0+4*k,0xff7fffffU);}
    io.put32(0x3ec,0x3f800000U);
    io.put32(0x3fc,0x3f800000U);
    io.zero(0x140,0x180);
    const auto members=hog_members(hog);
    std::size_t skms=0;
    bool skl=false;
    for(const auto& mem:members){
        const auto ext=extension(mem.name);
        if(ext==".SKL"){
            parse_skl(s,hog,mem.data,mem.size);
            for(std::size_t k=0;k<0x100;++k){model.bytes[0x20+k]=s.b[0x40+k];model.defined[0x20+k]=s.d[0x40+k];}
            skl=true;
        }
        if(ext==".GUN"){
            const auto from=mem.data&~std::int64_t(15);
            std::int32_t n=24;
            if(mem.size<0x180){
                io.zero(0x140,0x180);
                n=static_cast<std::int32_t>(mem.size)>>4;
            }
            std::size_t q=0;
            do{
                for(std::size_t k=0;k<16;++k){
                    const auto o=from+static_cast<std::int64_t>(16*q+k);
                    model.bytes.at(0x140+16*q+k)=hog.known(o)?hog_in.bytes[static_cast<std::size_t>(o)]:0;
                    model.defined.at(0x140+16*q+k)=hog.known(o);
                }
                ++q;
            }while(--n>0);
        }
        if(ext==".SKM"){
            if(skms>=16)fail("0038cd90: more than sixteen SKM members (the seventeenth overwrites the loader's reader struct)");
            const auto name=Stack::names+16*skms;
            copy_name([&](std::size_t o,std::uint8_t v){s.store(name+o,v,"00407ec0");},15,mem.name);
            skm_record(s,Stack::records+16*skms,hog,mem.data,mem.size);
            ++skms;
        }
    }
    if(!skl){
        io.put(0x40c,0);
        io.put(0x417,0xfe);
        return {};
    }
    std::size_t parts=0;
    for(;;++parts){
        const auto o=0x20+32*parts;
        if(o+16>GunModel::size)fail("0038cd90: part count scan runs past the model");
        if(low64_zero(&model.bytes[o],&model.defined[o],where))break;
    }
    if(parts>8)fail("0038cd90: more than eight SKL parts (records overlap; not ported)");
    io.put(0x40c,static_cast<std::uint8_t>(parts));
    for(std::size_t p=0;p<parts;++p){
        std::array<std::uint8_t,16> nb{};
        std::array<bool,16> nd{};
        for(std::size_t k=0;k<16;++k){nb[k]=model.bytes[0x20+32*p+k];nd[k]=model.defined[0x20+32*p+k];}
        auto find=[&](){
            std::size_t found=16;
            for(std::size_t r=0;r<skms;++r){
                bool differ=false,undefined=false;
                for(std::size_t k=0;k<16;++k){
                    if(!nd[k]){undefined=true;continue;}
                    if(nb[k]!=s.b[Stack::names+16*r+k])differ=true;
                }
                if(differ)continue;
                if(undefined)fail("0038cd90: a part name compare reads stale bytes");
                found=r;
            }
            return found;
        };
        auto copy_record=[&](std::size_t to,std::size_t r){
            for(std::size_t k=0;k<16;++k){model.bytes[to+k]=s.b[Stack::records+16*r+k];model.defined[to+k]=s.d[Stack::records+16*r+k];}
        };
        const auto lod0=0x2c0+16*p,lod1=0x340+16*p;
        if(const auto r=find();r<16)copy_record(lod0,r);
        else{io.put32(lod0,0);io.put16(lod0+4,0);}
        nb[14]='1';nd[14]=true;
        if(const auto r=find();r<16)copy_record(lod1,r);
        else io.copy(lod1,lod0,16);
        const auto s1=static_cast<std::uint8_t>(p);
        if(equal_n("_HOLS",nb.data(),nd.data(),5,where))io.put(0x410,s1);
        if(equal_n("_LIGH",nb.data(),nd.data(),5,where))io.put(0x412,s1);
        if(equal_n("1BODY",nb.data(),nd.data(),5,where))io.put(0x413,s1);
        if(equal_n("2BODY",nb.data(),nd.data(),5,where))io.put(0x414,s1);
        if(equal_n("_FLAR",nb.data(),nd.data(),5,where)){
            io.put(0x411,s1);
            if(equal_n("_FLAR_STU_OTHE",nb.data(),nd.data(),14,where)){io.put(0x415,1);io.put(0x411,0xff);}
        }
    }
    if(textures){
        textures->tinit();
        textures->tload(members.front().name);
    }
    for(std::size_t p=0;p<parts;++p){
        const auto at=0x3c0+2*p,atd=0x3d0+2*p;
        if(!textures){
            for(const auto o:{at,at+1,atd,atd+1})model.defined[o]=false;
            continue;
        }
        std::array<std::uint8_t,16> nb{};
        std::array<bool,16> nd{};
        for(std::size_t k=0;k<16;++k){nb[k]=model.bytes[0x30+32*p+k];nd[k]=model.defined[0x30+32*p+k];}
        io.put16(at,static_cast<std::uint16_t>(textures->tex(nb,nd)));
        nb[14]='1';nd[14]=true;
        io.put16(atd,static_cast<std::uint16_t>(textures->tex(nb,nd)));
        auto half=[&](std::size_t o){return static_cast<std::int16_t>(io.byte(o,where)|(io.byte(o+1,where)<<8));};
        if(half(at)==-1)io.put16(at,static_cast<std::uint16_t>(textures->texd()));
        if(half(atd)==-1)io.copy(atd,at,2);
    }
    io.copy(0x120,0x1a0,16);
    io.copy(0x130,0x1b0,16);
    std::int32_t s1=3;
    do{
        s1=static_cast<std::int32_t>(io.word(0x13c,where));
        if(s1<0||s1>11)fail("0038cd90: +13c chain index outside the 12 GUN bones (reads the LOD records)");
        const auto bone=0x150+32*static_cast<std::size_t>(s1);
        for(std::size_t k=0;k<4;++k)
            io.put32(0x130+4*k,bits(ee_add(io.real(0x130+4*k,where),io.real(bone+4*k,where),where)));
    }while(s1!=0);
    io.put32(0x13c,0x3f800000U);
    auto uploads=gun_model_bounds(model,hog_in);
    if(textures)textures->tfree();
    return uploads;
}

}
