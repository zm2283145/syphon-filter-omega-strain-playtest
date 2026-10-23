#include "collision_face.hpp"
#include "binary.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace {
float checked(float v) {
    if (!std::isfinite(v)) throw std::runtime_error("Nonfinite face-query arithmetic");
    return v;
}
Vec3 valid(Vec3 v) { checked(v.x); checked(v.y); checked(v.z); return v; }
Vec3 add(Vec3 a, Vec3 b) { return valid({a.x+b.x,a.y+b.y,a.z+b.z}); }
Vec3 sub(Vec3 a, Vec3 b) { return valid({a.x-b.x,a.y-b.y,a.z-b.z}); }
Vec3 scale(Vec3 a, float b) { return valid({a.x*b,a.y*b,a.z*b}); }
float dot(Vec3 a, Vec3 b) { return checked((a.x*b.x+a.y*b.y)+a.z*b.z); }
Vec3 cross(Vec3 a, Vec3 b) { return valid({a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}); }
Bounds bounds(Vec3 a, Vec3 b, float radius) {
    return {valid({std::min(a.x,b.x)-radius,std::min(a.y,b.y)-radius,std::min(a.z,b.z)-radius}),
            valid({std::max(a.x,b.x)+radius,std::max(a.y,b.y)+radius,std::max(a.z,b.z)+radius})};
}
bool overlaps(const Bounds& box, const FaceCandidate& c) {
    const auto& a=c.vertices[0]; const auto& b=c.vertices[1]; const auto& d=c.vertices[2];
    return std::max({a.x,b.x,d.x})>=box.minimum.x && std::min({a.x,b.x,d.x})<=box.maximum.x
        && std::max({a.y,b.y,d.y})>=box.minimum.y && std::min({a.y,b.y,d.y})<=box.maximum.y
        && std::max({a.z,b.z,d.z})>=box.minimum.z && std::min({a.z,b.z,d.z})<=box.maximum.z;
}
void vector_json(std::ostream& out, Vec3 v);
bool zero(Vec3 v) { return v.x==0 && v.y==0 && v.z==0; }
// FPU arithmetic past the binary32 range gives +-FLT_MAX (no infinities);
// div.s x / 0 gives FLT_MAX with the sign of x * 0 (+ for 0 / 0).
float clamped(float v) {
    if (std::isnan(v)) throw std::runtime_error("Nonfinite face-query arithmetic");
    return std::isinf(v) ? std::copysign(std::numeric_limits<float>::max(),v) : v;
}
float fpu_div(float a, float b) {
    if (b==0) return std::copysign(std::numeric_limits<float>::max(),a!=0 && std::signbit(a)!=std::signbit(b) ? -1.F : 1.F);
    return clamped(a/b);
}
void validate(const FaceCandidate& c) {
    for (const auto v:c.vertices) valid(v);
    valid(c.normal);
    const auto area=cross(sub(c.vertices[1],c.vertices[0]),sub(c.vertices[2],c.vertices[0]));
    const float area2=dot(area,area);
    // A COL triangle with exactly collinear vertices expands (003c1254) to a
    // zero normal, and a transformed or projected candidate can lose its area
    // in rounding; the EE queries them unchecked (FACE_QUERY_DEGENERATE in
    // HUMAN_COLLISION.md).
    if (zero(c.normal) && (area2==0 || c.query_transformed)) return;
    const float normal2=dot(c.normal,c.normal);
    if(normal2<=0) {
        std::ostringstream out;
        out<<std::setprecision(9)<<"Face-query normal must be nonzero (source "<<c.source_id<<", flags "<<unsigned(c.flags)
           <<", transformed "<<c.query_transformed<<", vertices ";
        vector_json(out,c.vertices[0]);vector_json(out,c.vertices[1]);vector_json(out,c.vertices[2]);
        out<<", normal ";vector_json(out,c.normal);out<<')';
        throw std::runtime_error(out.str());
    }
    if (c.query_transformed) return;
    if (area2<=0) throw std::runtime_error("Degenerate face-query triangle");
    const auto direction=scale(area,checked(1.F/std::sqrt(area2)));
    const auto normal_direction=scale(c.normal,checked(1.F/std::sqrt(normal2)));
    const float agreement=std::abs(dot(direction,normal_direction));
    if (agreement<1.F-0.0001F) {
        std::ostringstream out;
        out<<std::setprecision(9)<<"Face-query normal does not match triangle plane (source "<<c.source_id
           <<", flags "<<unsigned(c.flags)<<", |cos| "<<agreement<<", area2 "<<area2<<", normal2 "<<normal2<<", vertices ";
        vector_json(out,c.vertices[0]);vector_json(out,c.vertices[1]);vector_json(out,c.vertices[2]);
        out<<", normal ";vector_json(out,c.normal);out<<')';
        throw std::runtime_error(out.str());
    }
}
bool contains(const FaceCandidate& c, Vec3 point) {
    const auto a=sub(c.vertices[1],c.vertices[0]);
    const auto b=sub(c.vertices[2],c.vertices[0]);
    const auto p=sub(point,c.vertices[0]);
    const float x=std::abs(c.normal.x), y=std::abs(c.normal.y), z=std::abs(c.normal.z);
    // Preserve the recovered dominant-axis tie rule. Projection order is cyclic.
    const int axis=x>y ? (z>x ? 2 : 0) : (y>z ? 1 : 2);
    float a0,a1,b0,b1,p0,p1;
    if (axis==2) { a0=a.x;a1=a.y;b0=b.x;b1=b.y;p0=p.x;p1=p.y; }
    else if (axis==1) { a0=a.z;a1=a.x;b0=b.z;b1=b.x;p0=p.z;p1=p.x; }
    else { a0=a.y;a1=a.z;b0=b.y;b1=b.z;p0=p.y;p1=p.z; }
    const float determinant=checked(a0*b1-a1*b0);
    // A collinear projection (zero determinant) takes div.s's +-FLT_MAX
    // reciprocal and clamped products: u and v then have opposite signs (not
    // contained) unless the point's coordinate across the line is zero.
    const float inverse=fpu_div(1.F,determinant);
    const float u=clamped(inverse*checked(b1*p0-b0*p1));
    const float v=clamped(inverse*checked(p1*a0-p0*a1));
    return u>=0 && v>=0 && clamped(u+v)<=1;
}
void vector_json(std::ostream& out, Vec3 v) { out<<'['<<v.x<<','<<v.y<<','<<v.z<<']'; }
std::optional<float> root(float a,float b,float c) {
    const float discriminant=checked(b*b-checked(a*4.F)*c);
    if(discriminant<0 || a==0) return {};
    const float half_inverse=checked(0.5F/a);
    const float radical=std::sqrt(discriminant);
    const float value=checked(half_inverse*(a>0 ? -b-radical : -b+radical));
    if(value>=1 || checked(-b*half_inverse)<=0) return {};
    return std::max(0.F,value);
}
void contact(FaceResult& result, const FaceCandidate& c, Vec3 displacement, Vec3 normal, Vec3 point,unsigned type) {
    const float norm=dot(normal,normal);
    if(norm<=0) throw std::runtime_error("Undefined edge/vertex contact normal");
    normal=scale(normal,checked(1.F/std::sqrt(norm)));
    result.displacement=displacement;
    result.status=FaceResult::Status::hit;
    result.contact=FaceContact{normal,point,c.source_id,type};
}
bool fallback(const FaceQuery& q, FaceResult& result, const FaceCandidate& c) {
    bool edge_hit=false;
    const float radius2=checked(q.radius*q.radius);
    for(unsigned i=0;i<3;++i) {
        const auto a=c.vertices[i], edge=sub(c.vertices[(i+1)%3],a), w=sub(q.origin,a);
        const float ww=dot(w,w), we=dot(w,edge), wd=dot(w,result.displacement);
        const float ee=dot(edge,edge), ed=dot(edge,result.displacement), dd=dot(result.displacement,result.displacement);
        const float qa=checked(ed*ed-ee*dd), qb=checked(2.F*checked(we*ed-wd*ee));
        const float qc=checked(checked(we*we+radius2*ee)-ww*ee);
        const auto t=root(qa,qb,qc);
        if(!t || ee==0) continue;
        const float u=checked(checked(we+*t*ed)/ee);
        if(u<0 || u>1) continue;
        const auto shortened=scale(result.displacement,*t), along=scale(edge,u);
        contact(result,c,shortened,sub(add(w,shortened),along),add(a,along),2);
        edge_hit=true;
    }
    if(edge_hit) return true;
    bool vertex_hit=false;
    for(const auto v:c.vertices) {
        const auto w=sub(q.origin,v);
        const float qa=dot(result.displacement,result.displacement), qb=checked(dot(w,result.displacement)*2.F);
        const float qc=checked(dot(w,w)-radius2);
        const auto t=root(qa,qb,qc);
        if(!t) continue;
        const auto shortened=scale(result.displacement,*t);
        contact(result,c,shortened,add(w,shortened),v,3);
        vertex_hit=true;
    }
    return vertex_hit;
}
}

