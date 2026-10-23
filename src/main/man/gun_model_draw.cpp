#include "gun_model_draw.hpp"
#include "game_libm.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& message){throw std::runtime_error(message);}

std::uint8_t byte_at(const GunModel& m,std::size_t o,const char* where){
    if(!m.defined.at(o))fail(std::string(where)+": gun model byte +"+std::to_string(o)+" undefined");
    return m.bytes[o];
}
std::int8_t sbyte_at(const GunModel& m,std::size_t o,const char* where){return static_cast<std::int8_t>(byte_at(m,o,where));}
std::uint16_t half_at(const GunModel& m,std::size_t o,const char* where){
    return static_cast<std::uint16_t>(byte_at(m,o,where)|(byte_at(m,o+1,where)<<8));
}

// The constant 003870c0 scales the angle by (bits 0x3c8efa35: pi / 180).
const float degrees=std::bit_cast<float>(0x3c8efa35U);

// Rows 0..2 of `m` become (r0 * R[i].x + r1 * R[i].y) + r2 * R[i].z (all four lanes); row 3 stays.
void turn(ShotFrame& m,const std::array<std::array<float,3>,3>& r){
    ShotFrame out=m;
    for(std::size_t i=0;i<3;++i)
        for(std::size_t c=0;c<4;++c)out[i][c]=(m[0][c]*r[i][0]+m[1][c]*r[i][1])+m[2][c]*r[i][2];
    m=out;
}

bool flare_counter_lit(std::uint8_t v){return v!=0&&v<3;}

bool flare_lit(std::uint8_t point,const GunFlareCounters& counters){
    return (point==0&&flare_counter_lit(counters.hand[0]))||(point==1&&flare_counter_lit(counters.hand[1]));
}

std::size_t lod_base(std::int32_t lod,const char* where){
    if(lod!=0&&lod!=1)fail(std::string(where)+": LOD "+std::to_string(lod)+" outside 0..1 (the records +2c0 / +340)");
    return 0x2c0+0x80*static_cast<std::size_t>(lod);
}

// The bone section shared by 003870c0 (an attach point) and 003836b0 (the HUD preview).
GunSlotMatrices slot_matrices(const GunModel& model,const ShotFrame& point_world,bool flare,std::uint32_t frame,
    const char* where){
    GunSlotMatrices m{};
    m[0]=point_world;
    const ShotFrame& m0=m[0];
    for(std::size_t k=1;k<11;++k){
        const auto bone=0x150+32*k;
        const auto parent=static_cast<std::int32_t>(model.word(bone+12));
        if(parent<0||static_cast<std::size_t>(parent)>=k)
            fail(std::string(where)+": GUN bone "+std::to_string(k)+" parent "+std::to_string(parent)+" not below it (the original reads a stale matrix)");
        const float bx=model.real(bone),by=model.real(bone+4),bz=model.real(bone+8);
        RootVector v{};
        for(std::size_t c=0;c<3;++c)v[c]=(m0[0][c]*bx+m0[1][c]*by)+m0[2][c]*bz;
        m[k]=m[static_cast<std::size_t>(parent)];
        for(std::size_t c=0;c<3;++c)m[k][3][c]+=v[c];
    }
    if(flare){
        const float a=static_cast<float>(72*((frame>>1)&7U))*degrees;
        const float cs=game_cosf(a),sn=game_sinf(a);
        turn(m[3],{{{1.0F,0.0F,0.0F},{0.0F,cs,sn},{0.0F,-sn,cs}}});
    }
    if(byte_at(model,0x415,where)!=0){
        const float a=static_cast<float>(81*((frame>>1)&31U))*degrees;
        const float cs=game_cosf(a),sn=game_sinf(a);
        turn(m[3],{{{cs,0.0F,-sn},{0.0F,1.0F,0.0F},{sn,0.0F,cs}}});
    }
    return m;
}

}

void GunFlareCounters::fire(std::uint8_t hand_index){
    if(hand_index>1)fail("0013ff40: flash hand outside 0..1 (model +2d90 / +2d91)");
    hand[hand_index]=1;
}

void GunFlareCounters::age(){
    for(auto& v:hand){
        if(v==0)continue;
        ++v;
        if(v>=9)v=0;
    }
}

