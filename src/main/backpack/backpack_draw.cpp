#include "backpack_draw.hpp"
#include "game_libm.hpp"
#include "man_draw_gate.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& where,const std::string& message){throw std::runtime_error(where+": "+message);}

bool active(const std::array<std::uint8_t,3>& root){return root[1]!=0&&root[0]!=0&&root[2]==0;}

float word_float(const BackpackTransform& t,std::size_t o){return std::bit_cast<float>(t.word(o));}

std::uint8_t model_byte(const GunModel& m,std::size_t o){
    if(!m.defined.at(o))fail("003836b0","gear model byte +"+std::to_string(o)+" undefined");
    return m.bytes[o];
}

// The constant the stun angle is scaled by (bits 0x3c8efa35: pi / 180).
const float degrees=std::bit_cast<float>(0x3c8efa35U);

}

std::optional<BackpackDrawCall> backpack_draw_gate_00253fe0(const WeaponBackpack& bp,BackpackTransform& transform,
    std::uint32_t code_38,const BackpackDrawView& view){
    constexpr const char* where="00253fe0";
    if(bp.word(0x2f8)==0)return std::nullopt;
    if(!view.region_root)fail(where,"the root bytes of the region +2f8 are not supplied");
    if(!active(*view.region_root))return std::nullopt;
    if(bp.word(0x300)!=0){
        if(!view.owner)fail(where,"the owner +300's view is not supplied");
        const bool valid=static_cast<std::uint32_t>(view.owner->key_c)==bp.word(0x304);
        if(valid&&active(view.owner->root))return std::nullopt;
    }
    const std::int32_t armor=bp.word(0x2e4)!=0?bp.value(0x2e8):0;
    bool empty=!(armor>0);
    for(std::size_t k=0;empty&&k<3;++k)
        if(bp.word(WeaponBackpack::slot(k)+0xcc)!=0)empty=false;   // +130 + k x d4
    if(empty)return std::nullopt;
    BackpackDrawCall call;
    const float age=std::bit_cast<float>(bp.word(0x314));
    if(!std::isfinite(age))fail(where,"a non-finite age +314");
    if(age<=0.0F)call.fade_416=-1;   // c.ole.s
    else{
        const float lifetime=std::bit_cast<float>(backpack_object_lifetime);   // *0048af18
        const float x=std::bit_cast<float>(0x42fe0000U)*((lifetime-age)/lifetime);
        if(!(x>-2147483648.0F&&x<2147483648.0F))fail(where,"the fade outside cvt.w.s's range");
        call.fade_416=static_cast<std::int8_t>(static_cast<std::uint8_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(x))&0xffU));
    }
    // Virtual +e0 = 003cd420: the transform refreshed (its virtual +58 = 00132900 clears +50), then its +40.
    transform.set_byte(0x50,0);
    for(std::size_t k=0;k<4;++k)call.light_reference[k]=word_float(transform,0x40+4*k);
    for(std::size_t r=0;r<4;++r)
        for(std::size_t c=0;c<4;++c)call.matrix[r][c]=word_float(transform,0x10+16*r+4*c);
    call.code=code_38;
    return call;
}

std::int32_t gear_draw_lod(const RootVector& t){
    const float d=std::sqrt((t[0]*t[0]+t[1]*t[1])+t[2]*t[2]);
    if(!std::isfinite(d))fail("003836b0","a non-finite view distance");
    const float q=d/std::bit_cast<float>(gear_draw_lod_distance);
    if(q>=2147483648.0F)return 1;
    auto lod=static_cast<std::int32_t>(q);   // cvt.w.s
    if(lod>=2)lod=1;
    return lod;
}

