#include "model_light_gather.hpp"
#include "flashlight.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using V=ShadowVector;
// Interpreter model: binary32, round to nearest, denormal results flushed to
// zero; compiled with -ffp-contract=off. Non-finite results throw (the EE
// clamps them instead; not modelled).
void require(bool ok,const std::string& what){if(!ok)throw std::runtime_error("model light gather: "+what);}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string("model light gather: ")+what);}   // 132vs1: no string unless it fails
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){require(std::isfinite(v),"value not finite");return ftz(v);}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float dvd(float a,float b,const char* what){if(b==0.0F)require(false,std::string("division by zero (")+what+")");return check(vu::div(a,b));}
// max.s / min.s as the interpreter runs them: fs unless ft is strictly greater / smaller.
float max_s(float s,float t){return t>s?t:s;}
float min_s(float s,float t){return t<s?t:s;}
V sub4(const V& a,const V& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
// vmul.xyz, then ACC = 1*x, +1*y, +1*z.
float sum_squares(const V& v){return add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));}
// 00183350: vsqrt of the sum of squares.
float length(const V& v){return check(vu::sqrt(sum_squares(v)));}
// 00192fb0: vmul.xyzw, then (x + y) + z.
float dot3(const V& a,const V& b){return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}
// 00132060: xyz *= vrsqrt(vf0.w, sum of squares); w kept.
V normalized(const V& v,const char* what){
    const float s=sum_squares(v);
    if(s==0.0F)require(false,std::string("zero-length vector (")+what+")");
    const float q=check(vu::rsqrt(1.0F,s));
    return {mul(v[0],q),mul(v[1],q),mul(v[2],q),v[3]};
}
void require_finite(const V& v,const char* what){for(float x:v)if(!std::isfinite(x))require(false,std::string(what)+" not finite");}

// Pool strides (LEVEL_LIGHTS.md 2.1): ambient and point 0x40, directional
// 0x50, spot 0xe0. The direction +0x40 exists in the directional and spot
// pools, the cone cosines +0x50/+0x54 only in the spot pool.
const V& direction_40(const LevelLightRef& r,const LevelLight& l){
    require(r.pool==LevelLightPool::directional||r.pool==LevelLightPool::spot,"light +0x40 read past the pool stride");
    return l.direction;
}
void require_cone(const LevelLightRef& r){
    require(r.pool==LevelLightPool::spot,"light +0x50/+0x54 read past the pool stride");
}

// 003730f0 for table entry k (sphere test around the reference).
void sphere_candidate(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,std::size_t k){
    const ShadowLightTableEntry& e=table.set.table[k];
    const LevelLightRef& r=table.rows[k].light;
    float key=0.0F;
    if(e.flags&0x14U){
        const float d2=sum_squares(sub4(m.reference,e.position));                    // 00183380, 00373290
        if(!(d2<mul(e.range,e.range)))return;                                          // c.olt.s
        key=mul(d2,e.weight);
        if(e.flags==0x10U){                                                            // lbu == 0x10
            require_cone(r);
            const LevelLight& l=lights.light(r);
            if(l.cone_inner!=1.0F){                                                    // 00372ae0
                const V dir=direction_40(r,l);                                         // 00372ad0, 00132020
                const V n=normalized(sub4(e.position,m.reference),"spot light at the reference");   // 00183380, 00132060
                const float limit=sub(1.0F,l.cone_outer);                              // 00372ac0
                if(!(sub(1.0F,dot3(dir,n))<=limit))return;                             // 00192fb0, c.ole.s
            }
        }
    }else if(e.flags!=2U)return;
    m.candidates.push_back({r,key});
}

