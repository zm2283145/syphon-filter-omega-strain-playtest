#include "skybox.hpp"
#include "vu_float.hpp"
#include "game_libm.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Skybox: "+what);}
// The EE FPU / VU0 hold no infinities or denormals: a nonfinite result throws, a denormal flushes to zero.
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float mul(float a,float b){return check(a*b);}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
using Vec4=std::array<float,4>;
Vec4 scale(const Vec4& v,float s){return {mul(v[0],s),mul(v[1],s),mul(v[2],s),mul(v[3],s)};}
// vmul.xyz, then the ACC sum x^2 + y^2 + z^2 in that order (vmulax / vmadday / vmaddz w), then vsqrt.
float length3(const Vec4& v){
    const float xx=mul(v[0],v[0]),yy=mul(v[1],v[1]),zz=mul(v[2],v[2]);
    return check(vu::sqrt(add(add(xx,yy),zz)));
}
constexpr float sixtieth=0x1.111112p-6F;   // 0x3c888889

struct Reader{
    std::span<const std::uint8_t> bytes;std::size_t pos{};
    void need(std::size_t n) const{if(pos+n>bytes.size())fail("SKY: record past the section end");}
    std::uint32_t word(){need(4);std::uint32_t v;std::memcpy(&v,bytes.data()+pos,4);pos+=4;return v;}
    float real(){const auto w=word();float f;std::memcpy(&f,&w,4);return f;}
    void align(){pos=(pos+3)&~std::size_t{3};}
};
}