GunSlotMatrices gun_slot_matrices(const GunModel& model,const ShotFrame& point_world,std::uint8_t point,
    const GunFlareCounters& counters,std::uint32_t frame){
    constexpr const char* where="003870c0";
    if(point>7)fail(std::string(where)+": attach point outside 0..7");
    return slot_matrices(model,point_world,flare_lit(point,counters),frame,where);
}

GunSlotMatrices gun_preview_matrices(const GunModel& model,std::uint32_t frame){
    ShotFrame identity{};
    for(std::size_t i=0;i<4;++i)identity[i][i]=1.0F;
    return slot_matrices(model,identity,false,frame,"003836b0");
}

std::vector<std::size_t> gun_preview_parts(const GunModel& model){
    constexpr const char* where="003836b0";
    const auto parts=byte_at(model,0x40c,where);
    if(parts>8)fail(std::string(where)+": more than 8 parts (the records +2c0 and the texture ids +3c0 overlap)");
    const std::int32_t skip[]{sbyte_at(model,0x410,where),sbyte_at(model,0x411,where),sbyte_at(model,0x412,where),
        sbyte_at(model,0x414,where)};
    std::vector<std::size_t> out;
    for(std::size_t p=0;p<parts;++p){
        const auto s=static_cast<std::int32_t>(p);
        if(s==skip[0]||s==skip[1]||s==skip[2]||s==skip[3])continue;
        if(model.word(0x2c0+16*p)==0)continue;
        out.push_back(p);
    }
    return out;
}

bool gun_model_drawn(const GunModel& model,std::uint8_t hidden_2438){return hidden_2438==0&&byte_at(model,0x417,"003870c0")==0;}

std::vector<std::size_t> gun_drawn_parts(const GunModel& model,std::uint8_t point,const GunFlareCounters& counters,
    std::int32_t lod,bool a3,std::uint8_t point_byte){
    constexpr const char* where="003855d0";
    if(point>7)fail(std::string(where)+": attach point outside 0..7");
    const auto base=lod_base(lod,where);
    const auto parts=byte_at(model,0x40c,where);
    if(parts>8)fail(std::string(where)+": more than 8 parts (the texture ids +3c0 / +3d0 overlap)");
    const std::int32_t light=sbyte_at(model,0x412,where);
    const std::int32_t t8=point_byte!=0?-1:light;    // 00385738..00385748
    const bool t9=byte_at(model,0x415,where)!=0;
    const std::int32_t t7=(point==3||point==4)?-1:sbyte_at(model,0x410,where);
    std::int32_t t6=sbyte_at(model,point==0?0x414:0x413,where);
    if(point==6&&point_byte!=0)t6=sbyte_at(model,0x414,where);    // 00385780..00385798
    const std::int32_t t5=sbyte_at(model,0x411,where);
    std::vector<std::size_t> out;
    for(std::size_t p=0;p<parts;++p){
        if(model.word(base+16*p)==0)continue;
        const auto s=static_cast<std::int32_t>(p);
        if(s==t7||s==t8||s==t6)continue;
        if(s==t5){
            if(a3)continue;
            if(t9||(point==0&&flare_lit(0,counters)))out.push_back(p);
            if(t9||(point==1&&flare_lit(1,counters)))out.push_back(p);
            continue;
        }
        out.push_back(p);    }
    return out;
}

GunPartMesh gun_part_mesh(const GunModel& model,std::span<const std::uint8_t> hog,std::size_t part,std::int32_t lod){
    constexpr const char* where="003855d0";
    if(part>=8)fail(std::string(where)+": part outside 0..7");
    const auto rec=lod_base(lod,where)+16*part;
    const std::size_t t0=model.word(rec);
    const std::size_t qwc=half_at(model,rec+4,where);
    const std::size_t count=byte_at(model,rec+7,where);
    const std::size_t data=model.word(rec+8);
    if(t0==0||count==0)fail(std::string(where)+": the part has no chunks");
    if(t0<data||t0-data!=((2+2*count+15)&~std::size_t(15)))
        fail(std::string(where)+": record +0 is not the chunk start after the descriptors (not ported)");
    std::size_t span_end=t0;
    for(std::size_t a=0;a<count;++a){
        if(data+2+2*a>=hog.size())fail(std::string(where)+": descriptors run past the HOG");
        span_end+=std::size_t(hog[data+2+2*a])*16;
    }
    // The REF tag sends +4 qwords from +0: the chunks the descriptors give.
    if(span_end-t0!=qwc*16)fail(std::string(where)+": record +4 (the REF tag's QWC) differs from the chunk descriptors");
    if(span_end>hog.size())fail(std::string(where)+": chunks run past the HOG");
    GunPartMesh out;
    out.mesh=decode_skinned_mesh(hog.subspan(data,span_end-data),11);
    if(out.mesh.chunks.size()!=count)fail(std::string(where)+": decoded chunk count differs from the record");
    out.texture=static_cast<std::int16_t>(half_at(model,0x3c0+0x10*static_cast<std::size_t>(lod)+2*part,where));
    return out;
}

