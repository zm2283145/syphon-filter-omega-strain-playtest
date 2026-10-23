#include "camera_zoom.hpp"
#include "camera_rig.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float checked(float v) {
    if(!std::isfinite(v)) throw std::runtime_error("Camera zoom requires finite arithmetic");
    return v;
}
float add(float a,float b) { return checked(a+b); }
float sub(float a,float b) { return checked(a-b); }
float mul(float a,float b) { return checked(a*b); }
float divide(float a,float b) {
    if(b==0) throw std::runtime_error("Camera zoom division by zero");
    return checked(a/b);
}
void check(const RootVector& v) { for(float x:v) checked(x); }
float vsqrt(float x) { return checked(std::sqrt(std::fabs(x))); }    // vsqrt takes |x|

const float zoom_scale=std::bit_cast<float>(0x3fce5b5aU);    // 004a3fd0 (boot image) 1.6121628
const float zoom_near=std::bit_cast<float>(0x3d4ccccdU);     // 004a3fb4 (boot image) 0.05
const float zoom_inverse=std::bit_cast<float>(0x3f1ecafbU);  // 004a3fd8 (boot image) 0.6202847
const float zoom_radius=std::bit_cast<float>(0x3fdc28f6U);   // 004a3fc0 (boot image) 1.72
const float sweep_extent_2=std::bit_cast<float>(0x3f400000U);// 004a3fb8 (boot image) 0.75
const float literal_005=std::bit_cast<float>(0x3d4ccccdU);   // lui 0x3d4c / ori 0xcccd
const float flt_max=std::bit_cast<float>(0x7f7fffffU);

void merge(CameraZoomBounds& b,const RootVector& p) {         // vmini.xyz / vmax.xyz
    for(int i=0;i<3;++i) {
        b.minimum[i]=p[i]<b.minimum[i]?p[i]:b.minimum[i];
        b.maximum[i]=p[i]>b.maximum[i]?p[i]:b.maximum[i];
    }
}
CameraZoomBounds empty_bounds() {
    return {{flt_max,flt_max,flt_max,1.0F},{-flt_max,-flt_max,-flt_max,1.0F}};
}
}

RootVector camera_zoom_quaternion(const std::array<RootVector,4>& m) {
    for(int r=0;r<3;++r) check(m[r]);
    const RootVector& r0=m[0];const RootVector& r1=m[1];const RootVector& r2=m[2];
    RootVector q{};
    // vmulax.w / vmadday.w / vmaddz.w; MAC Z|S of W (0x11) selects the other branches.
    const float trace=add(add(r0[0],r1[1]),r2[2]);
    if(trace>0) {
        const float s=vsqrt(add(trace,1.0F));
        q={add(0,sub(r1[2],r2[1])),add(0,sub(r2[0],r0[2])),add(0,sub(r0[1],r1[0])),s};
        const float inverse=divide(1.0F,q[3]);
        for(float& x:q) x=mul(x,0.5F);
        for(int i=0;i<3;++i) q[i]=mul(q[i],inverse);
        return q;
    }
    // vsubay.x / vsubaz.y / vsubax.z then vmadd.xyz with vf0 * vf0 (+0): the
    // MAC sign bits 0x80 / 0x40 / 0x20.
    const float dx=add(sub(r0[0],r1[1]),0.0F),dy=add(sub(r1[1],r2[2]),0.0F),dz=add(sub(r2[2],r0[0]),0.0F);
    int largest;
    if(dx<0) largest=dy<0?2:1;
    else largest=dz<0?0:2;
    if(largest==2) {                                           // 0020d4c8
        const float s=vsqrt(add(sub(dz,r1[1]),1.0F));
        q={add(0,add(r0[2],r2[0])),add(0,add(r2[1],r1[2])),add(0,s),sub(r0[1],r1[0])};
        const float inverse=divide(1.0F,q[2]);
        for(float& x:q) x=mul(x,0.5F);
        q[0]=mul(q[0],inverse);q[1]=mul(q[1],inverse);q[3]=mul(q[3],inverse);
    } else if(largest==1) {                                    // 0020d51c
        const float s=vsqrt(add(sub(dy,r0[0]),1.0F));
        q={add(0,add(r1[0],r0[1])),add(0,s),add(0,add(r2[1],r1[2])),sub(r2[0],r0[2])};
        const float inverse=divide(1.0F,q[1]);
        for(float& x:q) x=mul(x,0.5F);
        q[0]=mul(q[0],inverse);q[2]=mul(q[2],inverse);q[3]=mul(q[3],inverse);
    } else {                                                   // 0020d568
        const float s=vsqrt(add(sub(dx,r2[2]),1.0F));
        q={add(0,s),add(0,add(r1[0],r0[1])),add(0,add(r0[2],r2[0])),sub(r1[2],r2[1])};
        const float inverse=divide(1.0F,q[0]);
        for(float& x:q) x=mul(x,0.5F);
        q[1]=mul(q[1],inverse);q[2]=mul(q[2],inverse);q[3]=mul(q[3],inverse);
    }
    return q;
}

