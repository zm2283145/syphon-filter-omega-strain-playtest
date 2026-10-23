#include "actor_edge_detection.hpp"
#include "actor_response_probes.hpp"
#include "binary.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t value){return std::bit_cast<float>(value);}
// The EE FPU and VU0 have no denormals: tiny results become signed zero.
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor edge detection: nonfinite arithmetic");
    if(std::fpclassify(v)==FP_SUBNORMAL)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
float divide(float a,float b){
    if(check(b)==0)throw std::runtime_error("Actor edge detection: division by zero");
    return check(check(a)/b);
}
float root(float v){
    if(check(v)<0)throw std::runtime_error("Actor edge detection: square root of a negative value");
    return check(std::sqrt(v));
}
// EE max.s / min.s for ordered operands.
float maximum(float a,float b){return a<b?b:a;}
float minimum(float a,float b){return b<a?b:a;}
RootVector checked(const RootVector& v){RootVector r{};for(int i=0;i<4;++i)r[i]=check(v[i]);return r;}
RootVector add4(const RootVector& a,const RootVector& b){return {add(a[0],b[0]),add(a[1],b[1]),add(a[2],b[2]),add(a[3],b[3])};}
RootVector sub4(const RootVector& a,const RootVector& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
// vmulx.xyzw: all four lanes times the scalar.
RootVector scale4(const RootVector& v,float s){return {mul(v[0],s),mul(v[1],s),mul(v[2],s),mul(v[3],s)};}
// vmulax ACC,vf0,vf0 + vmsubw: 0 - v on every lane.
RootVector negate4(const RootVector& v){return {sub(0,v[0]),sub(0,v[1]),sub(0,v[2]),sub(0,v[3])};}
// vmul.xyzw then vmulax.w/vmadday.w/vmaddz.w: (x*x' + y*y') + z*z'.
float dot3(const RootVector& a,const RootVector& b){return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}
float length_squared(const RootVector& v){return dot3(v,v);}
// vopmula.xyz/vopmsub.xyz, W = 1 - 1.
RootVector cross(const RootVector& a,const RootVector& b){
    return {sub(mul(a[1],b[2]),mul(a[2],b[1])),sub(mul(a[2],b[0]),mul(a[0],b[2])),sub(mul(a[0],b[1]),mul(a[1],b[0])),0};
}
// vrsqrt Q then vmulq.xyz (W kept).
RootVector normalize3(RootVector v){
    const float squared=length_squared(v);
    if(squared==0)throw std::runtime_error("Actor edge detection: normalizing a zero vector");
    const float q=divide(1.0F,root(squared));
    for(int i=0;i<3;++i)v[i]=mul(v[i],q);
    return v;
}
constexpr RootVector up{0,0,1,0};
const float flt_max=std::numeric_limits<float>::max();

}
ActorLadderFrame actor_ladder_frame(float edge_weight,float ladd_weight,const RootParentTransform& edge,const RootParentTransform& ladd){
    // 1e00fc..1e0138: f = w_ladd / (w_edge + w_ladd), 0.5 when the sum is zero.
    const float sum=add(edge_weight,ladd_weight);
    const float f=sum==0.0F?bits(0x3f000000U):divide(ladd_weight,sum);
    // vaddax/vmaddax/vmsubx: (E + L*f) - E*f per lane.
    const auto blend=[f](const RootVector& e,const RootVector& l){
        RootVector r{};
        for(int i=0;i<4;++i)r[i]=sub(add(add(e[i],0.0F),mul(l[i],f)),mul(e[i],f));
        return r;
    };
    ActorLadderFrame out;
    out.origin=blend(edge.position,ladd.position);out.origin[3]=1.0F;        // 1e01b4: W = 1
    auto q=blend(edge.orientation,ladd.orientation);
    // vmul/vmulax.w/vmadday.w/vmaddaz.w/vmaddw.w, vrsqrt, vmulq.xyzw.
    const float squared=add(add(add(mul(q[0],q[0]),mul(q[1],q[1])),mul(q[2],q[2])),mul(q[3],q[3]));
    if(squared==0.0F)throw std::runtime_error("Actor ladder frame: zero blended quaternion");
    const float r=check(vu::rsqrt(squared));
    for(auto& x:q)x=mul(x,r);
    // 00132380 row 1 (the image of local +Y): with t = q + q,
    // (qx*ty - qw*tz, (1 - qx*tx) - qz*tz, qz*ty + qw*tx, 0).
    const RootVector t{add(q[0],q[0]),add(q[1],q[1]),add(q[2],q[2]),add(q[3],q[3])};
    out.axis={sub(mul(q[0],t[1]),mul(q[3],t[2])),sub(sub(1.0F,mul(q[0],t[0])),mul(q[2],t[2])),
              add(mul(q[2],t[1]),mul(q[3],t[0])),0.0F};
    return out;
}
namespace {
const std::uint8_t* raw_triangle(std::span<const std::uint8_t> bytes,std::uint32_t triangle){
    checked_bytes(bytes,0,48);
    if(bytes[0]!='C'||bytes[1]!='O'||bytes[2]!='L')throw std::runtime_error("Actor edges: geometry is not COL");
    // 003c0b50/003c0f70: versions below 5 use 0x40/0x20 triangle strides.
    if(bytes[3]!=5)throw std::runtime_error("Actor edges: only COL version 5 triangles are supported");
    if(triangle>=read_u32(bytes,20))throw std::runtime_error("Actor edges: triangle outside the COL table");
    return checked_bytes(bytes,std::size_t(read_u32(bytes,24))+std::size_t(triangle)*16,16).data();
}
// 1ecee0..1ed088 (word-identical to the 0027d808 block apart from jump
// targets): rows -> quaternion. Lanes moved with vadd from vf0 turn -0 into +0.
RootVector rows_to_quaternion(const RootVector& r0,const RootVector& r1,const RootVector& r2){
    const float half=0.5F;
    const float trace=add(add(r0[0],r1[1]),r2[2]);
    RootVector q{};
    if(trace>0){
        const float s=root(add(trace,1.0F));
        q={add(0,sub(r1[2],r2[1])),add(0,sub(r2[0],r0[2])),add(0,sub(r0[1],r1[0])),s};
        const float inverse=divide(1.0F,s);
        for(auto& v:q)v=mul(v,half);
        for(int i=0;i<3;++i)q[i]=mul(q[i],inverse);
        return q;
    }
    const float xy=add(sub(r0[0],r1[1]),0),yz=add(sub(r1[1],r2[2]),0),zx=add(sub(r2[2],r0[0]),0);
    int largest;
    if(xy<0)largest=yz<0?2:1;
    else largest=zx<0?0:2;
    if(largest==2){                                                           // 1ecfa8
        const float s=root(add(sub(zx,r1[1]),1.0F));
        q={add(0,add(r0[2],r2[0])),add(0,add(r2[1],r1[2])),add(0,s),sub(r0[1],r1[0])};
        const float inverse=divide(1.0F,q[2]);
        for(auto& v:q)v=mul(v,half);
        q[0]=mul(q[0],inverse);q[1]=mul(q[1],inverse);q[3]=mul(q[3],inverse);
    }else if(largest==1){                                                     // 1ecffc
        const float s=root(add(sub(yz,r0[0]),1.0F));
        q={add(0,add(r1[0],r0[1])),add(0,s),add(0,add(r2[1],r1[2])),sub(r2[0],r0[2])};
        const float inverse=divide(1.0F,q[1]);
        for(auto& v:q)v=mul(v,half);
        q[0]=mul(q[0],inverse);q[2]=mul(q[2],inverse);q[3]=mul(q[3],inverse);
    }else{                                                                    // 1ed048
        const float s=root(add(sub(xy,r2[2]),1.0F));
        q={add(0,s),add(0,add(r1[0],r0[1])),add(0,add(r0[2],r2[0])),sub(r1[2],r2[1])};
        const float inverse=divide(1.0F,q[0]);
        for(auto& v:q)v=mul(v,half);
        q[1]=mul(q[1],inverse);q[2]=mul(q[2],inverse);q[3]=mul(q[3],inverse);
    }
    return q;
}
const ActorEdge& edge_at(std::span<const ActorEdge> edges,std::size_t index){
    if(index>=edges.size())throw std::runtime_error("Actor edges: group member outside the edge list");
    return edges[index];
}
std::uint16_t raw_index(const std::uint8_t* record,unsigned corner){
    return static_cast<std::uint16_t>(record[4+corner*2]|(record[5+corner*2]<<8));
}
}

