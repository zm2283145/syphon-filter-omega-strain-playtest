#include "terrain_point_lights.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using V=ShadowVector;
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string("terrain point lights: ")+what);}

// EE side. The bounds path (001832d0, 00183380, 00183350, 00373590) is VU0
// macro code (vadd / vmulx / vsub / vmul / ACC sums / vsqrt / vmulw): it
// saturates like the VU1 side below; the radius only passes through COP1
// registers ($f0 / $f12) without arithmetic. Only 00371cf0's 128 * colour is
// COP1 (mul.s): binary32, round to nearest, denormal results flushed to zero,
// non-finite results throw.
// Bit tests (MSVC's std::fpclassify / isnan / isinf are out-of-line calls).
std::uint32_t bits(float v){return std::bit_cast<std::uint32_t>(v);}
bool exponent_zero(float v){return (bits(v)&0x7f800000U)==0U;}
bool exponent_max(float v){return (bits(v)&0x7f800000U)==0x7f800000U;}
bool subnormal(float v){return exponent_zero(v)&&(bits(v)&0x7fffffU)!=0U;}
float ftz(float v){return subnormal(v)?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){require(!exponent_max(v),"value not finite");return ftz(v);}
float cop1_mul(float a,float b){return check(a*b);}

// VU0 macro / VU1: binary32, round to nearest, overflow saturated to
// +-FLT_MAX and denormal results flushed to zero. `overflow` collects the O
// flag.
struct Vu {
    bool overflow{false};
    float r(float v){
        if(exponent_max(v)){
            require((bits(v)&0x7fffffU)==0U,"VU value not a number");
            overflow=true;return std::copysign(std::numeric_limits<float>::max(),v);
        }
        return ftz(v);
    }
    float add(float a,float b){return r(a+b);}
    float sub(float a,float b){return r(a-b);}
    float mul(float a,float b){return r(a*b);}
    // (x*x + y*y) + z*z: the vmul then the ACC sum (vmulax / vmadday / vmaddz).
    float sum_squares(float x,float y,float z){return add(add(mul(x,x),mul(y,y)),mul(z,z));}
    // EFU eleng: sqrt((x*x + y*y) + z*z) (precision not established).
    float eleng(float x,float y,float z){return r(vu::sqrt(sum_squares(x,y,z)));}
};

// Light vcall +0x34 (level_lights.cpp, 00373a30's range).
float light_range(const LevelLights& lights,const LevelLight& l){
    if(l.vtable==level_light_vtable_flashlight)return lights.flash_dazzle_size;         // 0045bc00 (00497a20)
    require(l.vtable==level_light_vtable_base||l.vtable==level_light_vtable_spot||l.vtable==level_light_vtable_directional||
            l.vtable==level_light_vtable_point||l.vtable==level_light_vtable_ambient,"listed light with an unsupported range method");
    return l.outer;                                                                     // 001479f0
}

struct Candidate { LevelLightRef light; float key{}; };
}

std::vector<TerrainPointLight> terrain_point_lights(const LevelLights& lights,const LevelLightTable& table,
                                                    const ShadowVector& minimum,const ShadowVector& maximum,std::uint32_t mask){
    require(mask==terrain_point_light_mask,"only the terrain mask 4 is hosted");
    require(table.rows.size()==table.set.table.size(),"light table rows and entries differ");
    const std::size_t n=table.set.table.size();
    require(n<=256,"light table larger than the scratchpad copy (256 entries)");
    for(std::size_t i=0;i<4;++i)require(std::isfinite(minimum[i])&&std::isfinite(maximum[i]),"bounds not finite");
    Vu vu0;
    // 001832d0: vadd.xyzw, then vmulx.xyzw by 0.5.
    V centre{};
    for(std::size_t i=0;i<4;++i)centre[i]=vu0.mul(vu0.add(minimum[i],maximum[i]),0.5F);
    // 00183380 (vsub maximum - centre), 00183350 (vmul, ACC sum, vsqrt).
    const float radius=vu0.r(vu::sqrt(vu0.sum_squares(vu0.sub(maximum[0],centre[0]),vu0.sub(maximum[1],centre[1]),vu0.sub(maximum[2],centre[2]))));
    std::vector<Candidate> candidates;
    for(std::size_t k=0;k<n;++k){
        const ShadowLightTableEntry& e=table.set.table[k];
        if((mask&e.flags)==0)continue;
        if(e.flags==2U){candidates.push_back({table.rows[k].light,0.0F});continue;}     // 00373590: directional
        require(std::isfinite(e.range)&&std::isfinite(e.weight)&&std::isfinite(e.position[0])&&std::isfinite(e.position[1])
                &&std::isfinite(e.position[2]),"light table entry not finite");
        // vsub, vmul.xyz and the ACC sum (vf10); vaddz.x radius + range (vf7), vmul.x (vf8).
        const float d2=vu0.sum_squares(vu0.sub(e.position[0],centre[0]),vu0.sub(e.position[1],centre[1]),vu0.sub(e.position[2],centre[2]));
        const float reach=vu0.add(radius,e.range);
        const float reach2=vu0.mul(reach,reach);
        // vsub.x of d2 - reach^2; the S flag (cfc2 vi16 bit 1) of that result.
        const float raw=d2-reach2;
        require(!subnormal(raw),"denormal bounds difference (VU sign flag not established)");
        if(!(vu0.r(raw)<0.0F))continue;
        candidates.push_back({table.rows[k].light,vu0.mul(d2,e.weight)});              // vmulw.x
    }
    if(!candidates.empty()){                                                            // 001273c8 with 00373ba0
        std::vector<float> keys;
        keys.reserve(candidates.size());
        for(const Candidate& c:candidates)keys.push_back(c.key);
        const std::vector<std::size_t> order=shadow_light_order(keys);
        std::vector<Candidate> sorted;
        sorted.reserve(order.size());
        for(std::size_t i:order)sorted.push_back(candidates[i]);
        candidates=std::move(sorted);
    }
    std::vector<TerrainPointLight> list;
    for(const Candidate& c:candidates){                                                 // 00372548
        const LevelLight& l=lights.light(c.light);
        if((mask&l.type)==0)continue;                                                   // light +0x1e
        if(list.size()>=6)continue;                                                     // +0x2130 < 6
        TerrainPointLight r;                                                            // 00371cf0
        for(std::size_t i=0;i<3;++i){
            require(std::isfinite(l.position[i])&&std::isfinite(l.colour[i]),"light not finite");
            r.a[i]=l.position[i];
            r.b[i]=cop1_mul(128.0F,l.colour[i]);                                         // mul.s
        }
        r.a[3]=light_range(lights,l);
        r.b[3]=l.inner;
        require(std::isfinite(r.a[3])&&std::isfinite(r.b[3]),"light range not finite");
        list.push_back(r);
    }
    return list;
}

