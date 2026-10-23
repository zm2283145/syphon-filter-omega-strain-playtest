#include "actor_hit_volumes.hpp"
#include "character_shadows.hpp"
#include "vu_float.hpp"
#include "weapon_shot_frame.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const char* message){throw std::runtime_error(message);}
void require(bool condition,const char* message){if(!condition)fail(message);}
float fb(std::uint32_t bits){return std::bit_cast<float>(bits);}

// vmulax / vmadday / vmaddaz / vmaddw: ((m0*v.x + m1*v.y) + m2*v.z) + m3*v.w per lane.
RootVector combine(const ShotFrame& m,const RootVector& v){
    RootVector out{};
    for(std::size_t k=0;k<4;++k){
        float acc=m[0][k]*v[0];
        acc=acc+m[1][k]*v[1];
        acc=acc+m[2][k]*v[2];
        out[k]=acc+m[3][k]*v[3];
    }
    return out;
}
// vopmula / vopmsub, w = vf0.w - vf0.w.
RootVector cross(const RootVector& a,const RootVector& b){
    return {a[1]*b[2]-b[1]*a[2],a[2]*b[0]-b[2]*a[0],a[0]*b[1]-b[0]*a[1],0.0F};
}
// vmul.xyz, (x + y) + z in the w lane, vrsqrt Q = 1 / sqrt, vmulq.xyz (w kept).
RootVector normalized(const RootVector& v,const char* what){
    const float s=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];
    require(s!=0.0F,what);
    const float q=vu::rsqrt(s);
    return {v[0]*q,v[1]*q,v[2]*q,v[3]};
}
ShotFrame rigid_inverse(const ShotFrame& m){   // 00133200 (shadow_rigid_inverse, interpreter-verified)
    ShadowMatrix s;
    for(std::size_t k=0;k<4;++k)s.column[k]=m[k];
    const auto r=shadow_rigid_inverse(s);
    return {r.column[0],r.column[1],r.column[2],r.column[3]};
}
const SkinChunk& chunk_at(const SkinnedMesh& mesh,std::size_t index){
    require(index<mesh.chunks.size(),"Hit volumes: chunk index outside the LOD0 mesh (the original reads past its descriptors)");
    return mesh.chunks[index];
}
float sq_distance(const RootVector& p){        // mula.s z*z, madd.s + x*x
    const float acc=p[2]*p[2];
    return acc+p[0]*p[0];
}
void update_near_far(HitVolumeRecord& r,float score,float y,std::int32_t code,std::size_t vertex,std::size_t chunk,std::size_t part){
    if(score<r.near_score){
        r.near_score=score;r.near_y=y;r.code=code;
        r.vertex=static_cast<std::uint16_t>(vertex);r.chunk=static_cast<std::uint16_t>(chunk);r.part=static_cast<std::uint16_t>(part);
    }
    if(!(score<=r.far_score)){r.far_score=score;r.far_y=y;r.far_code=code;}
}
// VU0 data memory qwords 4..103 as the chunks sent during one query leave them.
// Per chunk (packet order; VIF0 ignores the FLG bit, the CYCLE writes are
// contiguous as the program's own addressing requires): the attribute UNPACK
// (STMOD 1, row 0x47000000) at qwords 4 + 2i, the weight and primitive UNPACKs
// from 4 + 2n, then the query program's stores (micro-PCs 86..101, 104, 146..167,
// 197..211; exactly the output slots 5 + 2i that hit_query_positions defines,
// no other SQ/SQI/SQD/ISW/ISWR). The metadata UNPACK (qwords 104 - records..103)
// and the first chunk's qword 117 are not tracked; 0038ba80 reads only 5 + 2v < 64.
struct QueryVu0 {
    std::array<std::array<std::uint32_t,4>,104> q{};
    std::array<bool,104> written{};
    bool tails_complete=true;
};
std::array<std::uint32_t,4> words(const RootVector& v){
    return {std::bit_cast<std::uint32_t>(v[0]),std::bit_cast<std::uint32_t>(v[1]),
            std::bit_cast<std::uint32_t>(v[2]),std::bit_cast<std::uint32_t>(v[3])};
}
void vu0_run(QueryVu0& vu,const SkinChunk& chunk,const std::vector<RootVector>& positions){
    const auto n=chunk.vertices.size();
    auto put=[&](std::size_t a,const std::array<std::uint32_t,4>& w){
        require(a<vu.q.size(),"Hit volumes: chunk UNPACK reaches the VU0 metadata qwords");
        vu.q[a]=w;vu.written[a]=true;
    };
    for(std::size_t i=0;i<n;++i){
        const auto& v=chunk.vertices[i];
        put(4+2*i,words({32896.0F+v.position.x,32896.0F+v.position.y,32896.0F+v.position.z,32896.0F+v.normal.z}));
        put(5+2*i,words({32896.0F+v.uv[0],32896.0F+v.uv[1],32896.0F+v.normal.x,32896.0F+v.normal.y}));
    }
    std::size_t tail=(chunk.primitives.size()+3)/4;
    if(chunk.groups.size()>1)for(const auto& g:chunk.groups)tail+=(g.weights.size()+3)/4;
    if(chunk.unpack_tail.size()!=tail)vu.tails_complete=false;
    for(std::size_t k=0;k<chunk.unpack_tail.size();++k)put(4+2*n+k,chunk.unpack_tail[k]);
    for(std::size_t i=0;i<n;++i)put(5+2*i,words(positions[i]));
}
std::vector<RootVector> run_chunk(QueryVu0& vu,const SkinChunk& chunk,std::span<const ShotFrame> matrices){
    auto positions=hit_query_positions(chunk,matrices);
    vu0_run(vu,chunk,positions);
    return positions;
}
// 0038bd30: the LOD0 chunks of one part against a fixed squared radius 0.04.
void gore_pass(const HitVolumePart& part,std::size_t index,std::vector<std::uint8_t>& gore,
               std::span<const ShotFrame> matrices,HitVolumeRecord& record,QueryVu0& vu){
    const auto code=static_cast<std::int32_t>(part.flags0&0x1cU);
    const auto& mesh=*part.lod0;
    for(std::size_t c=0;c<mesh.chunks.size();++c){
        const auto positions=run_chunk(vu,mesh.chunks[c],matrices);
        for(std::size_t v=0;v<positions.size();++v){
            const auto& p=positions[v];
            const float d=sq_distance(p);
            if(!(d<fb(0x3d23d70aU)))continue;                                    // 0.04
            const float score=0.25F*p[1]+std::fabs(d);
            update_near_far(record,score,p[1],code,v,c,index);
            const float amount_f=200.0F-16000.0F*d;
            std::int32_t amount=static_cast<std::int32_t>(amount_f);             // cvt.w.s (|value| <= 200)
            if(amount<=0)amount=1;
            std::int32_t sum=gore[32*c+v]+amount;
            if(sum>=0x5b)sum=0x5a;
            gore[32*c+v]=static_cast<std::uint8_t>(sum);
        }
    }
}
// 0038ba80: the hit vertex of the LOD0 chunk and the bones of its groups. The
// position is VU0 qword 5 + 2v after the LOD0 chunk ran; past the chunk's vertices
// that is its weight/primitive UNPACK or what an earlier chunk of this query left
// (the winning LOD1 chunk always wrote that qword, so it is defined).
void hit_point(std::span<const HitVolumePart> parts,std::span<const ShotFrame> matrices,HitVolumeRecord& record,QueryVu0& vu){
    require(record.part<parts.size(),"Hit volumes: hit part outside the model");
    const auto& part=parts[record.part];
    require(part.lod0!=nullptr,"Hit volumes: hit part without a LOD0 mesh (the original reads a null record)");
    const auto& chunk=chunk_at(*part.lod0,record.chunk);
    const auto positions=run_chunk(vu,chunk,matrices);
    const std::size_t slot=5+2*std::size_t(record.vertex);
    require(slot<64&&vu.written[slot],"Hit volumes: 0038ba80 reads a VU0 qword this query did not write");
    if(record.vertex>=positions.size())
        require(vu.tails_complete,"Hit volumes: 0038ba80 reads past the LOD0 chunk and a chunk lacks its raw UNPACK words");
    const auto& w=vu.q[slot];
    record.point=RootVector{fb(w[0]),fb(w[1]),fb(w[2]),fb(w[3])};
    require(!chunk.groups.empty()&&chunk.groups.size()<=8,"Hit volumes: unsupported group count");
    record.group_count=static_cast<std::int8_t>(chunk.groups.size()&0xfU);
    const std::uint32_t base=chunk.groups[0].output_address;
    const std::uint32_t vertex=record.vertex;
    for(std::size_t a=0;a<chunk.groups.size();++a){
        const auto& g=chunk.groups[a];
        const std::uint32_t rel=std::uint32_t(g.output_address)-base;
        const auto count=static_cast<std::uint32_t>(g.weights.size());
        const auto bone=static_cast<std::int32_t>(g.bone);
        const bool inside=!(vertex<rel)&&!(count+rel<vertex);
        record.groups[a]=static_cast<std::int8_t>(inside?bone:-bone);
        if(!inside)continue;
        require(record.code.has_value(),"Hit volumes: record +28 read before it was written");
        if((bone==2||bone==5||bone==8||bone==10)&&*record.code!=0x14)record.code=0xc;
        if((bone==9||bone==14)&&*record.code==8)record.code=4;
    }
}
}