std::vector<ActorEdge> gather_actor_edges(const CollisionCandidateBatch& batch,const ActorEdgeGeometry& geometry,
    const ActorEdgeOwner& owner){
    if(!geometry||!owner)throw std::runtime_error("Actor edges: geometry and owner resolvers are required");
    if(batch.candidates.size()!=batch.sources.size()||batch.candidates.size()>1000000)
        throw std::runtime_error("Actor edges: malformed gathered batch");
    std::vector<ActorEdge> edges;
    for(std::size_t index=0;index<batch.candidates.size();++index){           // 003c0790
        const auto& candidate=batch.candidates[index];
        const auto& source=batch.sources[index];
        if(candidate.source_id!=index||!source.geometry)throw std::runtime_error("Actor edges: candidate source mapping lost");
        const auto bytes=geometry(source.geometry);
        const auto* record=raw_triangle(bytes,source.triangle);
        // 001efb30 (filter object 004f4fe0, vtable +8): accepted unless +38 < 0.5.
        const bool accepted=!(check(candidate.normal.z)<0.5F);
        for(int edge=2;edge>=0;--edge){                                       // 003c08a8/3c0ae8
            const std::uint8_t flags=record[0x0b+edge];                       // 003c0f60 +0x0b
            if((flags&0x7f)==0)continue;                                      // 003c08b4 and -0x81
            if(!accepted)continue;
            const unsigned k=static_cast<unsigned>(edge);
            const std::array<std::uint16_t,2> pair{raw_index(record,k),raw_index(record,(k+1)%3)};
            bool duplicate=false;                                             // 003c0e00
            for(const auto& existing:edges){
                if(existing.identity.geometry!=source.geometry||existing.identity.object!=source.object)continue;
                const auto& o=existing.vertex_indices;
                if((o[0]==pair[0]&&o[1]==pair[1])||(o[0]==pair[1]&&o[1]==pair[0])){duplicate=true;break;}
            }
            if(duplicate)continue;
            // 003c0c10/003c0d90 vertex order, owner transform 1e1d60/171b30.
            const auto draft=expand_col5_triangle(bytes,source,[&]{return owner(source.object);});
            ActorEdge e;
            e.p0=checked(draft.vertices[k]);e.p1=checked(draft.vertices[(k+1)%3]);e.third=checked(draft.vertices[(k+2)%3]);
            e.flags=flags;                                                    // 003c0b50
            // 00183380 then 001d10c0: vsqrt, vdiv 1/length, vmulq.xyz.
            const auto d=sub4(e.p1,e.p0);
            e.length=root(length_squared(d));
            const float inverse=divide(1.0F,e.length);
            e.direction={mul(d[0],inverse),mul(d[1],inverse),mul(d[2],inverse),d[3]};
            e.identity={source.triangle,k,source.geometry,source.object};   // 003c0b30
            e.vertex_indices=pair;
            edges.push_back(e);
        }
    }
    return edges;
}