std::optional<GearDraw> gear_draw_003836b0(const GunModel& model,std::int8_t fade_416,std::uint32_t code,
    const ShotFrame& matrix,const RootVector& view_translation,std::uint32_t frame){
    constexpr const char* where="003836b0";
    if(model_byte(model,0x417)!=0)return std::nullopt;
    GearDraw d;
    if(fade_416>=0){
        d.alpha_5382c8={1,5,fade_416,3};
        d.vertex_alpha=static_cast<std::uint8_t>(fade_416+1);
    }else{
        d.alpha_5382c8={1,5,0x80,3};
        d.vertex_alpha=0x80;
    }
    d.flags=man_draw_flags_0037c350(0,code,0x480000);
    if(d.flags&0x80000000U)return std::nullopt;
    d.lod=gear_draw_lod(view_translation);
    if(d.flags&0x80U)fail(where,"flags with 0x80 (the depth-faded alpha 00383984..) are not ported");
    // The local matrices (scratchpad slots +40 + 0x70k): slot 0 identity; slot k a copy of its
    // parent's with identity rows times the bone (vmulax / vmadday / vmaddz) added to its origin.
    GunSlotMatrices local{};
    const ShotFrame identity{{{1.0F,0.0F,0.0F,0.0F},{0.0F,1.0F,0.0F,0.0F},{0.0F,0.0F,1.0F,0.0F},{0.0F,0.0F,0.0F,1.0F}}};
    local[0]=identity;
    for(std::size_t k=1;k<11;++k){
        const auto bone=0x150+32*k;
        const auto parent=static_cast<std::int32_t>(model.word(bone+12));
        if(parent<0||static_cast<std::size_t>(parent)>=k)
            fail(where,"gear bone "+std::to_string(k)+" parent "+std::to_string(parent)+" not below it (the original reads a stale slot)");
        const float bx=model.real(bone),by=model.real(bone+4),bz=model.real(bone+8);
        RootVector v{};
        for(std::size_t c=0;c<3;++c)v[c]=(identity[0][c]*bx+identity[1][c]*by)+identity[2][c]*bz;
        local[k]=local[static_cast<std::size_t>(parent)];
        for(std::size_t c=0;c<3;++c)local[k][3][c]+=v[c];
    }
    if(model_byte(model,0x415)!=0){
        const float a=static_cast<float>(81*((frame>>1)&31U))*degrees;
        const float cs=game_cosf(a),sn=game_sinf(a);
        // 00140e20(cos, 0, sin, 0, 1, 0, -sin, 0, cos): rows (cos, 0, -sin), (0, 1, 0), (sin, 0, cos).
        const std::array<std::array<float,3>,3> r{{{cs,0.0F,-sn},{0.0F,1.0F,0.0F},{sn,0.0F,cs}}};
        ShotFrame& m=local[3];
        const ShotFrame in=m;
        for(std::size_t i=0;i<3;++i)
            for(std::size_t c=0;c<4;++c)m[i][c]=(in[0][c]*r[i][0]+in[1][c]*r[i][1])+in[2][c]*r[i][2];
    }
    // Each slot through the object's matrix (the EE's is 0038b120's camera-stack matrix).
    for(std::size_t k=0;k<11;++k)
        for(std::size_t j=0;j<4;++j)
            for(std::size_t c=0;c<4;++c){
                const auto& l=local[k][j];
                d.matrices[k][j][c]=((matrix[0][c]*l[0]+matrix[1][c]*l[1])+matrix[2][c]*l[2])+matrix[3][c]*l[3];
            }
    const std::size_t base=0x2c0+0x80*static_cast<std::size_t>(d.lod);
    const auto parts=model_byte(model,0x40c);
    if(parts>8)fail(where,"more than 8 parts (the texture ids +3c0 / +3d0 overlap)");
    const auto sb=[&](std::size_t o){return static_cast<std::int32_t>(static_cast<std::int8_t>(model_byte(model,o)));};
    for(std::size_t p=0;p<parts;++p){
        const auto s=static_cast<std::int32_t>(p);
        if(s==sb(0x410)||s==sb(0x411)||s==sb(0x412)||s==sb(0x414))continue;
        if(model.word(base+16*p)==0)continue;
        d.parts.push_back(p);
    }
    return d;
}

}