std::uint8_t part_mesh_hit_flags(const std::array<char,16>& cache_name){
    // 001298b8 strncmp(prefix, name + 1, n) == 0.
    auto equal=[&](const char* prefix,std::size_t n){
        for(std::size_t i=0;i<n;++i){
            const char a=prefix[i],b=cache_name[1+i];
            if(a!=b)return false;
            if(a=='\0')return true;
        }
        return true;
    };
    struct Rule {const char* prefix; std::size_t n; std::uint8_t code, gore_off;};
    // 003ffb6c..003ffef0 in order; the last match wins. "BOO" (003ffb74) and "SHI"
    // (003ffd8c) are compared but their results are not used (BOO matches the default 8).
    static constexpr Rule rules[]={
        {"LEG",3,8,0},{"PAN",3,8,0},{"TOR",3,8,0},{"ARM",3,8,0},{"HAN",3,8,0},
        {"EYE",3,4,0},{"NEC",3,4,0},{"MOU",3,4,0},{"HEA",3,4,0},{"BEA",3,0x1c,0},
        {"LEN",3,4,0},{"GLA",3,4,0},{"EAR",3,0x1c,0},{"HAI",3,0x1c,0},{"MAS",3,4,0},
        {"HAT",3,4,0},{"TIE",3,0x1c,0},{"HOL",3,0x1c,0},{"BEL",3,0x1c,0},{"VES",3,0xc,0},
        {"JAC",3,8,0},{"HAT-_HLM",8,0x10,0},{"HAT-_RYU",8,0xc,0x80},{"JACK_FIR",8,0x14,0x80},
        {"VEST_TANK",9,0x14,0x80},{"VEST_REBR",9,0x14,0x80}};
    std::uint8_t code=8,gore_off=0;
    for(const auto& r:rules)if(equal(r.prefix,r.n)){code=r.code;if(r.gore_off)gore_off=r.gore_off;}
    // 003fff04..003fff60: any of the four nine-byte names gives 0xc.
    if(equal("BOOT_FIR_",9)||equal("LEGS_FIR_",9)||equal("SHIR_FIR_",9)||equal("MASK_2FI_",9))code=0xc;
    return static_cast<std::uint8_t>(code|gore_off);
}