bool actor_edges_connected(const ActorEdge& a,const ActorEdge& b,float cosine,float distance_squared,float overlap){
    if((a.flags&0x7f)!=(b.flags&0x7f))return false;                       // 003c1598
    // 003c14f0: parallel unless |dot| < cosine.
    if(std::fabs(dot3(a.direction,b.direction))<check(cosine))return false;
    // 003c13c0: squared distance from the member's line (vaddw.x 1 * ...).
    const auto line=[&](const RootVector& point){
        const float t=dot3(sub4(point,a.p0),a.direction);
        const auto d=sub4(point,add4(a.p0,scale4(a.direction,t)));
        return add(add(mul(d[0],d[0]),mul(d[1],d[1])),mul(d[2],d[2]));
    };
    const float near0=line(b.p0),near1=line(b.p1);
    if(!(maximum(near0,near1)<=check(distance_squared)))return false;     // 001f1350 max.s
    const float t0=dot3(sub4(b.p0,a.p0),a.direction);
    const float t1=dot3(sub4(b.p1,a.p0),a.direction);
    const float low=-check(overlap),high=add(a.length,overlap);
    if(!(t0<low)&&t0<=high)return true;
    if(!(t1<low)&&t1<=high)return true;
    return false;
}

ActorEdgeGroups group_actor_edges(std::span<const ActorEdge> edges){
    const float cosine=bits(0x3f34fdf4U),distance=bits(0x3e800000U),overlap=bits(0x3f000000U); // 004a0360/68/70
    ActorEdgeGroups groups;
    for(std::size_t e=0;e<edges.size();++e){
        std::optional<std::size_t> found;
        const std::size_t end=groups.size();                                  // 1e15a0 per edge
        for(std::size_t g=0;g<end;++g){
            bool match=false;                                                 // 001e1480
            for(const auto member:groups[g]){
                if((edges[member].flags&0x7f)!=(edges[e].flags&0x7f))continue;
                if(actor_edges_connected(edges[member],edges[e],cosine,distance,overlap)){match=true;break;}
            }
            if(!match)continue;
            if(!found)found=g;
            else{                                                             // 001e1390, 001e1380
                auto moved=groups[g];
                groups[*found].insert(groups[*found].end(),moved.begin(),moved.end());
                groups[g].clear();
            }
            groups[*found].push_back(e);                                      // 001e1360
        }
        if(!found)groups.push_back({e});                                      // 001e1150/001e1130
    }
    for(std::size_t i=groups.size();i-->0;){                                  // 1dfe20..1dfe70
        if(!groups[i].empty())continue;
        groups[i]=groups.back();
        groups.pop_back();
    }
    return groups;
}

