#include "float_bits.hpp"
#include "moving_collision_slide.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::size_t maximum=1000000;
float finite(float x){
    if(::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("Moving query: unsupported nonfinite/subnormal arithmetic");
    return x;
}
float add(float a,float b){return finite(finite(a)+finite(b));}
float sub(float a,float b){return finite(finite(a)-finite(b));}
float mul(float a,float b){return finite(finite(a)*finite(b));}
void valid(Vec3 v){finite(v.x);finite(v.y);finite(v.z);}
Vec3 add(Vec3 a,Vec3 b){return {add(a.x,b.x),add(a.y,b.y),add(a.z,b.z)};}
Vec3 sub(Vec3 a,Vec3 b){return {sub(a.x,b.x),sub(a.y,b.y),sub(a.z,b.z)};}
float length_squared(Vec3 v){return add(add(mul(v.x,v.x),mul(v.y,v.y)),mul(v.z,v.z));}
void valid(const MovingSlideMatrix& m){
    for(auto x:m)finite(x);
    if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)
        throw std::runtime_error("Moving query: only affine homogeneous frames supported");
}
RootVector transform(const MovingSlideMatrix& m,const RootVector& v){
    valid(m);for(auto x:v)finite(x);
    if(v[3]!=1)throw std::runtime_error("Moving query: origin W must be one");
    RootVector r{};
    for(std::size_t row=0;row<4;++row){
        r[row]=mul(m[row],v[0]);
        for(std::size_t column=1;column<4;++column)r[row]=add(r[row],mul(m[column*4+row],v[column]));
    }
    if(r[3]!=1)throw std::runtime_error("Moving query: transformed origin lost homogeneous W");
    return r;
}
void validate_state(const SlideQueryState& s){
    valid(s.query.origin);valid(s.query.displacement);finite(s.query.radius);
    finite(s.query.min_start_distance);finite(s.query.min_approach_cosine);
    finite(s.query.min_direct_cosine);finite(s.query.max_end_distance);
    finite(s.vertical_normal_scale);valid(s.displacement);valid(s.direction);
    finite(s.displacement_squared);valid(s.bounds.minimum);valid(s.bounds.maximum);
    for(auto v:s.normal_history)valid(v);
    if(s.contact){valid(s.contact->point);valid(s.contact->normal);}
}
void refresh(SlideQueryState& s){
    const auto v=s.displacement;
    const float squared=length_squared(v);
    // 3bdf20 zero includes W; this API admits only displacement W0.
    if(v.x==0&&v.y==0&&v.z==0)s.direction={};
    else{
        if(squared<=0)throw std::runtime_error("Moving query: undefined direction length");
        // 00132060: VU RSQRT Q = vf0.w/sqrt(|v|^2), then vmulq.xyz.
        const float reciprocal=finite(vu::rsqrt(squared));
        s.direction={mul(v.x,reciprocal),mul(v.y,reciprocal),mul(v.z,reciprocal)};
    }
    s.displacement_squared=squared;
    const auto endpoint=add(s.query.origin,s.displacement);
    const auto p=s.query.origin;const float radius=s.query.radius;
    s.bounds={
        {sub(std::min(p.x,endpoint.x),radius),sub(std::min(p.y,endpoint.y),radius),sub(std::min(p.z,endpoint.z),radius)},
        {add(std::max(p.x,endpoint.x),radius),add(std::max(p.y,endpoint.y),radius),add(std::max(p.z,endpoint.z),radius)}};
}
void move_origin(SlideQueryState& s,Vec3 origin){
    const auto endpoint=add(s.query.origin,s.displacement);
    s.query.origin=origin;s.displacement=sub(endpoint,origin);refresh(s);
}
}
void consume_moving_slide_candidates(SlideQueryState& state,std::span<FaceCandidate> candidates,
    const MovingSlideHost& host,const std::optional<MovingSlideShape>& shape){
    if(!host.component||!host.enabled||!host.relative_matrix)
        throw std::runtime_error("Moving query: complete live source host required");
    if(candidates.size()>maximum||state.visited>maximum-candidates.size())
        throw std::runtime_error("Moving query: candidate visit bound exceeded");
    auto next=state;auto copies=std::vector<FaceCandidate>(candidates.begin(),candidates.end());
    // Validate the retained slide mode and its existing state without consuming a
    // record. Shape data are consumed only if a component transition needs them.
    validate_state(next);consume_slide_candidates(next,{});
    const auto baseline=next.query.origin;MovingSlideComponent previous=0;
    for(auto& candidate:copies){
        if((candidate.flags&next.query.excluded_flags)!=0||
           (candidate.flags&next.query.required_flags)!=next.query.required_flags){
            ++next.visited;++next.filtered;continue;
        }
        const auto component=host.component(candidate.source_id);
        if(component!=previous&&(previous!=0||component==0||host.enabled(component))){
            // Capture the endpoint before any component-matrix read, matching
            // 3bdb10. A late invalid frame still leaves getter effects visible.
            const auto endpoint=add(next.query.origin,next.displacement);
            Vec3 origin=baseline;
            if(component!=0){
                const auto matrix=host.relative_matrix(component);
                const RootVector source=shape?shape->world_origin:RootVector{baseline.x,baseline.y,baseline.z,1};
                // 3bdbbc..3bdc70 transform all four lanes of query +f0 and keep
                // the resulting W in the origin (+e0) and so in the displacement
                // W lane, which later conversions read. Only W 1 is hosted.
                if(source[3]!=1)throw std::runtime_error(
                    "Moving query: a transformed center with W other than 1 at a component-origin change (3bdbbc) is not hosted");
                auto point=transform(matrix,source);
                if(shape)point=transform(shape->world_to_query,point);
                origin={point[0],point[1],point[2]};
            }
            next.query.origin=origin;next.displacement=sub(endpoint,origin);
            refresh(next);previous=component;
        }
        for(auto vertex:candidate.vertices){valid(vertex);}
        valid(candidate.normal);
        consume_slide_candidates(next,std::span<FaceCandidate>(&candidate,1));
        validate_state(next);valid(candidate.normal);
    }
    if(previous!=0)move_origin(next,baseline);
    validate_state(next);
    state=std::move(next);std::copy(copies.begin(),copies.end(),candidates.begin());
}
}