std::uint8_t part_mesh_hit_flags(std::string_view cache_name){
    std::array<char,16> name{};
    for(std::size_t i=0;i<name.size()&&i<cache_name.size();++i)name[i]=cache_name[i];
    return part_mesh_hit_flags(name);
}

std::vector<ShotFrame> hit_shot_frames(std::span<const std::array<float,16>> matrices){
    std::vector<ShotFrame> out(matrices.size());
    for(std::size_t i=0;i<matrices.size();++i)
        for(std::size_t k=0;k<4;++k)for(std::size_t j=0;j<4;++j)out[i][k][j]=matrices[i][4*k+j];
    return out;
}

HitRayFrame hit_ray_frame(const RootVector& origin,const RootVector& direction){
    HitRayFrame out;
    const auto dir=normalized(direction,"Hit volumes: zero-length ray direction");
    RootVector row0{},row2{};
    if(dir[2]<fb(0x3f34fdf4U)){
        row0=normalized(cross(dir,RootVector{0.0F,0.0F,1.0F,0.0F}),"Hit volumes: degenerate ray side axis");
        row2=cross(row0,dir);
    }else{
        row2=normalized(cross(RootVector{1.0F,0.0F,0.0F,0.0F},dir),"Hit volumes: degenerate ray side axis");
        row0=cross(dir,row2);
    }
    out.direction=dir;
    out.frame={row0,dir,row2,RootVector{origin[0],origin[1],origin[2],1.0F}};
    out.inverse=rigid_inverse(out.frame);
    return out;
}