RootVector actor_edge_outward_normal(const ActorEdge& edge){
    auto normal=normalize3(cross(edge.direction,up));                        // 001ed93c..1ed990
    const auto toward=sub4(edge.third,edge.p0);
    if(!(dot3(normal,toward)<=0))normal=negate4(normal);                     // 001ed9f8
    return normal;
}

namespace {
struct LedgeProbe {
    RootVector origin{},displacement{},point{};
    bool hit{};
    void test(FaceCandidate& triangle){                                       // 003bce50 -> 003c3c40
        const auto ray=trace_floor_ray(origin,displacement,std::span(&triangle,1),0,0);
        if(!ray.hit)return;
        hit=true;point=ray.hit->point;displacement=ray.displacement;
    }
};
}

std::uint32_t probe_actor_ledge(const RootVector& adjusted_in,const RootVector& outward_in,
    const RootVector& direction_in,std::span<FaceCandidate> triangles){
    const auto adjusted=checked(adjusted_in),outward=checked(outward_in),direction=checked(direction_in);
    const auto inward=negate4(outward);                                       // sp+9e0
    const auto side=cross(inward,direction);                                  // sp+9c0
    const auto along=side[2]<=0?direction:negate4(direction);                // 001ede10
    const auto origin=[&](float l,float u,float a){
        return add4(add4(add4(adjusted,scale4(inward,a)),scale4(up,u)),scale4(along,l));
    };
    const float lift=bits(0x40000000U);                                       // f20 = 2.0
    std::array<LedgeProbe,7> p;                                               // records 004f5000 + i*0x70
    p[0].origin=origin(-1,lift,bits(0xbe800000U));p[0].displacement=scale4(up,bits(0xc0800000U));
    p[1].origin=origin(1,lift,bits(0xbe800000U));p[1].displacement=scale4(up,bits(0xc0800000U));
    p[2].origin=origin(0,lift,bits(0xbe800000U));p[2].displacement=scale4(up,sub(bits(0xc0f00000U),lift));
    p[3].origin=origin(-1,lift,bits(0xc0000000U));p[3].displacement=scale4(inward,bits(0x40800000U));
    p[4].origin=origin(1,lift,bits(0xc0000000U));p[4].displacement=scale4(inward,bits(0x40800000U));
    p[5].origin=origin(-1,bits(0xc0000000U),bits(0xc0000000U));p[5].displacement=scale4(inward,bits(0x40800000U));
    p[6].origin=origin(1,bits(0xc0000000U),bits(0xc0000000U));p[6].displacement=scale4(inward,bits(0x40800000U));
    const float near_floor=bits(0x40200000U);                                 // 004a03b0 (2.5)
    std::uint32_t result=0;
    for(auto& triangle:triangles){                                            // 1ee900..1eeb60
        if(triangle.flags&0x80)continue;
        if(!(result&1)){
            p[0].test(triangle);
            if(p[0].hit)result|=7;
            else{p[1].test(triangle);if(p[1].hit)result|=7;}
        }
        if(!(result&4)||!(result&2)){
            p[2].test(triangle);
            if(p[2].hit){
                result|=4;
                if(sub(sub(p[2].origin[2],lift),p[2].point[2])<=near_floor)result|=2;
            }
        }
        if(!(result&8)){
            p[3].test(triangle);
            if(p[3].hit)result|=8;
            else{p[4].test(triangle);if(p[4].hit)result|=8;}
        }
        if(!(result&0x10)){
            p[5].test(triangle);
            if(p[5].hit)result|=0x10;
            else{p[6].test(triangle);if(p[6].hit)result|=0x10;}
        }
    }
    return result;
}