void validate_face_candidate(const FaceCandidate& candidate) { validate(candidate); }

bool query_edge_vertices(const FaceQuery& q, FaceResult& result, const FaceCandidate& candidate) {
    valid(q.origin); valid(result.displacement); checked(q.radius);
    if(q.radius<0) throw std::runtime_error("Negative edge/vertex query radius");
    validate(candidate);
    auto draft=result;
    const bool hit=fallback(q,draft,candidate);
    if(hit) result=std::move(draft);
    return hit;
}

FaceResult query_faces(const FaceQuery& q, std::span<FaceCandidate> candidates, bool include_edges) {
    valid(q.origin); valid(q.displacement); checked(q.radius);
    checked(q.min_start_distance); checked(q.min_approach_cosine);
    checked(q.min_direct_cosine); checked(q.max_end_distance);
    if (q.radius<0) throw std::runtime_error("Negative face-query radius");
    for (const auto& c:candidates) validate(c); // Fail before any normal mutation.
    const float length2=dot(q.displacement,q.displacement);
    const auto unit=length2>0 ? scale(q.displacement,checked(1.F/std::sqrt(length2))) : Vec3{};
    auto box=bounds(q.origin,add(q.origin,q.displacement),q.radius);
    FaceResult result;
    result.displacement=q.displacement;
    for (std::size_t i=0;i<candidates.size();++i) {
        auto& c=candidates[i]; ++result.visited;
        if ((c.flags&q.required_flags)!=q.required_flags || (c.flags&q.excluded_flags)!=0) {
            ++result.filtered; continue;
        }
        if (!overlaps(box,c)) continue;
        ++result.aabb_pass;
        float distance=dot(sub(q.origin,c.vertices[0]),c.normal);
        if ((c.flags&2)!=0 && distance<0) { c.normal=scale(c.normal,-1); distance=-distance; }
        if (distance<q.min_start_distance) continue;
        const float cosine=-dot(unit,c.normal);
        if (cosine<q.min_approach_cosine) continue;
        const float approach=-dot(result.displacement,c.normal);
        if (checked(distance-approach)>q.max_end_distance) continue;
        if (cosine<q.min_direct_cosine || approach<=0) continue;
        const float fraction=std::clamp(checked(checked(distance-q.radius)/approach),0.F,1.F);
        const auto shortened=scale(result.displacement,fraction);
        const auto point=add(sub(q.origin,scale(c.normal,q.radius)),shortened);
        if (!contains(c,point)) {
            if(include_edges) {
                if(fallback(q,result,c)) box=bounds(q.origin,add(q.origin,result.displacement),q.radius);
                continue;
            }
            result.status=FaceResult::Status::unsupported_edge_vertex;
            result.unsupported_index=i;
            return result;
        }
        result.status=FaceResult::Status::hit;
        result.contact=FaceContact{c.normal,point,c.source_id};
        result.displacement=shortened;
        dot(shortened,shortened);
        box=bounds(q.origin,add(q.origin,shortened),q.radius);
    }
    return result;
}