std::vector<ShotFrame> hit_node_matrices(std::span<const ShotFrame> nodes,std::span<const ShotFrame> inverse_bind,
    const ShotFrame& component,const ShotFrame& frame_inverse){
    require(nodes.size()>=21,"Hit volumes: the query loads 21 node matrices; the skeleton has fewer");
    require(inverse_bind.size()==nodes.size(),"Hit volumes: inverse bind count differs from the node count");
    const auto ray=shot_frame_node_world(component,frame_inverse);
    std::vector<ShotFrame> out;
    for(std::size_t k=0;k<21;++k)
        out.push_back(shot_frame_node_world(shot_frame_node_world(inverse_bind[k],nodes[k]),ray));
    return out;
}

std::vector<RootVector> hit_query_positions(const SkinChunk& chunk,std::span<const ShotFrame> m){
    const auto n=chunk.vertices.size();
    require(n>=1&&n<=30,"Hit volumes: unsupported chunk vertex count");
    require(!chunk.groups.empty()&&chunk.groups.size()<=8,"Hit volumes: unsupported chunk group count");
    for(const auto& g:chunk.groups){
        require(g.bone<m.size()&&g.bone<21,"Hit volumes: group bone outside the 21 query matrices");
        require(!g.weights.empty()&&g.first_vertex<=n&&g.weights.size()<=n-g.first_vertex,"Hit volumes: group range outside the chunk");
    }
    require(chunk.groups[0].first_vertex==0,"Hit volumes: the query program starts the first group at vertex 0");
    auto position=[&](std::size_t i){
        const auto& p=chunk.vertices[i].position;
        return RootVector{p.x,p.y,p.z,1.0F};
    };
    std::vector<RootVector> out(n);
    const auto& g0=chunk.groups[0];
    const auto& b0=m[g0.bone];
    if(chunk.groups.size()==1){
        const auto count=g0.weights.size();
        for(std::size_t i=0;i<count;++i)out[i]=combine(b0,position(i));
        for(std::size_t i=count;i<n;++i){   // the unpacked second vector, row 0x47000000 added
            const auto& v=chunk.vertices[i];
            out[i]={32896.0F+v.uv[0],32896.0F+v.uv[1],32896.0F+v.normal.x,32896.0F+v.normal.y};
        }
        return out;
    }
    for(std::size_t i=0;i<g0.weights.size();++i){
        const auto t=combine(b0,position(i));
        const float w=g0.weights[i];
        out[i]={t[0]*w,t[1]*w,t[2]*w,t[3]*w};
    }
    for(std::size_t i=g0.weights.size();i<n;++i)
        out[i]={b0[3][0]*0.0F,b0[3][1]*0.0F,b0[3][2]*0.0F,b0[3][3]*0.0F};
    for(std::size_t g=1;g<chunk.groups.size();++g){
        const auto& group=chunk.groups[g];
        for(std::size_t j=0;j<group.weights.size();++j){
            const auto i=group.first_vertex+j;
            const auto t=combine(m[group.bone],position(i));
            const float w=group.weights[j];
            for(std::size_t k=0;k<4;++k)out[i][k]=t[k]*w+out[i][k];
        }
    }
    return out;
}

void age_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore){
    if(gore.level_2c08==0)return;
    require(gore.parts.size()==parts.size(),"Hit volumes: gore arrays differ from the part count");
    for(std::size_t i=0;i<parts.size();++i){
        if(!gore.parts[i])continue;
        require(parts[i].lod0!=nullptr,"Hit volumes: gore array on a part without a LOD0 mesh");
        auto& bytes=*gore.parts[i];
        const auto& mesh=*parts[i].lod0;
        require(bytes.size()>=32*mesh.chunks.size(),"Hit volumes: gore array shorter than its LOD0 chunks");
        for(std::size_t c=0;c<mesh.chunks.size();++c)
            for(std::size_t v=0;v<mesh.chunks[c].vertices.size();++v){
                auto& b=bytes[32*c+v];
                if(b==0)continue;
                const std::uint32_t sum=std::uint32_t(b)+gore.level_2c08;
                b=static_cast<std::uint8_t>(sum<0x5bU?sum:0x5aU);
            }
    }
    gore.level_2c08=1;
}