std::uint32_t actor_ledge_probe_mask(std::uint32_t result){
    // Entries {bit, required, excluded} built from 0048a978/80/88 on first use.
    struct Entry { std::uint32_t bit,required,excluded; };
    constexpr std::array<Entry,3> table{{{9,0,0x10},{8,0,0x04},{2,0,0x02}}};
    std::uint32_t mask=0;
    for(const auto& e:table)
        if((result&e.required)==e.required&&(~result&e.excluded)==e.excluded)mask|=1U<<e.bit;
    return mask;
}

const std::array<ActorEdgePreset,9>& actor_edge_presets(){
    // 001ea4f0 (from 001eb5c0); cosine limits are computed there from angles.
    static const std::array<ActorEdgePreset,9> presets{{
        {0,0,0x1000,0,9,bits(0xbfc00000U),bits(0x3f000000U),bits(0x80000000U),bits(0x41100000U),bits(0x3effffffU),bits(0x3f800000U),true,false},
        {1,0,0x1000,0,9,bits(0xbfc00000U),bits(0x3f000000U),bits(0xbe800000U),bits(0x41100000U),bits(0x3db27ebeU),bits(0x3f800000U),true,false},
        {3,0x10,0x1000,0,9,bits(0x3fc00000U),bits(0x40e00000U),bits(0x80000000U),bits(0x41800000U),bits(0xbe31d0d5U),bits(0x3f800000U),false,false},
        {5,0,0x1000,0,9,bits(0x40e00000U),bits(0x41440000U),bits(0x80000000U),bits(0x41800000U),bits(0xbf7ffd81U),bits(0x3f800000U),false,false},
        {6,0,0x1000,0,9,bits(0x41440000U),bits(0x41b20000U),bits(0x80000000U),bits(0x41800000U),bits(0xbf7ffd81U),bits(0x3f800000U),false,false},
        {7,0,0x1000,0,0xd,bits(0x40200000U),bits(0x40a00000U),bits(0x80000000U),bits(0x41c80000U),bits(0x3db27ebeU),bits(0x3f800000U),false,true},
        {8,0,0x1000,0,0,bits(0x40b00000U),bits(0x41100000U),bits(0xc0100000U),bits(0x41100000U),bits(0x3effffffU),bits(0x3f800000U),false,false},
        {12,0x1000,0x40000,0,9,bits(0xff7fffffU),bits(0x3f000000U),bits(0x80000000U),bits(0x41c80000U),bits(0xbf7ffd81U),bits(0x3f800000U),true,false},
        {12,0x1000,0x40000,0,9,bits(0x3fc00000U),bits(0x41080000U),bits(0x80000000U),bits(0x41c80000U),bits(0xbf7ffd81U),bits(0x3f800000U),false,false},
    }};
    return presets;
}