// 00372120's attenuation of a point (type 4) or spot (0x10) light at distance d.
float distance_factor(const LevelLight& l,float d){
    const float g=max_s(0.0F,sub(d,l.inner));                                          // 001f1350
    const float span=sub(l.outer,l.inner);                                             // 00372af0 - 00372b00
    return g<span?sub(1.0F,dvd(g,span,"attenuation span")):0.0F;
}
// The cone factor of a spot light for the unit vector `dir` toward it.
float cone_factor(const LevelLightRef& r,const LevelLight& l,const V& dir,float f){
    require_cone(r);
    if(l.cone_inner==1.0F)return f;                                                    // 00372ae0
    const float c=dot3(direction_40(r,l),dir);                                         // 00372ad0, 00192fb0
    if(c<=l.cone_outer)return 0.0F;                                                    // 00372ac0, c.ole.s
    if(!(c<l.cone_inner))return f;
    const float a=sub(sub(1.0F,c),sub(1.0F,l.cone_inner));
    const float b=sub(sub(1.0F,l.cone_outer),sub(1.0F,l.cone_inner));
    return mul(f,sub(1.0F,dvd(a,b,"cone span")));
}
// The light's factor, writing the slot direction when `slot` is given.
float light_factor(const ModelLightGather& m,const LevelLightRef& r,const LevelLight& l,V* slot){
    float f=1.0F;
    if(l.type==0x10U||l.type==4U){
        const V v=sub4(l.position,m.reference);                                        // 00372b10, 00183380
        const float d=length(v);                                                       // 00183350
        V dir{};
        const bool directed=l.type==0x10U||slot!=nullptr;
        if(directed)dir={dvd(v[0],d,"light at the reference"),dvd(v[1],d,"light at the reference"),dvd(v[2],d,"light at the reference"),0.0F};
        if(slot)*slot=dir;                                                             // div.s, w = 0 (0013ab00 / 001325e0)
        f=distance_factor(l,d);
        if(l.type==0x10U)f=cone_factor(r,l,dir,f);
    }else if(l.type==2U){
        if(slot)*slot=direction_40(r,l);                                               // 00372b20, 001325e0
    }
    return min_s(max_s(f,0.0F),1.0F);                                                  // 001f1350, 0024ffd0
}

// 00372120 (flags bits 1 and 2; bit 4 is refused by the caller).
void build_lights(ModelLightGather& m,const LevelLights& lights,std::uint32_t mask,std::uint32_t flags){
    if(flags&2U)m.slot_count_48=0;
    if(flags&1U)m.colour={0.0F,0.0F,0.0F,0.0F};
    // +0x2221 = 0 here; the block that sets it (+0x2220 set, aux 0x20,
    // type 2) never runs: the constructor's 0 is the only write of +0x2220.
    int summed=0;                                                                      // s3
    for(const ModelLightCandidate& c:m.candidates){
        const LevelLight& l=lights.light(c.light);
        if((mask&l.type)==0)continue;                                                  // light +0x1e
        bool slot=false;
        if((flags&2U)&&m.slot_count_48<4){
            const std::size_t s=static_cast<std::size_t>(m.slot_count_48);
            const float f=light_factor(m,c.light,l,&m.slot_direction[s]);
            m.slot_colour[s]={mul(f,l.colour[0]),mul(f,l.colour[1]),mul(f,l.colour[2]),0.0F};
            ++m.slot_count_48;
            slot=true;
        }
        if((flags&1U)&&m.dynamic_colour_43&&summed<4){
            if(slot){
                const V& s=m.slot_colour[static_cast<std::size_t>(m.slot_count_48-1)];
                for(std::size_t i=0;i<3;++i)m.colour[i]=add(m.colour[i],s[i]);
            }else{
                const float f=light_factor(m,c.light,l,nullptr);
                for(std::size_t i=0;i<3;++i)m.colour[i]=add(m.colour[i],mul(f,l.colour[i]));
            }
            ++summed;
        }
    }
    if(flags&2U)
        for(;m.slot_count_48<4;++m.slot_count_48){
            const std::size_t s=static_cast<std::size_t>(m.slot_count_48);
            m.slot_direction[s]={0.0F,0.0F,0.0F,0.0F};
            m.slot_colour[s]={0.0F,0.0F,0.0F,0.0F};
        }
    if(flags&1U){
        if(m.dynamic_colour_43){
            const float sixth=std::bit_cast<float>(0x3e2aaaabU);                     // vmulx.xyzw
            for(float& x:m.colour)x=mul(x,sixth);
        }else{
            for(const LevelLight& a:lights.ambient)                                    // manager +0 / +0x10
                if(a.enabled)
                    for(std::size_t i=0;i<4;++i)m.colour[i]=add(m.colour[i],i<3?a.colour[i]:0.0F);   // vadd.xyzw
        }
        for(float& x:m.colour)x=mul(x,128.0F);                                         // vmulx.xyzw
        m.colour[3]=128.0F;                                                            // +0x206c
    }
}