bool gun_part_skinnable(const SkinnedMesh& mesh){
    for(const auto& c:mesh.chunks){
        const auto e=c.microprogram_entry;
        if(e==12||e==14)continue;
        if(e!=8&&e!=10)return false;
        if(c.groups.size()!=1)return false;
    }
    return true;
}

bool gun_part_flare_program(const SkinnedMesh& mesh){
    if(mesh.chunks.empty())return false;
    for(const auto& c:mesh.chunks)if(c.microprogram_entry!=12&&c.microprogram_entry!=14)return false;
    return true;
}

SkinnedMeshGeometry gun_part_geometry(const SkinnedMesh& mesh,const GunSlotMatrices& matrices){
    SkinnedMeshGeometry output;
    gun_part_geometry(mesh,matrices,output);
    return output;
}
void gun_part_geometry(const SkinnedMesh& mesh,const GunSlotMatrices& matrices,SkinnedMeshGeometry& output){
    if(!gun_part_skinnable(mesh))fail("gun part: chunks outside the single-group entries 8 / 10 and the flare entries 12 / 14 (render programs not ported)");
    std::array<SkinPaletteBone,std::tuple_size_v<GunSlotMatrices>> palette{};
    for(std::size_t k=0;k<matrices.size();++k){
        SkinMatrix s{};
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)s[4*i+j]=matrices[k][i][j];
        palette[k]={s,s};
    }
    skin_skinned_mesh_into(mesh,palette,std::nullopt,output);
}

ShotFrame gun_holster_matrix(const GunModel& hand_model,const ShotFrame& holster_point_world){
    const ShotFrame& m=holster_point_world;
    const float gx=hand_model.real(0x150),gy=hand_model.real(0x154),gz=hand_model.real(0x158);
    ShotFrame out=m;
    for(std::size_t c=0;c<3;++c)out[3][c]+=(m[0][c]*gx+m[1][c]*gy)+m[2][c]*gz;
    return out;
}

std::optional<std::size_t> gun_holster_part(const GunModel& hand_model,std::int32_t lod){
    constexpr const char* where="0038598c";
    const std::int32_t p=sbyte_at(hand_model,0x410,where);
    if(p==-1)return std::nullopt;
    if(p<0||p>=8)fail(std::string(where)+": holster part "+std::to_string(p)+" outside 0..7 (the record and texture areas)");
    if(p>=byte_at(hand_model,0x40c,where))fail(std::string(where)+": holster part "+std::to_string(p)+" not below +40c (the original sends whatever record is there)");
    const auto rec=lod_base(lod,where)+16*static_cast<std::size_t>(p);
    const auto qwc=half_at(hand_model,rec+4,where);
    if(hand_model.word(rec)==0){
        if(qwc==0)return std::nullopt;
        fail(std::string(where)+": holster record word 0 with a QWC (a REF from address 0)");
    }
    return static_cast<std::size_t>(p);
}

bool gun_part_matrix0_only(const SkinnedMesh& mesh){
    for(const auto& c:mesh.chunks){
        if(c.microprogram_entry==12||c.microprogram_entry==14){
            if(c.groups.empty()||c.groups.front().bone!=0)return false;
            continue;
        }
        for(const auto& g:c.groups)if(g.bone!=0)return false;
    }
    return true;
}

std::uint8_t gun_thermal_class(const GunModel& model,std::size_t part){
    constexpr const char* where="0038a918";
    if(part>=byte_at(model,0x40c,where))fail(std::string(where)+": part outside +40c");
    return static_cast<std::int32_t>(part)==sbyte_at(model,0x411,where)?4:1;
}

}