void score_actor_edge(const ActorEdgePreset& preset,ActorEdgeRecord& record,ActorLedgeProbeCache& cache,
    const ActorEdge& edge,const ActorEdgeScoreInput& in,std::span<FaceCandidate> triangles){
    if(!record.requested)return;                                              // +31
    std::uint32_t mask=in.mask;
    if((mask&preset.required)!=preset.required)return;                       // 001ed8d0
    if((~mask&preset.excluded)!=preset.excluded)return;                      // 001ed8a0/8d0/860
    const float height=check(in.height);
    if(height<=record.height)return;                                          // strict improvement over +e0
    if(height<preset.height_min||!(height<=preset.height_max))return;
    const auto outward=actor_edge_outward_normal(edge);                      // 001ed910
    float cosine=dot3(checked(in.forward),outward);
    if(!preset.polarity)cosine=-cosine;
    cosine=minimum(maximum(cosine,-1.0F),1.0F);                               // max.s / min.s
    if(cosine<preset.cosine_min||!(cosine<=preset.cosine_max))return;
    const auto away=checked(in.away);
    float distance=add(add(mul(away[0],away[0]),mul(away[1],away[1])),mul(away[2],away[2]));
    distance=mul(distance,dot3(away,outward)<=0?-1.0F:1.0F);
    if(preset.polarity)distance=-distance;
    if(distance<preset.distance_min||!(distance<=preset.distance_max))return;
    if(!preset.allow_unsupported&&in.unsupported_dc)return;                   // +30 / stack +0
    bool accept=true;
    if(in.ordinary){
        auto& cached=cache.results[preset.polarity?1:0];                     // cache +8 / +0
        if(!cached)cached=probe_actor_ledge(in.adjusted,outward,edge.direction,triangles);
        const std::uint32_t result=*cached;
        accept=(result&preset.probe_required)==preset.probe_required
            &&(~result&preset.probe_excluded)==preset.probe_excluded;
        mask|=actor_ledge_probe_mask(result);                                 // 001eda40, ORed either way
    }
    if(!accept)return;
    record.accepted=true;                                                     // 1ed66c..1ed76c
    record.edge=edge;
    record.mask=mask|(1U<<(preset.selector&31));
    record.outward=outward;
    record.projected=checked(in.projected);
    record.adjusted=checked(in.adjusted);
    record.height=height;
    record.cosine=cosine;
}

void publish_actor_edge(ActorEdgeSlotState& slot,const ActorEdgeRecord& record,const ActorEdgeGroups& groups,
    std::span<const ActorEdge> edges){
    if(!record.accepted){slot.result=false;return;}                         // 001ed0b0
    if(!slot.request||slot.result)return;
    ActorEdgeSlotState next=slot;
    next.result=true;
    next.edge=record.edge;next.mask=record.mask;next.cosine=record.cosine;
    const auto tangent=cross(up,record.outward);                              // sp+1f0
    if(next.mask&0x8000){
        const float side=dot3(sub4(record.projected,record.adjusted),tangent);
        next.mask|=side<0?0x20000U:0x10000U;
    }
    if(next.mask&0x100){
        const std::vector<std::size_t>* group=nullptr;                        // 001ed120
        for(const auto& g:groups){
            for(const auto member:g)if(edge_at(edges,member).identity==record.edge.identity){group=&g;break;}
            if(group)break;
        }
        if(group){
            const float reach=bits(0x3fdeb852U);                              // 004a0320 (1.74)
            const auto within=[&](const ActorEdge& m,const RootVector& q){
                float a=dot3(sub4(m.p1,m.p0),tangent),b=dot3(sub4(q,m.p0),tangent);
                if(a<0){a=-a;b=-b;}
                return !(b<0)&&b<=a&&a!=0;
            };
            for(const auto member:*group){
                const auto& m=edge_at(edges,member);
                auto offset=scale4(m.direction,reach);
                if(dot3(offset,tangent)<0)offset=negate4(offset);
                if(within(m,sub4(record.adjusted,offset)))next.mask|=0x400;
                if(within(m,add4(record.adjusted,offset)))next.mask|=0x800;
            }
        }
    }
    const auto inward=negate4(record.outward);                                // sp+140
    const auto x=cross(inward,up);                                            // 001320f0
    if(x[0]==0&&x[1]==0&&x[2]==0)                                             // 001320b0 -> 131c70/131c40
        throw std::runtime_error("Actor edge publish: vertical outward normal (00131c70 frame) is not native");
    const auto row1=normalize3(inward);                                       // 00132060
    const auto row0=normalize3(x);                                            // 00132030
    const auto row2=cross(row0,row1);
    next.orientation=rows_to_quaternion(row0,row1,row2);
    next.point=record.adjusted;
    slot=next;
}