// 00371e70.
void lighting_matrices(ModelLightGather& m,const ShadowMatrix* transform){
    std::array<V,4> t=m.slot_direction;
    if(transform){
        for(const V& c:transform->column)require_finite(c,"model transform");
        const ShadowMatrix inv=shadow_rigid_inverse(*transform);                       // 00133300, 00133200
        for(std::size_t k=0;k<4;++k){t[k]=shadow_transform(inv,m.slot_direction[k]);require_finite(t[k],"model-space direction");}
    }
    m.world_direction=m.slot_direction;                                                // +0x2070
    for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)m.model_direction[i][j]=t[j][i];   // +0x20b0, transposed
    m.light_colour=m.slot_colour;                                                      // +0x20f0
    for(V& c:m.light_colour)for(std::size_t i=0;i<3;++i)c[i]=mul(c[i],128.0F);
}

// The recompute branch of 00373320.
void recompute(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,std::uint32_t mask,std::uint32_t flags){
    m.candidates.clear();                                                              // +0x50 = 0
    const std::size_t n=table.set.table.size();
    if(n!=0){
        require(n<=256,"light table larger than the scratchpad copy (256 entries)");
        for(std::size_t k=0;k<n;++k){
            const ShadowLightTableEntry& e=table.set.table[k];
            if((mask&e.flags)==0)continue;
            if(m.sphere_40)sphere_candidate(m,lights,table,k);
            else{
                // 00373590: a type-2 entry is a candidate with key 0; any other
                // compares (centre distance)^2 against (radius + range)^2 through
                // the VU status flag (cfc2 vi16), which the interpreter does not model.
                require(e.flags==2U,"bounds test 00373590 for a non-directional table entry (VU status flag) is not hosted");
                m.candidates.push_back({table.rows[k].light,0.0F});
            }
        }
        if(!m.candidates.empty()){                                                      // 001273c8 with 00373ba0
            std::vector<float> keys;
            keys.reserve(m.candidates.size());
            for(const ModelLightCandidate& c:m.candidates)keys.push_back(c.key);
            const std::vector<std::size_t> order=shadow_light_order(keys);
            std::vector<ModelLightCandidate> sorted;
            sorted.reserve(order.size());
            for(std::size_t i:order)sorted.push_back(m.candidates[i]);
            m.candidates=std::move(sorted);
        }
    }
    m.dirty_42=false;
    m.mask_44=mask;
    build_lights(m,lights,mask,flags);
}
bool same_bits(const V& a,const V& b){
    for(std::size_t i=0;i<4;++i)if(std::bit_cast<std::uint32_t>(a[i])!=std::bit_cast<std::uint32_t>(b[i]))return false;
    return true;
}
}

void set_model_light_reference(ModelLightGather& m,const ShadowVector& p){
    bool zero=true;
    for(std::size_t i=0;i<4;++i){
        require(std::isfinite(p[i])&&std::isfinite(m.reference[i]),"reference not finite");
        const float d=m.reference[i]-p[i];                                             // vsub.xyzw (MAC flags of the unflushed result)
        require(std::fpclassify(d)!=FP_SUBNORMAL,"denormal reference difference (VU zero flag not established)");
        require(!(d==0.0F&&std::signbit(d)),"-0 reference difference (VU sign flag not established)");
        if(d!=0.0F)zero=false;
    }
    if(zero)return;                                                                    // MAC == 0xf
    m.reference=p;
    m.sphere_40=true;
    m.reference_changed_41=true;
}