void setup_camera_zoom(CameraZoomBody& b,const std::array<RootVector,4>& m,float fov) {
    checked(fov);check(m[1]);check(m[3]);
    CameraZoomBody n=b;
    n.hit_4=false;
    n.fov_8=fov;
    n.rotation_10=camera_zoom_quaternion(m);
    const float c=divide(1.0F,game_tanf(mul(0.5F,fov)));
    const float scaled=mul(c,zoom_scale);
    n.scale_20=divide(scaled,add(c,zoom_near));
    n.near_24=mul(n.scale_20,zoom_near);
    const float reach=sub(scaled,n.near_24);
    n.eye_30=m[3];
    for(int i=0;i<4;++i) n.reach_40[i]=mul(m[1][i],reach);  // vmulx.xyzw
    b=n;
}

CameraZoomBounds camera_zoom_bounds(const CameraZoomBody& b) {
    check(b.eye_30);check(b.reach_40);checked(b.scale_20);
    CameraZoomBounds out=empty_bounds();
    merge(out,b.eye_30);
    RootVector end{};
    for(int i=0;i<4;++i) end[i]=add(b.eye_30[i],b.reach_40[i]);   // vadd.xyzw
    CameraZoomBounds box=empty_bounds();
    merge(box,end);
    const float r=b.scale_20;
    const float minus_r=sub(0,r);                                    // vmulax ACC 0, vmsubw
    for(int i=0;i<3;++i) {
        box.minimum[i]=add(box.minimum[i],minus_r);
        box.maximum[i]=add(box.maximum[i],r);
    }
    if(box.minimum[0]<=box.maximum[0]) {                             // c.ole.s
        merge(out,box.minimum);
        merge(out,box.maximum);
    }
    const float margin=std::bit_cast<float>(0x3e800000U);            // 0.25
    const float minus_margin=sub(0,margin);
    for(int i=0;i<3;++i) {
        out.minimum[i]=add(out.minimum[i],minus_margin);
        out.maximum[i]=add(out.maximum[i],margin);
    }
    return out;
}

CameraZoomSweep camera_zoom_sweep(const CameraZoomBody& b) {
    check(b.eye_30);check(b.reach_40);checked(b.fov_8);
    CameraZoomSweep s;
    s.eye=b.eye_30;
    s.reach=b.reach_40;
    // 0020cfbc..0020d018: the camera_quaternion_rows block with row 3 = vf0.
    const auto rows=camera_quaternion_rows(b.rotation_10);
    s.rows={rows[0],rows[1],rows[2],RootVector{0,0,0,1}};
    s.extents={1.0F,zoom_near,sweep_extent_2};
    s.scalars={literal_005,literal_005,0.0F,1.0F};
    s.fov=b.fov_8;
    s.radius=zoom_radius;
    return s;
}