void saturate_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore){
    require(gore.parts.size()==parts.size(),"Hit volumes: gore arrays differ from the part count");
    for(std::size_t i=0;i<parts.size();++i){
        if(!gore.parts[i])continue;                                              // +2340 + 4 part
        require(parts[i].lod0!=nullptr,"Hit volumes: gore array on a part without a LOD0 mesh");
        auto& bytes=*gore.parts[i];
        const auto& mesh=*parts[i].lod0;                                         // +1ec8, chunk count +1ec7
        require(bytes.size()>=32*mesh.chunks.size(),"Hit volumes: gore array shorter than its LOD0 chunks");
        for(std::size_t c=0;c<mesh.chunks.size();++c)
            for(std::size_t v=0;v<mesh.chunks[c].vertices.size();++v){         // chunk header +1
                auto& b=bytes[32*c+v];
                if(b!=0)b=0x5a;
            }
    }
}

bool update_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore,std::int8_t draw_mode_2bda,std::uint32_t frame){
    if(static_cast<std::int32_t>(gore.level_2c08)<=0)return false;             // blez +2c08
    if(draw_mode_2bda==0){
        saturate_hit_gore(parts,gore);
        gore.level_2c08=0xffffffffU;
        return true;
    }
    if((frame&1U)!=0)return false;                                              // s1 = *005383c8 & 1
    gore.level_2c08+=1;
    if(static_cast<std::int32_t>(gore.level_2c08)<0x5a)return false;           // slti 0x5a
    gore.level_2c08=0xffffffffU;
    saturate_hit_gore(parts,gore);
    return true;
}

std::int32_t character_gore_level(std::uint32_t level_2c08){
    return std::max(static_cast<std::int32_t>(level_2c08),0);
}

CharacterGoreSubtract character_gore_subtract(std::uint8_t byte,std::int32_t level){
    if((byte&0x7fU)==0)return {};
    require(level>=-1,"Character gore: 123.y below -1.0");                    // 0.0.. or the a3 form's -1.0
    const std::int32_t m=std::min<std::int32_t>(level+byte,90);
    return {static_cast<std::uint8_t>((m/4+1)/2),static_cast<std::uint8_t>(m)};
}

std::vector<CharacterGoreSubtract> character_gore_vertices(const SkinnedMesh& lod0,std::span<const std::uint8_t> bytes,
    std::int32_t level,std::size_t vertex_count){
    require(bytes.size()>=32*lod0.chunks.size(),"Character gore: gore array shorter than its LOD0 chunks");
    std::vector<CharacterGoreSubtract> out;out.reserve(vertex_count);
    for(std::size_t c=0;c<lod0.chunks.size();++c){
        const auto& chunk=lod0.chunks[c];
        require(chunk.vertices.size()<=32,"Character gore: chunk with more than 32 vertices");
        const bool flare=chunk.microprogram_entry==12||chunk.microprogram_entry==14;
        for(std::size_t k=0;k<chunk.vertices.size();++k){
            const std::uint8_t b=bytes[32*c+k];
            if(flare&&b!=0)throw std::runtime_error("0x3058: a gore byte on a flare chunk (character program 0x518 / 0x570) is not hosted");
            out.push_back(character_gore_subtract(b,level));
        }
    }
    require(out.size()==vertex_count,"Character gore: LOD0 chunk vertices differ from the drawn geometry");
    return out;
}