std::vector<ActorEdgeMessage> select_actor_edge_request(const std::array<ActorEdgeSlotState,9>& slots){
    std::vector<ActorEdgeMessage> messages;
    float best=-flt_max;
    int winner=-1;
    bool passive=false,event_sent=false;
    std::uint32_t found=0;
    for(int s=8;s>=0;--s){                                                    // 1e1678..1e17a8
        const auto& slot=slots[static_cast<std::size_t>(s)];
        if(!slot.request)continue;
        if(slot.result){
            if(!slot.query_only){
                const float cosine=check(slot.cosine);
                if(cosine<=best)continue;
                best=cosine;winner=s;
            }else{found|=slot.mask;passive=true;}
        }else if(slot.query_only)passive=true;
        else if((s>=1&&s<=4)||s==7||s==8){                                     // table 004a0470
            if(event_sent)continue;
            messages.push_back({ActorEdgeMessage::Kind::cas_event,8,0,static_cast<std::size_t>(s)});
            event_sent=true;
        }
    }
    if(winner>=0){                                                            // table 004a0440
        if(winner==0)throw std::runtime_error("Edge request: slot 0 running jump (physical +9a7 action) is not native");
        if(winner!=6){
            const auto& slot=slots[static_cast<std::size_t>(winner)];
            ActorEdgeMessage m{ActorEdgeMessage::Kind::cas_action,0x2a,14,static_cast<std::size_t>(winner),slot.mask,
                slot.edge.identity.object,slot.orientation,slot.point};
            messages.push_back(m);
        }
    }
    if(passive){                                                              // "found ledge" 004a0318
        ActorEdgeMessage m{ActorEdgeMessage::Kind::found_ledge};
        m.mask=found;
        messages.push_back(m);
    }
    return messages;
}