namespace {
struct BatchSphere { float centre[3]; float radius; };
BatchSphere batch_sphere(Vu& vu,const MeshBatch& batch){
    BatchSphere s{};
    for(std::size_t i=0;i<3;++i)                                                         // mulai, maddai (I = 0.5), maddw + 0
        s.centre[i]=vu.add(vu.add(vu.mul(batch.header_minimum[i],0.5F),vu.mul(batch.header_maximum[i],0.5F)),0.0F);
    s.radius=vu.eleng(vu.sub(s.centre[0],batch.header_maximum[0]),vu.sub(s.centre[1],batch.header_maximum[1]),
                      vu.sub(s.centre[2],batch.header_maximum[2]));                     // msubw, eleng
    return s;
}
bool reached(Vu& vu,const TerrainPointLight& light,const BatchSphere& s){
    const float reach=vu.add(light.a[3],s.radius);                                       // add.w
    const float dist=vu.eleng(vu.sub(light.a[0],s.centre[0]),vu.sub(light.a[1],s.centre[1]),vu.sub(light.a[2],s.centre[2]));
    return !std::signbit(vu.sub(reach,dist));                                            // sub.w, fmand 0x10
}
}

bool terrain_batch_reached(const TerrainPointLight& light,const MeshBatch& batch){
    Vu vu;
    return reached(vu,light,batch_sphere(vu,batch));
}

namespace {bool fast_reject=false;}
void set_terrain_light_fast_reject(bool on){fast_reject=on;}
bool terrain_light_fast_reject(){return fast_reject;}

void apply_terrain_point_lights(std::span<const TerrainPointLight> lights,const MeshBatch& batch,
                                std::span<const std::array<float,3>> positions,std::span<std::array<std::uint8_t,3>> colours){
    require(positions.size()==batch.vertex_count&&colours.size()==batch.vertex_count,"vertex spans differ from the batch");
    Vu vu;
    const BatchSphere s=batch_sphere(vu,batch);
    for(const TerrainPointLight& light:lights){
        if(!reached(vu,light,s))continue;
        const float span=vu.sub(light.a[3],light.b[3]);                                  // sub.w
        // div q: a zero divisor gives +-FLT_MAX (sign 1 xor span), as the EE interpreter returns (vu_float.hpp);
        // the vision light 0025fbb0 starts at range 0 = inner 0 while its fade +2c is 0.
        const float q=span==0.0F?std::copysign(std::numeric_limits<float>::max(),span):vu.r(vu::div(1.0F,span));
        // Port-side speed only (132vt2d): a vertex at least `beyond` from the light (double distance) gets a <= 0
        // on the VU path, so it is skipped without it. The VU path's error (the rounded differences, squares,
        // sums, eleng, the subtraction, q and the product) is a few binary32 ulps of |dl| + |B.w|; the margin
        // here is 1e-4 of the operands, far above it. Only for a positive span (a falls as the distance grows).
        double beyond2=std::numeric_limits<double>::infinity();
        if(fast_reject&&span>0.0F){
            const double range=light.a[3],inner=light.b[3];
            const double beyond=(range+1e-4*(std::fabs(range)+std::fabs(inner))+1e-30)/(1.0-1e-4);
            if(beyond>0.0)beyond2=beyond*beyond;
        }
        for(std::size_t v=0;v<positions.size();++v){
            const auto& p=positions[v];
            const double ex=double(light.a[0])-p[0],ey=double(light.a[1])-p[1],ez=double(light.a[2])-p[2];
            if(ex*ex+ey*ey+ez*ez>=beyond2)continue;
            const float dl=vu.eleng(vu.sub(light.a[0],p[0]),vu.sub(light.a[1],p[1]),vu.sub(light.a[2],p[2]));
            Vu w;                                                                         // the w MAC flags of msubq
            const float raw=1.0F-(vu.sub(dl,light.b[3])*q);
            const float a=w.r(raw);
            if(w.overflow||a<=0.0F)continue;                                              // fmand 0x1111 (Z, S, U, O)
            const float f=std::max(std::min(a,1.0F),0.0F);                                // miniw, maxx
            for(std::size_t i=0;i<3;++i){
                const float c=static_cast<float>(colours[v][i]);                          // itof0 of the byte & 0xff
                const float lit=std::min(vu.add(c,vu.mul(light.b[i],f)),255.0F);          // adda, maddw, minii
                const double t=std::trunc(static_cast<double>(lit));                      // ftoi0
                const std::int64_t word=static_cast<std::int64_t>(std::clamp(t,-2147483648.0,2147483647.0));
                colours[v][i]=static_cast<std::uint8_t>(word&0xff);
            }
        }
    }
}
}