std::vector<FaceCandidate> decode_col_faces(std::span<const std::uint8_t> bytes) {
    const auto mesh=decode_col(bytes);
    const auto count=read_u32(bytes,40), offset=read_u32(bytes,44);
    if(count>65536) throw std::runtime_error("COL5 surface count exceeds diagnostic limit");
    checked_bytes(bytes,offset,std::size_t(count)*16);
    std::vector<std::pair<std::size_t,std::size_t>> regions;
    for(const auto field:{20U,28U,40U}) {
        const auto size=read_u32(bytes,field), start=read_u32(bytes,field+4);
        if(!size)continue;
        if(start<48 || start%16) throw std::runtime_error("COL5 table overlaps header or is unaligned");
        regions.emplace_back(start,std::size_t(start)+std::size_t(size)*16);
    }
    std::sort(regions.begin(),regions.end());
    for(std::size_t i=1;i<regions.size();++i)
        if(regions[i-1].second>regions[i].first) throw std::runtime_error("Overlapping COL5 tables");
    std::vector<FaceCandidate> result;
    result.reserve(mesh.triangles.size());
    for(std::size_t i=0;i<mesh.triangles.size();++i) {
        const auto& t=mesh.triangles[i];
        const auto surface=(t.flags&255U)>>1;
        if(surface>=count) throw std::runtime_error("COL5 triangle surface index outside table");
        FaceCandidate c;
        for(unsigned j=0;j<3;++j)c.vertices[j]=mesh.vertices[t.indices[j]];
        c.normal=scale(cross(sub(c.vertices[1],c.vertices[0]),sub(c.vertices[2],c.vertices[0])),t.coefficient);
        c.flags=bytes[offset+surface*16];
        c.source_id=static_cast<std::uint32_t>(i);
        validate(c);
        result.push_back(c);
    }
    return result;
}

void write_face_result(std::ostream& out, const FaceResult& r, std::span<const FaceCandidate> candidates) {
    const char* status=r.status==FaceResult::Status::none ? "none" :
        r.status==FaceResult::Status::hit ? "hit" : "unsupported_edge_vertex";
    out<<std::setprecision(9)<<"{\"status\":\""<<status<<"\",\"displacement\":";
    vector_json(out,r.displacement);
    out<<",\"normal\":"; if(r.contact) vector_json(out,r.contact->normal); else out<<"null";
    out<<",\"point\":"; if(r.contact) vector_json(out,r.contact->point); else out<<"null";
    out<<",\"source_id\":"; if(r.contact) out<<r.contact->source_id; else out<<"null";
    out<<",\"contact_type\":"; if(r.contact) out<<r.contact->type; else out<<"null";
    out<<",\"unsupported_index\":"; if(r.unsupported_index) out<<*r.unsupported_index; else out<<"null";
    out<<",\"visited\":"<<r.visited<<",\"filtered\":"<<r.filtered<<",\"aabb_pass\":"<<r.aabb_pass;
    out<<",\"normals\":[";
    for(std::size_t i=0;i<candidates.size();++i) { if(i)out<<',';vector_json(out,candidates[i].normal); }
    out<<"]}";
}
}