void finish_camera_zoom(CameraZoomBody& b,const CameraZoomHit& hit) {
    CameraZoomBody n=b;
    n.hit_4=hit.hit;
    if(hit.hit) {
        const RootVector& v=hit.vector_80;
        for(int i=0;i<3;++i) checked(v[i]);
        // vmul.xyz, then vmulax.w / vmadday.w / vmaddz.w and vsqrt.
        const float d=vsqrt(add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2])));
        const float x=divide(1.0F,mul(add(checked(n.near_24),d),zoom_inverse));
        n.fov_8=mul(2.0F,game_atanf(x));
    }
    b=n;
}

void update_camera_zoom(CameraZoomBody& b,const CameraZoomHost& host) {
    if(!host.gather||!host.sweep)
        throw std::runtime_error("Camera zoom requires the gather (00181260) and the sweep (003bcee0 / 003bced0)");
    const auto bounds=camera_zoom_bounds(b);
    host.gather(bounds);
    const auto request=camera_zoom_sweep(b);
    finish_camera_zoom(b,host.sweep(request));
}

namespace {
using Matrix=std::array<float,16>;      // column-major, as the VU blocks load it
Matrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
// vmulax / vmadday / vmaddaz / vmaddw (00182f30 and the inline blocks).
RootVector transform(const Matrix& m,const RootVector& v) {
    RootVector r{};
    for(unsigned row=0;row<4;++row) {
        float x=mul(m[row],v[0]);
        for(unsigned column=1;column<4;++column) x=add(x,mul(m[column*4+row],v[column]));
        r[row]=x;
    }
    return r;
}
Matrix compose(const Matrix& left,const Matrix& right) {
    Matrix result{};
    for(unsigned column=0;column<4;++column) {
        const auto v=transform(left,{right[column*4],right[column*4+1],right[column*4+2],right[column*4+3]});
        for(unsigned row=0;row<4;++row) result[column*4+row]=v[row];
    }
    return result;
}
Matrix diagonal(float x,float y,float z) {
    Matrix m=identity();
    m[0]=x;m[5]=y;m[10]=z;
    return m;
}
RootVector point(const Vec3& v,float w) { return {v.x,v.y,v.z,w}; }
Vec3 xyz(const RootVector& v) { return {v[0],v[1],v[2]}; }

// The candidate as 003bf760 holds it: vertices with W, the source triangle
// (its coefficient sign) and the port's FaceCandidate fields.
struct Projected {
    std::array<RootVector,3> vertices{};
    Vec3 normal{};
    float sign{};
    FaceCandidate face;
};
// 003bf900 / 003bfab0: f = 1.0 / y (div.s), x *= f, z *= f; Y and W kept.
RootVector project(const RootVector& v) {
    const float f=divide(1.0F,v[1]);
    return {mul(v[0],f),v[1],mul(v[2],f),v[3]};
}
// 003bf900 tail / 003c0020: 00183380 differences, 001320f0 cross (vopmula /
// vopmsub), 0017f670 by the coefficient sign (003c0180), 00132030 RSQRT.
Vec3 projected_normal(const std::array<RootVector,3>& v,float sign) {
    float a[3],b[3];
    for(int i=0;i<3;++i) { a[i]=sub(v[1][i],v[0][i]); b[i]=sub(v[2][i],v[0][i]); }
    float n[3];
    for(int i=0;i<3;++i) {
        const int j=(i+1)%3,k=(i+2)%3;
        n[i]=mul(sub(mul(a[j],b[k]),mul(b[j],a[k])),sign);
    }
    const float squared=add(add(mul(n[0],n[0]),mul(n[1],n[1])),mul(n[2],n[2]));
    // A collinear projection: RSQRT of 0 gives Q = FLT_MAX and 0 * Q = 0.
    if(n[0]==0&&n[1]==0&&n[2]==0) return {n[0],n[1],n[2]};
    if(squared<=0) throw std::runtime_error("Camera zoom sweep: a projected candidate's normal underflows (003c0020 RSQRT)");
    const float reciprocal=checked(vu::rsqrt(squared));
    return {mul(n[0],reciprocal),mul(n[1],reciprocal),mul(n[2],reciprocal)};
}
// 003bfab0 intersection: d = to - from; d.y == 0 gives the zero vector
// (003b26e0); else from + d * ((near - from.y) / d.y), 4 lanes.
RootVector clip_point(const RootVector& from,const RootVector& to,float near) {
    RootVector d{};
    for(int i=0;i<4;++i) d[i]=sub(to[i],from[i]);
    if(d[1]==0) return {};
    const float t=divide(sub(near,from[1]),d[1]);
    RootVector r{};
    for(int i=0;i<4;++i) r[i]=add(from[i],mul(d[i],t));
    return r;
}
// 003bf760(collection, world-to-query, near): candidates from last to first.
void project_candidates(std::vector<Projected>& list,const Matrix& world_to_query,float near) {
    for(std::ptrdiff_t i=static_cast<std::ptrdiff_t>(list.size())-1;i>=0;--i) {
        auto& c=list[static_cast<std::size_t>(i)];
        std::array<RootVector,3> v{};
        for(int k=0;k<3;++k) v[k]=transform(world_to_query,c.vertices[k]);
        if(!(v[0][1]<=near)&&!(v[1][1]<=near)&&!(v[2][1]<=near)) {      // 003c01e0 x3
            for(int k=0;k<3;++k) c.vertices[k]=project(v[k]);               // 003bf900
            c.normal=projected_normal(c.vertices,c.sign);
            continue;
        }
        // 003bfab0: (P, prev, next) = (v2, v1, v0), (v0, v2, v1), (v1, v0, v2).
        std::vector<RootVector> out;
        const int order[3][3]={{2,1,0},{0,2,1},{1,0,2}};
        for(const auto& o:order) {
            const auto& p=v[o[0]];
            if(!(p[1]<=near)) { out.push_back(p); continue; }
            if(!(v[o[1]][1]<=near)) out.push_back(clip_point(p,v[o[1]],near));
            if(!(v[o[2]][1]<=near)) out.push_back(clip_point(p,v[o[2]],near));
        }
        if(out.empty()) {                                                    // last into slot i, 003c01d0
            c=list.back();
            list.pop_back();
            continue;
        }
        if(out.size()<3) continue;
        for(auto& p:out) p=project(p);
        c.vertices={out[0],out[1],out[2]};
        c.normal=projected_normal(c.vertices,c.sign);
        if(out.size()==4) {                                                  // 00139b00 copy, 003bfff0 append
            Projected copy=c;
            copy.vertices={out[0],out[2],out[3]};
            copy.normal=projected_normal(copy.vertices,copy.sign);
            list.push_back(copy);
        }
    }
}
}

