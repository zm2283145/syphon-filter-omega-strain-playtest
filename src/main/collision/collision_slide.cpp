#include "collision_slide.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::size_t limit=1000000;
float check(float v){if(!std::isfinite(v))throw std::runtime_error("Nonfinite slide-query arithmetic");return v;}
Vec3 valid(Vec3 v){check(v.x);check(v.y);check(v.z);return v;}
Vec3 add(Vec3 a,Vec3 b){return valid({a.x+b.x,a.y+b.y,a.z+b.z});}
Vec3 sub(Vec3 a,Vec3 b){return valid({a.x-b.x,a.y-b.y,a.z-b.z});}
Vec3 scale(Vec3 a,float s){check(s);return valid({a.x*s,a.y*s,a.z*s});}
float dot(Vec3 a,Vec3 b){return check(check(check(a.x*b.x)+check(a.y*b.y))+check(a.z*b.z));}
Vec3 cross(Vec3 a,Vec3 b){return valid({check(a.y*b.z)-check(a.z*b.y),
    check(a.z*b.x)-check(a.x*b.z),check(a.x*b.y)-check(a.y*b.x)});}
bool zero(Vec3 v){return v.x==0&&v.y==0&&v.z==0;}
// FPU results past the binary32 range give +-FLT_MAX; div.s x / 0 gives
// FLT_MAX with the sign of x * 0 (+ for 0 / 0).
float clamped(float v){
    if(std::isnan(v))throw std::runtime_error("Nonfinite slide-query arithmetic");
    return std::isinf(v)?std::copysign(std::numeric_limits<float>::max(),v):v;
}
float fpu_div(float a,float b){
    if(b==0)return std::copysign(std::numeric_limits<float>::max(),a!=0&&std::signbit(a)!=std::signbit(b)?-1.F:1.F);
    return clamped(a/b);
}
// 00132060 / inline at 003bebc4, 003bef84, 003bf0f4, 003bf3b8: vrsqrt
// Q = vf0.w/sqrt(|v|^2), then vmulq.xyz. Float square root, then float
// reciprocal, as the interpreter models VU RSQRT (a double reciprocal rounded
// once differs by an ulp on some inputs).
Vec3 normalize(Vec3 v){
    if(zero(v))return {};
    const float squared=dot(v,v);
    if(squared<=0)throw std::runtime_error("Undefined slide-query normalization");
    return scale(v,check(1.0F/check(std::sqrt(squared))));
}
Bounds box(Vec3 a,Vec3 b,float r){return {
    valid({std::min(a.x,b.x)-r,std::min(a.y,b.y)-r,std::min(a.z,b.z)-r}),
    valid({std::max(a.x,b.x)+r,std::max(a.y,b.y)+r,std::max(a.z,b.z)+r})};}
