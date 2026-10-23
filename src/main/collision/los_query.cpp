#include "float_bits.hpp"
#include "los_query.hpp"
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
float checked(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("LOS arithmetic outside finite normal-or-zero domain");
    return value;
}
float add(float a,float b){return checked(a+b);}
float sub(float a,float b){return checked(a-b);}
float mul(float a,float b){return checked(a*b);}
float divide(float a,float b){if(b==0)throw std::runtime_error("LOS projected triangle is degenerate");return checked(a/b);}
void validate(const RootVector& value){for(auto lane:value)checked(lane);}
RootVector difference(const RootVector& a,const RootVector& b) {
    RootVector out{};for(unsigned i=0;i<4;++i)out[i]=sub(a[i],b[i]);return out;
}
float dot_xyz(const RootVector& a,const RootVector& b) {
    RootVector products{};for(unsigned i=0;i<4;++i)products[i]=mul(a[i],b[i]);
    return add(add(products[0],products[1]),products[2]);
}
}
namespace {
struct Crossing {RootVector normal,point,shortened;};
// 003c45b0 / 003c4a80: the shared crossing test of one candidate against the
// segment (origin, displacement); a flip is written to stored when flag 2 is set.
std::optional<Crossing> crossing(const RootVector& origin,const RootVector& displacement,const LosTriangle& candidate,RootVector& stored){
    validate(stored);for(const auto& vertex:candidate.vertices)validate(vertex);
    auto normal=stored;
    auto approach=dot_xyz(displacement,normal);
    if(!(approach<0)) {
        // Original VU computes zero minus each lane; only flag2 writes back.
        for(auto& lane:normal)lane=sub(0,lane);
        if(candidate.flags&2U)stored=normal;
        approach=checked(-approach);
    }
    const auto plane=dot_xyz(difference(candidate.vertices[0],origin),normal);
    if(!(plane<0&&approach<plane))return std::nullopt; // Both segment endpoints excluded.
    const auto fraction=divide(plane,approach);
    Crossing c;c.normal=normal;
    for(unsigned i=0;i<4;++i){c.shortened[i]=mul(displacement[i],fraction);c.point[i]=add(origin[i],c.shortened[i]);}
    // Projection uses the stored candidate normal, including a persistent
    // flip; absolute values make the temporary orientation irrelevant.
    const auto x=std::abs(stored[0]),y=std::abs(stored[1]),z=std::abs(stored[2]);
    unsigned axis;
    if(y<x)axis=x<z?2U:0U;else axis=z<y?1U:2U;
    const auto a=difference(candidate.vertices[1],candidate.vertices[0]);
    const auto b=difference(candidate.vertices[2],candidate.vertices[0]);
    const auto q=difference(c.point,candidate.vertices[0]);
    // Cyclic projection preserves the original scalar MULA/MSUB operand
    // order, with explicit binary32 rounding instead of host contraction.
    const unsigned u=axis==0?1U:(axis==1?2U:0U),v=axis==0?2U:(axis==1?0U:1U);
    const auto reciprocal=divide(1,sub(mul(a[u],b[v]),mul(a[v],b[u])));
    const auto alpha=mul(reciprocal,sub(mul(b[v],q[u]),mul(b[u],q[v])));
    const auto beta=mul(reciprocal,sub(mul(q[v],a[u]),mul(q[u],a[v])));
    if(alpha<0||beta<0||add(alpha,beta)>1)return std::nullopt;
    return c;
}
void check_count(std::size_t n){if(n>1000000)throw std::runtime_error("LOS candidate limit exceeded");}
}

LosResult trace_los_triangles(const LosRay& ray,std::span<LosTriangle> candidates) {
    check_count(candidates.size());
    validate(ray.origin);validate(ray.displacement);
    std::vector<RootVector> normals;normals.reserve(candidates.size());
    for(const auto& candidate:candidates)normals.push_back(candidate.normal);
    LosResult result;result.displacement=ray.displacement;
    for(std::size_t index=0;index<candidates.size();++index) {
        ++result.visited;
        const auto& candidate=candidates[index];
        if((candidate.flags&ray.excluded_flags)!=0||
           (candidate.flags&ray.required_flags)!=ray.required_flags){++result.filtered;continue;}
        const auto c=crossing(ray.origin,result.displacement,candidate,normals[index]);
        if(!c)continue;
        result.hit=LosHit{c->normal,c->point,candidate.source,index};
        result.displacement=c->shortened;
    }
    for(std::size_t i=0;i<candidates.size();++i)candidates[i].normal=normals[i];
    return result;
}

LosPassResult trace_los_pass_triangles(const LosRay& ray,std::uint8_t pass_flags,std::span<LosTriangle> candidates) {
    check_count(candidates.size());
    validate(ray.origin);validate(ray.displacement);
    std::vector<RootVector> normals;normals.reserve(candidates.size());
    for(const auto& candidate:candidates)normals.push_back(candidate.normal);
    LosPassResult result;result.first.displacement=ray.displacement;
    bool seen=false;
    // 003c3ad0..003c3b44: excluded, then required, then the pass flag (seen, skipped), else 003c45b0.
    for(std::size_t index=0;index<candidates.size();++index) {
        ++result.first.visited;
        const auto& candidate=candidates[index];
        if((candidate.flags&ray.excluded_flags)!=0||
           (candidate.flags&ray.required_flags)!=ray.required_flags){++result.first.filtered;continue;}
        if((candidate.flags&pass_flags)!=0){seen=true;++result.first.filtered;continue;}
        const auto c=crossing(ray.origin,result.first.displacement,candidate,normals[index]);
        if(!c)continue;
        result.first.hit=LosHit{c->normal,c->point,candidate.source,index};
        result.first.displacement=c->shortened;
    }
    // 003c3b50..003c3c08: only when a pass-flag triangle was seen; 003c4a80 tests
    // the shortened segment and reports a crossing without recording it.
    if(seen)
        for(std::size_t index=0;index<candidates.size();++index) {
            const auto& candidate=candidates[index];
            if((candidate.flags&pass_flags)==0||(candidate.flags&ray.excluded_flags)!=0||
               (candidate.flags&ray.required_flags)!=ray.required_flags)continue;
            if(const auto c=crossing(ray.origin,result.first.displacement,candidate,normals[index]))
                result.crossings.push_back({c->point,index});
        }
    for(std::size_t i=0;i<candidates.size();++i)candidates[i].normal=normals[i];
    return result;
}
}