CameraZoomHit sweep_camera_zoom(const CameraZoomSweep& s,std::span<const CameraZoomCandidate> world,
    std::vector<FaceCandidate>* projected) {
    check(s.eye);check(s.reach);
    for(const auto& r:s.rows) check(r);
    for(float x:s.extents) checked(x);
    for(float x:s.scalars) checked(x);
    checked(s.fov);checked(s.radius);
    // 003c6fc0: placement = rows 0..2 (with W) and the eye (W = 1); scale and
    // its reciprocal from t2; 00133200 inverts the placement.
    Matrix placement=identity();
    for(unsigned c=0;c<3;++c) for(unsigned r=0;r<4;++r) placement[c*4+r]=s.rows[c][r];
    placement[12]=s.eye[0];placement[13]=s.eye[1];placement[14]=s.eye[2];placement[15]=1;
    Matrix inverse=identity();
    for(unsigned c=0;c<3;++c) for(unsigned r=0;r<3;++r) inverse[c*4+r]=placement[r*4+c];
    if(s.eye[0]!=0||s.eye[1]!=0||s.eye[2]!=0) {
        const float negated[3]={sub(0,s.eye[0]),sub(0,s.eye[1]),sub(0,s.eye[2])};
        for(unsigned row=0;row<3;++row)
            inverse[12+row]=add(add(mul(inverse[row],negated[0]),mul(inverse[4+row],negated[1])),
                mul(inverse[8+row],negated[2]));
    }
    const Matrix scale=diagonal(s.extents[0],s.extents[1],s.extents[2]);
    const Matrix inverse_scale=diagonal(divide(1,s.extents[0]),divide(1,s.extents[1]),divide(1,s.extents[2]));
    Matrix query_to_world=compose(placement,scale);                       // +120
    Matrix world_to_query=compose(inverse_scale,inverse);                 // +1a0
    if(!(s.fov<=0)) {                                                     // c.ole.s fov, 0
        const float f=mul(game_tanf(mul(0.5F,s.fov)),s.extents[1]);        // 00121a10 * t2[1]
        const float g=divide(1.0F,f);
        query_to_world=compose(query_to_world,diagonal(f,1,f));
        world_to_query=compose(diagonal(g,1,g),world_to_query);
    }
    const float near=transform(inverse_scale,{0,s.radius,0,0})[1];
    std::vector<Projected> list;
    list.reserve(world.size()*2);
    for(const auto& w:world) {
        validate_face_candidate(w.face);
        Projected p;
        for(int k=0;k<3;++k) p.vertices[k]=point(w.face.vertices[k],1);
        // 003c0180: c.ole.s coefficient, 0. The EE FPU reads exponent 255 as a number (the
        // church COL record holds 0x7f800000) and an exponent-0 operand as zero.
        const auto bits=std::bit_cast<std::uint32_t>(w.coefficient);
        p.sign=(bits>>31)!=0||((bits>>23)&0xffU)==0?-1.0F:1.0F;
        p.face=w.face;
        list.push_back(p);
    }
    project_candidates(list,world_to_query,near);
    std::vector<FaceCandidate> candidates;
    candidates.reserve(list.size());
    for(const auto& p:list) {
        FaceCandidate c=p.face;
        for(int k=0;k<3;++k) c.vertices[k]=xyz(p.vertices[k]);
        c.normal=p.normal;
        c.query_transformed=true;
        candidates.push_back(c);
    }
    if(projected) *projected=candidates;
    // 003bf540: origin 0; the reach (W = 0) into the query frame, X and Z
    // over Y; radius f12 = 1.0, cosines .05 / .05, start 0, end 1.0.
    RootVector reach=transform(world_to_query,{s.reach[0],s.reach[1],s.reach[2],0});
    const float f=divide(1.0F,reach[1]);
    reach[0]=mul(reach[0],f);reach[2]=mul(reach[2],f);
    FaceQuery q;
    q.origin={};
    q.displacement=xyz(reach);
    q.radius=1.0F;
    q.min_approach_cosine=s.scalars[0];
    q.min_direct_cosine=s.scalars[1];
    q.min_start_distance=s.scalars[2];
    q.max_end_distance=s.scalars[3];
    q.required_flags=0;q.excluded_flags=0;
    FaceResult result;
    try{result=query_faces(q,candidates,true);}
    catch(const std::runtime_error& error){
        throw std::runtime_error(std::string("Camera zoom sweep (003c6fc0 -> 003bd900): ")+error.what());
    }
    if(result.status==FaceResult::Status::unsupported_edge_vertex)
        throw std::runtime_error("Camera zoom sweep: incomplete edge/vertex result");
    // 003c3220 with +11a: +80 = +70; X and Z times Y; +120 times the vector.
    RootVector v=point(result.displacement,0);
    v[0]=mul(v[0],v[1]);
    v[2]=mul(v[2],v[1]);
    CameraZoomHit hit;
    hit.hit=result.status==FaceResult::Status::hit;
    hit.vector_80=transform(query_to_world,v);
    return hit;
}

}