void gather_model_lights(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
                         std::uint32_t mask,std::uint32_t flags,const ShadowMatrix* transform){
    require((flags&4U)==0,"flags bit 4 (00371cf0, the +0x2130 light list) is not hosted");
    require(table.rows.size()==table.set.table.size(),"light table rows and entries differ");
    require_finite(m.reference,"reference");
    const bool unhosted=m.unhosted_queries;
    m.unhosted_queries=false;
    if(m.reference_changed_41){m.dirty_42=true;m.reference_changed_41=false;}
    if(!m.dirty_42&&mask!=m.mask_44)m.dirty_42=true;
    if(m.dirty_42)recompute(m,lights,table,mask,flags);
    else if(unhosted){
        // The original may have recomputed here (another reference, mask or a
        // light change in between); accept the cached result only when a
        // recompute gives the same values.
        ModelLightGather fresh=m;
        fresh.sphere_40=true;
        recompute(fresh,lights,table,mask,flags);
        bool same=true;
        if(flags&1U)same=same_bits(fresh.colour,m.colour);
        if(flags&2U){
            same=same&&fresh.slot_count_48==m.slot_count_48;
            for(std::size_t s=0;s<4;++s)same=same&&same_bits(fresh.slot_direction[s],m.slot_direction[s])&&same_bits(fresh.slot_colour[s],m.slot_colour[s]);
        }
        require(same,"00373320's cached path: the cached gather (same reference and mask, no light change seen) differs from a recompute, and unhosted "
                     "code (the renderer's model lighting, light setters) may have used the manager since the last native call");
    }
    if(flags&2U)lighting_matrices(m,transform);
}

std::int32_t model_light_stealth(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
                                 const StealthActor& actor,const StealthThresholds& thresholds){
    set_model_light_reference(m,actor.position);                                       // 00133b68..00133bfc
    gather_model_lights(m,lights,table,0x17U,3U,&actor.transform);                     // 00133c20..00133c30
    return actor_stealth_value(m.colour,actor.player,actor.flashlight_on,              // 00133c38..00133da8
        thresholds.darkness_decreaser,thresholds.npc_darkness_decreaser);
}

void run_stealth_pass(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
                      std::vector<StealthActor>& actors,const StealthThresholds& thresholds){
    for(StealthActor& a:actors){                                                       // 004ffd44 in list order
        if(!(a.present_2d&&a.present_2c&&!a.hidden_2e))continue;                       // 00133b24..00133b60
        a.stealth_44=model_light_stealth(m,lights,table,a,thresholds);
    }
}

StealthLighting::StealthLighting(std::size_t actors,StealthThresholds thresholds)
    :thresholds_(thresholds),values_(actors,0){}
void StealthLighting::update(const LevelLights& lights,const LevelLightTable& table,std::vector<StealthActor>& actors){
    require(actors.size()==values_.size(),"actor count differs from the stealth host's");
    for(std::size_t k=0;k<actors.size();++k)actors[k].stealth_44=values_[k];
    gather_.unhosted_queries=true;
    run_stealth_pass(gather_,lights,table,actors,thresholds_);
    for(std::size_t k=0;k<actors.size();++k)values_[k]=actors[k].stealth_44;
}
void StealthLighting::append_actor(){values_.push_back(0);}
void StealthLighting::reset_actor(std::size_t actor){values_.at(actor)=0;}
ShadowVector particle_game_light(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
    const ShadowVector& point){
    set_model_light_reference(m,point);
    gather_model_lights(m,lights,table,0x16,1,nullptr);
    return m.colour;
}
std::int32_t StealthLighting::value(std::size_t actor) const {
    require(actor<values_.size(),"actor outside the stealth host");
    return values_[actor];
}
}