std::vector<SkyRecord> decode_sky_records(std::span<const std::uint8_t> body,std::uint32_t count){
    Reader r{body};
    std::vector<SkyRecord> out;
    for(std::uint32_t i=0;i<count;++i){
        SkyRecord s;
        s.group=r.word();s.id=r.word();
        for(;;){r.need(1);const char c=static_cast<char>(r.bytes[r.pos++]);if(!c)break;s.name.push_back(c);}
        r.align();
        s.f1=r.real();s.f0=r.real();s.f2=r.real();
        r.need(1);s.byte=r.bytes[r.pos++];
        r.align();
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<SkyDefinition> build_sky_definitions(const std::vector<SkyRecord>& records){
    std::vector<SkyDefinition> skies;
    for(std::size_t i=0;i<records.size();){
        const auto group=records[i].group;
        SkyDefinition sky;
        std::uint32_t id=0;
        // 001c5e50: consume records while the next one's group matches the first one's (the loader passes the
        // records left, so a run ends at the section end too).
        do{
            const auto& rec=records[i++];
            id=rec.id;
            if(sky.layers.size()==8)fail("001c5e50: a sky of more than 8 layers overflows the constructor's stack");
            SkyLayerParams p;
            p.name=rec.name;
            p.a={rec.f1,rec.f0,0.0F,0.0F};
            p.b={0.0F,0.0F,rec.f2,0.0F};
            p.flags=rec.byte?0x10U:4U;
            p.flags|=1U;
            if(!(rec.f2==0.0F)&&!rec.byte)p.flags|=8U;
            sky.layers.push_back(std::move(p));
        }while(i<records.size()&&records[i].group==group);
        sky.key=id|(group<<16);
        skies.push_back(std::move(sky));
    }
    return skies;
}

std::vector<SkyLayer> setup_sky_layers(const SkyDefinition& sky,const std::vector<SkyModelBox>& boxes){
    if(boxes.size()!=sky.layers.size())fail("003bbdc0: one model box per layer");
    std::vector<SkyLayer> layers(sky.layers.size());
    std::optional<std::size_t> boxed;
    for(std::size_t i=0;i<layers.size();++i){
        layers[i].flags=sky.layers[i].flags;
        if(sky.layers[i].flags&0x10U)boxed=i;
    }
    if(!boxed)fail("003bbdc0: no layer flagged 0x10 (the EE reads the box through a null layer)");
    const auto& box=boxes[*boxed];
    const Vec4 d{sub(box.maximum[0],box.minimum[0]),sub(box.maximum[1],box.minimum[1]),
                 sub(box.maximum[2],box.minimum[2]),sub(box.maximum[3],box.minimum[3])};
    const Vec4 e=scale(d,0.5F);
    const Vec4 hi{e[0],e[1],e[2],0.0F},lo{-e[0],-e[1],-e[2],0.0F};
    for(std::size_t i=0;i<layers.size();++i){
        auto& layer=layers[i];
        const auto& p=sky.layers[i];
        const Vec4 span{sub(hi[0],lo[0]),sub(hi[1],lo[1]),sub(hi[2],lo[2]),sub(hi[3],lo[3])};
        layer.extent=mul(0.5F,length3(span));
        layer.rotation_step=(layer.flags&8U)?p.b:scale(p.b,sixtieth);
        const Vec4 t=scale(p.a,sixtieth);
        layer.speed=length3(t);
        layer.direction=t;
        if(!(layer.speed==0.0F))layer.direction=scale(layer.direction,check(vu::div(1.0F,layer.speed)));
    }
    return layers;
}

void update_sky_layer(SkyLayer& layer){
    layer.offset=add(layer.offset,layer.speed);
    if(!(layer.flags&8U))
        for(std::size_t k=0;k<4;++k)layer.rotation[k]=add(layer.rotation[k],layer.rotation_step[k]);
    if(layer.flags&4U){
        if(!(layer.offset<=layer.extent))layer.offset=-layer.extent;
        if(layer.offset<-layer.extent)layer.offset=layer.extent;
    }
    layer.translation=scale(layer.direction,layer.offset);
    layer.translation[3]=1.0F;
}

SkyLayerDraw sky_layer_draw(SkyLayer& layer,const std::array<float,4>& view_row_1){
    SkyLayerDraw out;
    Vec4 colour{};
    if(layer.flags&0x10U)out.colour_mode=false;
    else{
        out.colour_mode=true;
        if(layer.extent==0.0F)fail("003bc8a8: fade divided by a zero extent");
        float fade=check(vu::div(std::fabs(layer.offset),layer.extent));
        if(!(fade<1.0F))fade=1.0F;
        colour[3]=-fade;
    }
    out.colour=scale(colour,128.0F);
    if(layer.flags&8U){
        // 003bc940..003bca44 (f0 = a, f4 = +1d8, f3 = the new side, f2 = pi + one of them).
        const float a=check(game_atan2f(view_row_1[1],view_row_1[0]));
        if(!(layer.follow_yaw==a)){
            const float half_pi=std::bit_cast<float>(0x3fc90fdbU),pi=std::bit_cast<float>(0x40490fdbU);
            float last=layer.follow_yaw,now=a;
            if(last< -half_pi&&!(now<=half_pi)){           // 003bc990..003bc9d4
                const float p=add(pi,last);
                last=mul(last,-1.0F);
                now=sub(now,p);
            }else if(now< -half_pi&&!(last<=half_pi)){     // 003bc9d8..003bca24
                const float p=add(pi,now);
                now=mul(now,-1.0F);
                last=sub(last,p);
            }
            layer.rotation[2]=add(layer.rotation[2],mul(layer.rotation_step[2],sub(now,last)));   // 003bca28..003bca3c
            layer.follow_yaw=a;                                                                   // 003bca40
        }
    }
    NiMatrix m{};
    for(std::size_t k=0;k<4;++k)m[k][k]=1.0F;
    ni_rotate_rows(m,ni_axis_angle(1.0F,0.0F,0.0F,layer.rotation[0]));
    ni_rotate_rows(m,ni_axis_angle(0.0F,1.0F,0.0F,layer.rotation[1]));
    ni_rotate_rows(m,ni_axis_angle(0.0F,0.0F,1.0F,layer.rotation[2]));
    m[3]=layer.translation;
    out.matrix=m;
    return out;
}

std::array<float,4> sky_vertex_colour(const std::array<float,4>& vertex,const std::array<float,4>& colour){
    std::array<float,4> out{};
    for(std::size_t k=0;k<4;++k){
        const float v=std::trunc(add(std::round(mul(vertex[k],128.0F)),colour[k]));
        if(v<0.0F)fail("VU1 set 0 pair 0x1b2: a negative faded colour lane (the GS keeps its low 8 bits)");
        out[k]=v/128.0F;
    }
    return out;
}

}  // namespace omega
