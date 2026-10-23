#include "camera_collision.hpp"
#include "collision_face.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
float check(float v) {
    if(!std::isfinite(v)) throw std::runtime_error("Camera collision requires finite arithmetic");
    return v;
}
float add(float a,float b) { return check(a+b); }
float sub(float a,float b) { return check(a-b); }
float mul(float a,float b) { return check(a*b); }
float divide(float a,float b) {
    if(b==0) throw std::runtime_error("Camera collision division by zero");
    return check(a/b);
}
const float body_radius=std::bit_cast<float>(0x3fdc28f6U);      // 004a3f60 / 004a3f88 = 1.72
const Vec3 body_scale{1.0F,0.5F,std::bit_cast<float>(0x3f75c28fU)}; // 004a3f70..78
const float approach=std::bit_cast<float>(0x3d4ccccdU);         // 0.05

CollisionMatrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
RootVector transform(const CollisionMatrix& m,const RootVector& v) {
    RootVector r{};
    for(unsigned row=0;row<4;++row) {
        float x=mul(m[row],v[0]);
        for(unsigned column=1;column<4;++column) x=add(x,mul(m[column*4+row],v[column]));
        r[row]=x;
    }
    return r;
}
CollisionMatrix compose(const CollisionMatrix& left,const CollisionMatrix& right) {
    CollisionMatrix result{};
    for(unsigned column=0;column<4;++column) {
        const auto v=transform(left,{right[column*4],right[column*4+1],right[column*4+2],right[column*4+3]});
        std::copy(v.begin(),v.end(),result.begin()+column*4);
    }
    return result;
}
Vec3 transform3(const CollisionMatrix& m,Vec3 v,float w) {
    const auto r=transform(m,{v.x,v.y,v.z,w});
    return {r[0],r[1],r[2]};
}
// Candidate normal into the query frame, 003c66b0 at 003c6e44: VU RSQRT
// Q = vf0.w/sqrt(|v|^2), then vmulq.xyz. RSQRT of 0 gives Q = FLT_MAX, so a
// collinear candidate's zero normal stays zero.
Vec3 normalize(Vec3 v) {
    if(v.x==0&&v.y==0&&v.z==0) return v;
    const float squared=add(add(mul(v.x,v.x),mul(v.y,v.y)),mul(v.z,v.z));
    if(squared<=0) throw std::runtime_error("Undefined camera collision normal");
    const float reciprocal=check(vu::rsqrt(squared));
    return {mul(v.x,reciprocal),mul(v.y,reciprocal),mul(v.z,reciprocal)};
}
RootVector add4(const RootVector& a,const RootVector& b) {
    return {add(a[0],b[0]),add(a[1],b[1]),add(a[2],b[2]),add(a[3],b[3])};
}
RootVector sub4(const RootVector& a,const RootVector& b) {
    return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};
}
RootVector scaled(const RootVector& v,float s) { return {mul(v[0],s),mul(v[1],s),mul(v[2],s),mul(v[3],s)}; }

struct Sweep { bool hit{}; RootVector displacement{}; };