ActorEdgeDetectionResult detect_actor_edges(const ActorEdgeDetectionInput& in,
    const std::array<ActorEdgeSlotState,9>& slots,CollisionCandidateBatch& gathered,
    const ActorEdgeGeometry& geometry,const ActorEdgeOwner& owner){
    ActorEdgeDetectionResult out;
    const bool ladder=(in.bits_24&0x200)!=0;                                  // 1dffb0
    out.edges=gather_actor_edges(gathered,geometry,owner);                    // 1efa30, 3c0790
    out.groups=group_actor_edges(out.edges);
    const auto& presets=actor_edge_presets();
    for(std::size_t i=0;i<9;++i){                                             // 1dfeb0
        out.records[i].requested=slots[i].request;
        out.records[i].height=-flt_max;
    }
    out.ladder.top_990=-flt_max;out.ladder.bottom_998=flt_max;               // 0048a998 byte is zero
    out.ladder.band3_994=in.band3_994;                                        // retained
    const bool settled=!ladder&&!in.unsupported_dc&&!in.byte_de&&check(in.value_394)<=bits(0x3e4ccccdU);
    RootVector reference=checked(in.floor_root);                              // sp+470
    RootVector predicted=add4(reference,scale4(checked(in.velocity_310),bits(0x3dcccccdU))); // sp+460
    RootVector ladder_axis{};                                                 // sp+490
    if(ladder){
        const auto frame=actor_ladder_frame(in.edge_weight,in.ladd_weight,in.edge_world,in.ladd_world);
        reference=predicted=frame.origin;ladder_axis=frame.axis;
    }
    const auto forward=checked(in.forward);                                   // sp+4a0
    auto triangles=gathered.candidates;                                       // staged two-sided flips
    for(const auto& c:out.edges){                                             // 1e0318..1e0d40
        const std::uint8_t flags=c.flags;
        const bool special=(flags&1)!=0;
        if(!(std::fabs(c.direction[2])<=bits(0x3f34fdf4U)))continue;
        const auto horizontal=normalize3({c.direction[0],c.direction[1],0,c.direction[3]});
        std::uint32_t mask=0;
        if(special){
            mask|=0x1000;
            if(settled){
                const float dz=sub(c.p0[2],reference[2]);
                const unsigned band=(flags>>1)&7;
                const float lo=bits(0xbf400000U),hi=bits(0x3f400000U),one=bits(0x3f800000U); // 004a0430
                if(band==3&&!(dz<=lo)&&dz<hi)mask|=0x8000;
                else if(band==2&&!(dz<=add(lo,one))&&dz<add(hi,one))mask|=0x4000;
                else if(band==1&&!(dz<=lo)&&dz<hi)mask|=0x2000;
                else mask|=0x40000;
            }else mask|=0x40000;
        }
        const float extension=(mask&0x4000)?2.0F:(mask&0x8000)?3.5F:(mask&0x2000)?2.5F:0.0F;
        const auto stretch=scale4(c.direction,extension);
        const auto a0=sub4(c.p0,stretch),a1=add4(c.p1,stretch);
        const auto& query=special?reference:predicted;
        const auto span=sub4(a1,a0);
        float a=dot3(span,horizontal),b=dot3(sub4(query,a0),horizontal);
        if(a<0){a=-a;b=-b;}
        if(b<0||!(b<=a)||a==0)continue;
        const auto projected=add4(a0,scale4(span,divide(b,a)));                 // sp+3c0
        auto adjusted=projected;                                              // sp+3b0
        if(ladder){                                                           // 1e0718..1e0a44
            if(!special)continue;
            const float plane=dot3(sub4(c.p0,reference),ladder_axis);
            if(!(std::fabs(plane)<bits(0x3f000000U)))continue;
            auto& l=out.ladder;
            l.top_990=maximum(l.top_990,projected[2]);                        // max.s
            l.bottom_998=minimum(l.bottom_998,projected[2]);                  // min.s
            if(flags&0x10)l.flags_99c[9]=1;                                   // +9a5
            const float one=bits(0x3f800000U),near=bits(0x3f400000U),far=bits(0xbf400000U);
            const float rz=sub(projected[2],reference[2]);
            const float r7=add(rz,bits(0x40e00000U));                         // 004a0410
            const unsigned band=(flags>>1)&7;
            if(!(rz<=sub(far,one))&&rz<near){                                 // (-1.75, 0.75)
                if(band==1)l.flags_99c[4]=1;                                  // +9a0
                continue;
            }
            const float window=add(near,one);                                 // 1.75
            if(r7<=far||!(r7<add(one,window)))continue;                       // (-0.75, 2.75)
            const auto side=[&]{
                switch((flags>>5)&3){
                case 0:l.flags_99c[7]=1;break;                                // +9a3
                case 1:l.flags_99c[5]=1;break;                                // +9a1
                case 2:l.flags_99c[6]=1;break;                                // +9a2
                default:l.flags_99c[5]=1;l.flags_99c[6]=1;break;
                }
            };
            if(band==2){if(r7<window)l.flags_99c[2]=1;}                       // +99e
            else if(band==3){
                if(r7<add(near,one)){l.flags_99c[8]=0;l.flags_99c[3]=1;l.band3_994=projected[2];side();}
            }else if(band==4){
                if(r7<near&&l.flags_99c[3]==0){l.flags_99c[8]=1;side();}       // +9a4 unless +99f
            }
            continue;
        }
        if(in.mode_20!=0&&!(in.bits_24&2))continue;                           // 1e0a48
        auto relative=sub4(adjusted,reference);
        float height=relative[2];
        const RootVector away{sub(0,relative[0]),sub(0,relative[1]),sub(0,0.0F),sub(0,relative[3])};
        if(special){
            if(mask&0x1000){
                adjusted=scale4(add4(c.p0,c.p1),0.5F);
                if(mask&0xc000)adjusted[2]=add(adjusted[2],bits(0x40e00000U));  // 004a0410
                relative=sub4(adjusted,reference);
                height=relative[2];
            }
        }else if(flags&4)mask|=0x10;
        ActorLedgeProbeCache cache;
        for(std::size_t i=0;i<9;++i){
            const ActorEdgeScoreInput score{mask,away,projected,adjusted,forward,height,in.unsupported_dc,!special};
            score_actor_edge(presets[i],out.records[i],cache,c,score,triangles);
        }
    }
    out.slots=slots;
    for(std::size_t i=0;i<9;++i)publish_actor_edge(out.slots[i],out.records[i],out.groups,out.edges);
    if(in.mode_20==9){                                                        // 1e0d98: tail
        const float z=check(in.ladd_world.position[2]);                       // 00132400('ladd') +10.z
        out.ladder.flags_99c[1]=sub(out.ladder.top_990,z)<bits(0x3f000000U)?1:0;                   // 001e19f0 +99d
        out.ladder.flags_99c[0]=sub(add(out.ladder.bottom_998,bits(0x40e00000U)),z)<=bits(0xbf000000U)?0:1; // 001e1a60 +99c
    }
    if(out.slots[6].result)out.edge_direction_a10=out.slots[6].edge.direction; // +7a2 -> +a10
    gathered.candidates=std::move(triangles);
    return out;
}
}