void refresh(SlideQueryState& s,bool direction){
    if(direction)s.direction=normalize(s.displacement);
    s.displacement_squared=dot(s.displacement,s.displacement);
    s.bounds=box(s.query.origin,add(s.query.origin,s.displacement),s.query.radius);
}
void validate_query(const FaceQuery& q){
    valid(q.origin);valid(q.displacement);check(q.radius);check(q.min_start_distance);
    check(q.min_approach_cosine);check(q.min_direct_cosine);check(q.max_end_distance);
    if(q.radius<0)throw std::runtime_error("Negative slide-query radius");
}
void validate_candidate(const FaceCandidate& c){
    for(auto v:c.vertices){valid(v);}
    valid(c.normal);
    const auto area=cross(sub(c.vertices[1],c.vertices[0]),sub(c.vertices[2],c.vertices[0]));
    // Mapped by the actor body query (003c66b0 loop 003c6d18..003c6e88): the normal is used as
    // stored; the world candidate was plane-checked before mapping (validate_face_candidate).
    // A collinear COL triangle (zero normal, 003c1254) and a mapped candidate whose area or
    // normal rounded away are queried unchecked, as the EE does.
    if(c.query_transformed||(zero(area)&&zero(c.normal)))return;
    if(zero(area)||zero(c.normal))throw std::runtime_error("Degenerate slide-query candidate");
    if(std::abs(dot(normalize(area),normalize(c.normal)))<1.F-.0001F)
        throw std::runtime_error("Slide-query normal differs from triangle plane");
}
bool overlaps(const Bounds& b,const FaceCandidate& c){
    const auto a=c.vertices[0],v=c.vertices[1],w=c.vertices[2];
    return std::max({a.x,v.x,w.x})>=b.minimum.x&&std::min({a.x,v.x,w.x})<=b.maximum.x&&
        std::max({a.y,v.y,w.y})>=b.minimum.y&&std::min({a.y,v.y,w.y})<=b.maximum.y&&
        std::max({a.z,v.z,w.z})>=b.minimum.z&&std::min({a.z,v.z,w.z})<=b.maximum.z;
}
bool contains(const FaceCandidate& c,Vec3 point){
    const auto a=sub(c.vertices[1],c.vertices[0]),b=sub(c.vertices[2],c.vertices[0]),p=sub(point,c.vertices[0]);
    const float x=std::abs(c.normal.x),y=std::abs(c.normal.y),z=std::abs(c.normal.z);
    const int axis=x>y?(z>x?2:0):(y>z?1:2);
    float a0,a1,b0,b1,p0,p1;
    if(axis==2){a0=a.x;a1=a.y;b0=b.x;b1=b.y;p0=p.x;p1=p.y;}
    else if(axis==1){a0=a.z;a1=a.x;b0=b.z;b1=b.x;p0=p.z;p1=p.x;}
    else{a0=a.y;a1=a.z;b0=b.y;b1=b.z;p0=p.y;p1=p.z;}
    const float determinant=check(check(a0*b1)-check(a1*b0));
    // Zero determinant (collinear projection): div.s gives +-FLT_MAX and the
    // products clamp, so u and v have opposite signs unless the point lies on
    // the line's projected coordinate (collision_face.cpp contains).
    const float inverse=fpu_div(1.F,determinant);
    const float u=clamped(inverse*check(check(b1*p0)-check(b0*p1)));
    const float v=clamped(inverse*check(check(p1*a0)-check(p0*a1)));
    return u>=0&&v>=0&&clamped(u+v)<=1;
}
bool corner(const SlideQueryState& s,Vec3 normal){
    if(!s.check_corners)return false;
    for(const auto previous:s.normal_history){
        if(dot(normal,previous)>=0)continue;
        const auto sum=add(normal,previous);
        if(dot(s.direction,sum)>=0||zero(sum))continue;
        if(dot(s.direction,normalize(sum))<-.866F)return true;
    }
    return false;
}
void hit(SlideQueryState& s,const FaceCandidate& c,Vec3 point){
    s.contact=FaceContact{c.normal,point,c.source_id,1};
}
bool edges(SlideQueryState& s,const FaceCandidate& c){
    FaceResult r;r.displacement=s.displacement;
    if(!query_edge_vertices(s.query,r,c))return false;
    s.displacement=r.displacement;s.contact=r.contact;
    refresh(s,false);return true;
}
bool consume(SlideQueryState& s,FaceCandidate& c){
    ++s.visited;
    if((c.flags&s.query.required_flags)!=s.query.required_flags||(c.flags&s.query.excluded_flags)!=0){++s.filtered;return false;}
    if(!overlaps(s.bounds,c))return false;
    ++s.aabb_pass;
    const auto origin=s.query.origin;
    float distance=dot(sub(origin,c.vertices[0]),c.normal);
    if((c.flags&2)!=0&&distance<0){c.normal=scale(c.normal,-1);distance=-distance;}
    if(distance<s.query.min_start_distance)return false;
    const float cosine=-dot(s.direction,c.normal);
    if(cosine<s.query.min_approach_cosine)return false;
    const float approach=-dot(s.displacement,c.normal);
    if(check(distance-approach)>s.query.max_end_distance)return false;
    const bool constrained=corner(s,c.normal);
    if(!((cosine<s.query.min_direct_cosine&&!constrained)||approach<=0)){
        if(!constrained)return false;
        const float fraction=std::clamp(check(check(distance-s.query.radius)/approach),0.F,1.F);
        const auto shortened=scale(s.displacement,fraction);
        const auto point=add(sub(origin,scale(c.normal,s.query.radius)),shortened);
        if(!contains(c,point))return edges(s,c);
        hit(s,c,point);s.displacement=shortened;refresh(s,false);return true;
    }
    const auto desired=add(origin,s.displacement);
    const float end_distance=dot(sub(desired,c.vertices[0]),c.normal);
    const auto projected=sub(desired,scale(c.normal,end_distance));
    if(contains(c,projected)){
        hit(s,c,projected);
        if(cosine>=0&&end_distance<s.query.radius){
            s.displacement=sub(add(projected,scale(c.normal,s.query.radius)),origin);
            refresh(s,true);
        }
        return true;
    }
    if(!edges(s,c))return false;
    const auto normal=s.contact->normal;
    if(corner(s,normal))return true;
    const auto remaining=sub(desired,add(origin,s.displacement));
    s.displacement=sub(sub(desired,scale(normal,dot(remaining,normal))),origin);
    if(s.mode==SlideQueryMode::vertical_projection&&(normal.z>0)==(s.displacement.z>0)){
        auto adjusted=normal;adjusted.z=check(adjusted.z*s.vertical_normal_scale);
        if(!zero(adjusted)){
            adjusted=normalize(adjusted);
            s.displacement=sub(sub(desired,scale(adjusted,dot(remaining,adjusted))),origin);
        }
    }
    refresh(s,true);return true;
}
}
SlideQueryState make_slide_query(const FaceQuery& q,float vertical_normal_scale,bool check_corners,SlideQueryMode mode){
    validate_query(q);check(vertical_normal_scale);
    if(mode!=SlideQueryMode::projection&&mode!=SlideQueryMode::vertical_projection)
        throw std::runtime_error("Slide query supports only recovered modes4/5");
    SlideQueryState state;state.query=q;state.mode=mode;state.vertical_normal_scale=vertical_normal_scale;
    state.check_corners=check_corners;state.displacement=q.displacement;refresh(state,true);return state;
}
void consume_slide_candidates(SlideQueryState& state,std::span<FaceCandidate> candidates){
    if(candidates.size()>limit||state.normal_history.size()>limit||state.visited>limit-candidates.size())
        throw std::runtime_error("Slide-query diagnostic limit exceeded");
    validate_query(state.query);check(state.vertical_normal_scale);valid(state.displacement);valid(state.direction);
    if(state.mode!=SlideQueryMode::projection&&state.mode!=SlideQueryMode::vertical_projection)
        throw std::runtime_error("Slide query supports only recovered modes4/5");
    check(state.displacement_squared);valid(state.bounds.minimum);valid(state.bounds.maximum);
    for(auto n:state.normal_history)valid(n);
    for(const auto& c:candidates)validate_candidate(c);
    auto next=state;std::vector<FaceCandidate> copies(candidates.begin(),candidates.end());
    for(auto& c:copies)if(consume(next,c)){
        if(next.normal_history.size()==limit)throw std::runtime_error("Slide-query contact history limit exceeded");
        next.normal_history.push_back(next.contact->normal);
    }
    state=std::move(next);std::copy(copies.begin(),copies.end(),candidates.begin());
}
}