// 003bf750 -> 003c66b0 with t0 = 0, then 003c3450(query, 0, 0).
Sweep sweep(const std::array<RootVector,3>& basis,const RootVector& origin,const RootVector& displacement,
    std::span<const FaceCandidate> world) {
    // Placement: basis rows as columns, origin (W = 1) as translation.
    CollisionMatrix placement=identity();
    for(unsigned c=0;c<3;++c) for(unsigned r=0;r<4;++r) placement[c*4+r]=basis[c][r];
    placement[12]=origin[0];placement[13]=origin[1];placement[14]=origin[2];placement[15]=1;
    // 00133200: transposed basis; translation from three ordered products of
    // the negated origin unless the origin is zero.
    CollisionMatrix inverse=identity();
    for(unsigned c=0;c<3;++c) for(unsigned r=0;r<3;++r) inverse[c*4+r]=placement[r*4+c];
    if(origin[0]!=0||origin[1]!=0||origin[2]!=0) {
        const RootVector negated{sub(0,origin[0]),sub(0,origin[1]),sub(0,origin[2]),1};
        for(unsigned row=0;row<3;++row)
            inverse[12+row]=add(add(mul(inverse[row],negated[0]),mul(inverse[4+row],negated[1])),
                mul(inverse[8+row],negated[2]));
    }
    CollisionMatrix scale=identity(),inverse_scale=identity();
    scale[0]=body_scale.x;scale[5]=body_scale.y;scale[10]=body_scale.z;
    inverse_scale[0]=divide(1,body_scale.x);inverse_scale[5]=divide(1,body_scale.y);inverse_scale[10]=divide(1,body_scale.z);
    const auto query_to_world=compose(placement,scale);
    const auto world_to_query=compose(inverse_scale,inverse);
    const auto normal_to_query=compose(scale,inverse);
    std::vector<FaceCandidate> candidates(world.begin(),world.end());
    for(auto& c:candidates) {
        validate_face_candidate(c);
        for(auto& v:c.vertices) v=transform3(world_to_query,v,1);
        c.normal=normalize(transform3(normal_to_query,c.normal,0));
        c.query_transformed=true;
    }
    FaceQuery q;
    q.origin={};
    q.displacement=transform3(world_to_query,{displacement[0],displacement[1],displacement[2]},0);
    q.radius=body_radius;
    q.min_start_distance=0;
    q.min_approach_cosine=approach;
    q.min_direct_cosine=approach;
    q.max_end_distance=body_radius;
    q.required_flags=0;q.excluded_flags=0;
    FaceResult result;
    try{result=query_faces(q,candidates,true);}
    catch(const std::runtime_error& error){
        throw std::runtime_error(std::string("Camera collision sweep (003bf750 -> 003c3450): ")+error.what());
    }
    if(result.status==FaceResult::Status::unsupported_edge_vertex)
        throw std::runtime_error("Camera collision: incomplete edge/vertex result");
    const Vec3 world_displacement=transform3(query_to_world,result.displacement,0);
    Sweep s;
    s.hit=result.status==FaceResult::Status::hit;
    s.displacement={world_displacement.x,world_displacement.y,world_displacement.z,0};
    return s;
}
}

Bounds camera_body_bounds(const CameraBody& body) {
    constexpr float big=std::numeric_limits<float>::max();
    RootVector minimum{big,big,big,1},maximum{-big,-big,-big,1};
    const RootVector forward=scaled(body.forward_10,body_radius);
    auto include=[&](const RootVector& p) {
        for(int i=0;i<3;++i) { minimum[i]=std::min(minimum[i],p[i]); maximum[i]=std::max(maximum[i],p[i]); }
    };
    for(const auto& p:body.points) include(add4(p,forward));
    include(body.look_40);
    include(body.pivot_50);
    const float margin=add(body_radius,0.25F);
    for(int i=0;i<3;++i) { minimum[i]=add(minimum[i],sub(0,margin)); maximum[i]=add(maximum[i],margin); }
    return {{minimum[0],minimum[1],minimum[2]},{maximum[0],maximum[1],maximum[2]}};
}

CameraCollisionResult limit_camera_point(const CameraBody& body,const RootVector& point,
    std::span<const FaceCandidate> world) {
    const auto basis=camera_quaternion_rows(body.orientation_20);
    const RootVector forward=scaled(body.forward_10,body_radius);
    CameraCollisionResult out{point,false};
    // Query A: look target -> pivot (body +b0 is zero).
    RootVector pivot=body.pivot_50;
    const auto a=sweep(basis,body.look_40,sub4(body.pivot_50,body.look_40),world);
    if(a.hit) { out.hit=true; pivot=add4(body.look_40,a.displacement); }
    // Query B: pivot -> point pushed forward.
    const RootVector forward_point=add4(point,forward);
    const auto b=sweep(basis,pivot,sub4(forward_point,pivot),world);
    if(b.hit) { out.hit=true; out.point=sub4(add4(pivot,b.displacement),forward); }
    return out;
}
}