std::int32_t query_hit_volumes(std::span<const HitVolumePart> parts,HitVolumeGore& gore,
    std::span<const ShotFrame> matrices,std::int32_t mode,HitVolumeRecord& r){
    require(matrices.size()>=21,"Hit volumes: 21 query matrices are required");
    require(gore.parts.size()==parts.size(),"Hit volumes: gore arrays differ from the part count");
    require(parts.size()<=0xffff,"Hit volumes: part count above +23c0");
    r.near_score=fb(0x4b189680U);    // 1e7
    r.far_score=fb(0xcb189680U);     // -1e7
    const float r2=r.radius*r.radius;
    bool any=false,gored=false;
    QueryVu0 vu;
    for(std::size_t i=parts.size();i-->0;){
        const auto& part=parts[i];
        const auto code=static_cast<std::int32_t>(part.flags1&0x1cU);
        bool gore_mode=mode==2||(code<0xc&&mode==1);
        if(part.flags0&0x80U)gore_mode=false;
        if(part.lod1==nullptr||code==0x1c)continue;
        for(std::size_t c=0;c<part.lod1->chunks.size();++c){
            const auto positions=run_chunk(vu,part.lod1->chunks[c],matrices);
            bool chunk_hit=false;
            for(std::size_t v=0;v<positions.size();++v){
                const auto& p=positions[v];
                const float d=sq_distance(p);
                if(!(d<r2))continue;
                any=true;chunk_hit=true;
                update_near_far(r,0.25F*p[1]+std::fabs(d),p[1],code,v,c,i);   // abs.s of the squared distance
                if(gore_mode)break;
            }
            if(chunk_hit&&gore_mode){
                // 0038c350: the next chunk was sent (and run) before this one was scanned.
                if(c+1<part.lod1->chunks.size())(void)run_chunk(vu,part.lod1->chunks[c+1],matrices);
                if(!gore.parts[i]){
                    require(part.lod0!=nullptr,"Hit volumes: gore pass on a part without a LOD0 mesh");
                    gore.parts[i]=std::vector<std::uint8_t>(32*part.lod0->chunks.size(),0);   // man.cc, zeroed
                }
                require(part.lod0!=nullptr,"Hit volumes: gore pass on a part without a LOD0 mesh");
                require(gore.parts[i]->size()>=32*part.lod0->chunks.size(),"Hit volumes: gore array shorter than its LOD0 chunks");
                gored=true;
                gore_pass(part,i,*gore.parts[i],matrices,r,vu);
                break;
            }
        }
    }
    if(!any)return 0;
    hit_point(parts,matrices,r,vu);
    if(gored)gore.level_2c08=1;
    require(r.code.has_value(),"Hit volumes: record +28 read before it was written");
    return *r.code;
}

std::uint32_t actor_hit_volumes(const HitVolumeActor& a,const RootVector& origin,const RootVector& direction,
    std::int32_t mode,HitVolumeRecord& record,std::vector<HitVolumeEffect>& effects){
    require(a.gore!=nullptr&&a.gore_2df2!=nullptr,"Hit volumes: actor gore state missing");
    const auto frame=hit_ray_frame(origin,direction);
    const auto matrices=hit_node_matrices(a.nodes,a.inverse_bind,a.component,frame.inverse);
    age_hit_gore(a.parts,*a.gore);
    const auto code=query_hit_volumes(a.parts,*a.gore,matrices,mode,record);
    if(code==0)return 0;
    std::uint32_t flags=0x100;
    std::uint32_t entry=1;
    if(code==0xc){entry=2;flags|=0x80;}
    if(code==0x10){entry=3;flags|=0x40;}
    if(code==4)flags|=0x40;
    if(code==0x14)flags|=0x20;
    if(mode<=0)return flags;
    const auto& d=frame.direction;
    effects.push_back({entry,combine(frame.frame,RootVector{0.0F,record.near_y,0.0F,1.0F}),
                       RootVector{0.0F-d[0],0.0F-d[1],0.0F-d[2],0.0F-d[3]}});
    bool exit=mode==2;
    if(!exit){
        require(record.far_code.has_value(),"Hit volumes: record +3c read before it was written");
        exit=*record.far_code<0xc&&code<0xc;
    }
    if(exit){
        effects.push_back({0,combine(frame.frame,RootVector{0.0F,record.far_y,0.0F,1.0F}),d});
        if(a.life_state!=2&&a.life_state!=3&&*a.gore_2df2==0)*a.gore_2df2=1;
    }
    return flags;
}

BulletHitVolumes bullet_hit_volumes(const HitVolumeActor& actor,const RootVector& origin,const RootVector& direction,
    std::int32_t mode,float radius,std::vector<HitVolumeEffect>& effects){
    HitVolumeRecord record;
    record.radius=radius;
    BulletHitVolumes out;
    out.flags=actor_hit_volumes(actor,origin,direction,mode,record,effects);
    if(out.flags==0)return out;
    require(record.group_count.has_value(),"Hit volumes: record +48 read before it was written");
    out.distance=record.near_y;
    out.nodes.assign(record.groups.begin(),record.groups.begin()+*record.group_count);
    return out;
}

}
