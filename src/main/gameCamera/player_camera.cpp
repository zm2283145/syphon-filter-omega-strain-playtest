#include "player_camera.hpp"
#include "game_libm.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float checked(float v) {
    if(!std::isfinite(v)) throw std::runtime_error("Player camera requires finite arithmetic");
    return v;
}
float add(float a,float b) { return checked(a+b); }
float sub(float a,float b) { return checked(a-b); }
float mul(float a,float b) { return checked(a*b); }
float divide(float a,float b) {
    if(b==0) throw std::runtime_error("Player camera division by zero");
    return checked(a/b);
}
void check(const RootVector& v) { for(float x:v) checked(x); }
constexpr float half=0.5F;
const float pi=std::bit_cast<float>(0x40490fdbU);
const float two_pi=std::bit_cast<float>(0x40c90fdbU);
const float pitch_limit=std::bit_cast<float>(0x3f9c61aaU);      // 004a9318, 70 degrees
const float aim_pitch_limit=std::bit_cast<float>(0x3fc49809U);  // 004a9320, 88 degrees
const float transition=std::bit_cast<float>(0x3f2b851fU);        // 0.67
const float heading_time=std::bit_cast<float>(0x3e428f5cU);      // 0.19
const float aim_exit_time=std::bit_cast<float>(0x3ea8f5c3U);     // 0.33 (0027dcd0)
const float lock_time=std::bit_cast<float>(0x3e75c28fU);         // 004a9330, 0.24
const float lock_hold=std::bit_cast<float>(0x3e000000U);         // 0.125 (00280f94)

RootVector normalize4(const RootVector& q) {
    // 001b13b4: vrsqrt of the 4D squared length.
    const float d=add(add(add(mul(q[0],q[0]),mul(q[1],q[1])),mul(q[2],q[2])),mul(q[3],q[3]));
    if(!(d>0)) throw std::runtime_error("Player camera target quaternion is zero");
    const float r=checked(1.0F/std::sqrt(d));
    return {mul(q[0],r),mul(q[1],r),mul(q[2],r),mul(q[3],r)};
}

// 0027f7e0 with the actor's +f8 floor plane: pitch from the slope along the
// facing direction, else the configured pitch.
float slope_pitch(float base,const RootVector& q,const PlayerCameraTarget& t) {
    if(!(t.physical&&t.plane_valid_e8)) return base;
    const RootVector forward=camera_rotate(q,{0,1,0,0});
    RootVector n=t.plane_f0;n[3]=0;
    // 0027f9c0: v - n * dot3(v, n), W kept.
    const float d=add(add(mul(forward[0],n[0]),mul(forward[1],n[1])),mul(forward[2],n[2]));
    RootVector p{sub(forward[0],mul(n[0],d)),sub(forward[1],mul(n[1],d)),sub(forward[2],mul(n[2],d)),forward[3]};
    const float length=checked(std::sqrt(add(add(mul(p[0],p[0]),mul(p[1],p[1])),0.0F)));
    float a=checked(std::atan2(p[2],length));
    a= a<=0 ? mul(a,std::bit_cast<float>(0x3f4ccccdU)) : mul(a,0.5F);
    return add(a,base);
}
}

namespace {
// vu: the 0027d808 / 0027ecb0 VU blocks move three lanes through
// vadd{x,y,z} vf9, vf0 (+0 + v, so -0 becomes +0); the fourth goes through
// vmul vf0.w (1 * v, sign kept).
float vu_lane(bool vu,float v) { return vu && v==0.0F ? 0.0F : v; }
RootVector matrix_to_quaternion(const std::array<RootVector,3>& m,bool vu) {
    for(const auto& r:m) check(r);
    const float trace=add(add(m[0][0],m[1][1]),m[2][2]);
    RootVector q{};
    if(trace>0) {
        const float s=checked(std::sqrt(add(trace,1.0F)));
        const float inverse=divide(1.0F,s);
        q={mul(mul(vu_lane(vu,sub(m[1][2],m[2][1])),half),inverse),mul(mul(vu_lane(vu,sub(m[2][0],m[0][2])),half),inverse),
           mul(mul(vu_lane(vu,sub(m[0][1],m[1][0])),half),inverse),mul(s,half)};
        return q;
    }
    const float xy=sub(m[0][0],m[1][1]),yz=sub(m[1][1],m[2][2]),zx=sub(m[2][2],m[0][0]);
    const bool x_below_y=xy<0, y_below_z=yz<0, z_below_x=zx<0;
    int largest;
    if(x_below_y) largest=y_below_z ? 2 : 1;
    else largest=z_below_x ? 0 : 2;
    if(largest==2) {
        const float s=checked(std::sqrt(add(sub(zx,m[1][1]),1.0F)));
        const float inverse=divide(1.0F,s);
        q={mul(mul(vu_lane(vu,add(m[0][2],m[2][0])),half),inverse),mul(mul(vu_lane(vu,add(m[2][1],m[1][2])),half),inverse),
           mul(s,half),mul(mul(sub(m[0][1],m[1][0]),half),inverse)};
    } else if(largest==1) {
        const float s=checked(std::sqrt(add(sub(yz,m[0][0]),1.0F)));
        const float inverse=divide(1.0F,s);
        q={mul(mul(vu_lane(vu,add(m[1][0],m[0][1])),half),inverse),mul(s,half),
           mul(mul(vu_lane(vu,add(m[2][1],m[1][2])),half),inverse),mul(mul(sub(m[2][0],m[0][2]),half),inverse)};
    } else {
        const float s=checked(std::sqrt(add(sub(xy,m[2][2]),1.0F)));
        const float inverse=divide(1.0F,s);
        q={mul(s,half),mul(mul(vu_lane(vu,add(m[1][0],m[0][1])),half),inverse),
           mul(mul(vu_lane(vu,add(m[0][2],m[2][0])),half),inverse),mul(mul(sub(m[1][2],m[2][1]),half),inverse)};
    }
    return q;
}
}

RootVector camera_matrix_to_quaternion(const std::array<RootVector,3>& m) { return matrix_to_quaternion(m,false); }
RootVector camera_vu_matrix_to_quaternion(const std::array<RootVector,3>& m) { return matrix_to_quaternion(m,true); }

RootVector camera_axis_quaternion(const RootVector& axis,float angle) {
    const float h=mul(angle,half);
    const float c=checked(std::cos(h)),s=checked(std::sin(h));
    return {mul(s,axis[0]),mul(s,axis[1]),mul(s,axis[2]),c};
}

namespace {
using Rows=std::array<RootVector,3>;
float game_cos(float x) { return checked(game_cosf(checked(x))); }
float game_sin(float x) { return checked(game_sinf(checked(x))); }
// 00140e20(out, f12..f19, stack word): rows (f12, f15, f18), (f13, f16, f19),
// (f14, f17, stack), W = 0.
Rows matrix_140e20(float f12,float f13,float f14,float f15,float f16,float f17,float f18,float f19,float f20) {
    return {RootVector{f12,f15,f18,0},RootVector{f13,f16,f19,0},RootVector{f14,f17,f20,0}};
}
// 00281384 / 00281428: out[i] = b[0] * a[i].x + b[1] * a[i].y + b[2] * a[i].z
// (vmulax / vmadday / vmaddz, each product rounded).
Rows rows_product(const Rows& a,const Rows& b) {
    Rows out{};
    for(int i=0;i<3;++i)
        for(int k=0;k<4;++k)
            out[i][k]=add(add(mul(b[0][k],a[i][0]),mul(b[1][k],a[i][1])),mul(b[2][k],a[i][2]));
    return out;
}
}

RootVector camera_matrix_euler(const Rows& m) {
    for(const auto& r:m) check(r);
    // The flag byte 004a9340 is 1 (static data), so every angle is negated.
    const float z=-game_atan2f(-m[0][1],m[0][0]);
    const float length=checked(std::sqrt(add(mul(m[0][0],m[0][0]),mul(m[0][1],m[0][1]))));
    const float y=-game_atan2f(m[0][2],length);
    const float s1=game_sin(-z),c1=game_cos(-z);
    const Rows m1=matrix_140e20(c1,-s1,0,s1,c1,0,0,0,1);
    const float s2=game_sin(-y),c2=game_cos(-y);
    const Rows m2=matrix_140e20(c2,0,s2,0,1,0,-s2,0,c2);
    const Rows p=rows_product(m1,m2);
    const Rows q=rows_product(m,p);
    const float x=-game_atan2f(-q[1][2],q[1][1]);
    return {checked(x),checked(y),checked(z),0};
}

RootVector camera_matrix_euler_1bd590(const Rows& m) {
    for(const auto& r:m) check(r);
    // The flag byte 0049e9a0 is 0 (static data, no writer): no negation.
    const float z=checked(game_atan2f(-m[1][0],m[1][1]));
    const float length=checked(std::sqrt(add(mul(m[1][1],m[1][1]),mul(m[1][0],m[1][0]))));
    const float x=checked(game_atan2f(m[1][2],length));
    const float s1=game_sin(-z),c1=game_cos(-z);
    const Rows m1=matrix_140e20(c1,-s1,0,s1,c1,0,0,0,1);
    const float s2=game_sin(-x),c2=game_cos(-x);
    const Rows m2=matrix_140e20(1,0,0,0,c2,-s2,0,s2,c2);
    const Rows p=rows_product(m1,m2);
    const Rows q=rows_product(m,p);
    const float y=checked(game_atan2f(-q[0][2],q[0][0]));
    return {x,y,z,0};
}

namespace {
// 0027ec0c..0027ee68 / 0027efa8..0027f1e8: 00140e20 from cos/sin (00140e80 /
// 00140e70) of X, Y, Z, then the VU matrix-to-quaternion block.
RootVector euler_rows_quaternion(float x,float y,float z) {
    const float cx=game_cos(x),sx=game_sin(x);
    const float cy=game_cos(y),sy=game_sin(y);
    const float cz=game_cos(z),sz=game_sin(z);
    const float f4=mul(sz,cx),f3=mul(sz,sx),f15=mul(sz,cy),stack=mul(cy,cx),f1=mul(cz,sx);
    const float f13=sub(mul(f1,sy),f4);
    const float f17=sub(mul(f4,sy),f1);
    const float f2=mul(cz,cx);
    const float f14=add(f3,mul(f2,sy));
    const float f16=add(f2,mul(f3,sy));
    const float f12=mul(cz,cy),f19=mul(cy,sx),f18=-sy;
    return camera_vu_matrix_to_quaternion(matrix_140e20(f12,f13,f14,f15,f16,f17,f18,f19,stack));
}
// 0027ffc0.
float wrap_angle(float v) {
    for(;;) {
        if(!(v<=pi)) { v=add(v,-two_pi); continue; }
        if(v<-pi) { v=add(v,two_pi); continue; }
        return v;
    }
}
}

RootVector camera_aim_orientation(const RootVector& q,float yaw_d78) {
    checked(yaw_d78);
    const RootVector e=camera_matrix_euler(camera_quaternion_rows(q));
    return euler_rows_quaternion(e[0],e[1],yaw_d78);
}

RootVector camera_pick_orientation(const RootVector& q,std::uint8_t& hold_d80,std::uint8_t& hold_d81,
    std::uint64_t lock_d04,std::int32_t lock_id_d08,std::int32_t subtarget_d0c,
    const std::optional<RootVector>& node_15,const std::array<RootVector,4>& transform_50,const PlayerCameraLookAtHost* host) {
    check(q);
    // 0027fa70..0027fbc0: the same validity tests as 00280070, whose result
    // (always 1 here) 0027fbf0 ignores.
    const auto look=camera_lock_look_at(lock_d04,lock_id_d08,subtarget_d0c,node_15,host);
    if(!look) return q;
    // 0027fbf8: the +50 transform (refresh 00132900 only clears its dirty byte).
    if(transform_50[0]==RootVector{}&&transform_50[1]==RootVector{}&&transform_50[2]==RootVector{})
        throw std::runtime_error("Player camera: 0027fa40 reads the +50 transform before the camera's first update (not modelled)");
    const RootVector e=camera_matrix_euler_1bd590({transform_50[0],transform_50[1],transform_50[2]});
    // 0027fc40..0027fcdc: d = look - e, each wrapped into [-pi, pi].
    RootVector d{wrap_angle(sub((*look)[0],e[0])),wrap_angle(sub((*look)[1],e[1])),wrap_angle(sub((*look)[2],e[2])),0};
    // 0027fce0..0027fde4: the hold bytes, with hysteresis.
    if(hold_d80) hold_d80=!(d[0]<=0.0F)&&d[0]<std::bit_cast<float>(0x3eb2b8c2U);
    else hold_d80=!(d[0]<=std::bit_cast<float>(0xbe32b8c2U))&&d[0]<std::bit_cast<float>(0x3f060a92U);
    hold_d81=std::fabs(d[2])<std::bit_cast<float>(hold_d81 ? 0x3f060a92U : 0x3f860a92U);
    if(hold_d80) d[0]=0.0F;                    // 0027fdf4
    if(hold_d81) d[2]=0.0F;                    // 0027fe04
    // 0027fe08..0027ff24.
    return camera_game_euler_quaternion(add(e[0],d[0]),add(e[1],d[1]),add(e[2],d[2]));
}

PlayerCameraPick camera_pick_frame(const RootVector& q) {
    // 0027ee98..0027f214: Euler angles of the frame (00281220); +d70 = X and
    // sp+510 its pitch quaternion; X = 0 and the frame is rebuilt.
    const RootVector e=camera_matrix_euler(camera_quaternion_rows(q));
    return {euler_rows_quaternion(0.0F,e[1],e[2]),camera_aim_pitch_quaternion(e[0]),e[0]};
}

RootVector camera_aim_pitch_quaternion(float pitch_d70) {
    const float h=mul(half,checked(pitch_d70));
    const float c=game_cos(h),s=game_sin(h);
    return {mul(s,1.0F),mul(s,0.0F),mul(s,0.0F),c};
}

namespace {
// VU results flush denormals to signed zero.
float vu_ftz(float v) { return v!=0.0F&&std::fabs(v)<std::numeric_limits<float>::min() ? std::copysign(0.0F,v) : v; }
}

std::optional<RootVector> camera_lock_look_at(std::uint64_t lock_d04,std::int32_t lock_id_d08,
    std::int32_t subtarget_d0c,const std::optional<RootVector>& origin,const PlayerCameraLookAtHost* host) {
    // 0028009c..002800f0: +d04 null.
    if(lock_d04==0) return std::nullopt;
    if(!host||!host->id_c||!host->transform_50||!host->root_bytes||!host->eye_c0)
        throw std::runtime_error("Player camera: 00280070 needs the locked object's +c, +50, root bytes and virtual +c0 (look-at host not supplied)");
    // 002800b4: the handle is valid while the object's +c equals +d08.
    if(host->id_c(lock_d04)!=lock_id_d08) return std::nullopt;
    // 0028014c: object +50.
    if(!host->transform_50(lock_d04)) return std::nullopt;
    // 002801b0..002801f0: +2d and +2c set, +2e clear.
    const auto r=host->root_bytes(lock_d04);
    if(!(r[1]!=0&&r[0]!=0&&r[2]==0)) return std::nullopt;
    // 00280248: virtual +c0(sp+b0, object, +d0c).
    const RootVector eye=host->eye_c0(lock_d04,subtarget_d0c);
    check(eye);
    // 00280264 (t0 != 0): the origin qword.
    if(!origin) throw std::runtime_error("Player camera: 00280070 reads the +50 transform's translation before the camera's first update (not modelled)");
    check(*origin);
    // 002802cc vsub.xyzw; sp+70 = (d.x, d.y, 0, 0).
    RootVector d{};
    for(std::size_t k=0;k<4;++k) d[k]=vu_ftz(sub(eye[k],(*origin)[k]));
    // 00280310..00280328: vmul.xyz, ACC.w = 1 * x2, += 1 * y2, + 1 * (0 * 0); vsqrt; vaddq.x.
    const float x2=vu_ftz(mul(d[0],d[0])),y2=vu_ftz(mul(d[1],d[1]));
    const float sum=vu_ftz(add(vu_ftz(add(vu_ftz(mul(1.0F,x2)),mul(1.0F,y2))),mul(1.0F,0.0F)));
    const float length=vu_ftz(add(0.0F,vu_ftz(checked(std::sqrt(std::fabs(sum))))));
    // 00280334 / 0028034c: out[0] = atan2f(d.z, length), out[1] = 0, out[2] = atan2f(-d.x, d.y).
    const float pitch=checked(game_atan2f(d[2],length));
    const float yaw=checked(game_atan2f(-d[0],d[1]));
    return RootVector{pitch,0.0F,yaw,0.0F};
}

RootVector camera_recoil_rotation(const RootVector& q) {
    check(q);
    // 001ca840..001ca870: c.ole.s |1 - q.w|, 0x33d6bf95 -> out = q.
    if(std::fabs(sub(1.0F,q[3]))<=std::bit_cast<float>(0x33d6bf95U)) return q;
    // 001ca8dc..001ca8e4: ACC = I + 0 (vaddax), ACC += q * 0.35, vf4 = ACC - I * 0.35.
    const float k=std::bit_cast<float>(0x3eb33333U);
    const RootVector identity{0,0,0,1};
    RootVector b{};
    for(std::size_t i=0;i<4;++i) {
        const float acc=vu_ftz(add(vu_ftz(add(identity[i],0.0F)),mul(q[i],k)));
        b[i]=vu_ftz(sub(acc,mul(identity[i],k)));
    }
    // 001ca8e8..001ca904: vmul; ACC.w = 1 * x2 + 1 * y2 + 1 * z2, vf6.w = ACC + 1 * w2;
    // vrsqrt Q = 1 / float(sqrt(|vf6.w|)); vmulq.
    RootVector s{};
    for(std::size_t i=0;i<4;++i) s[i]=vu_ftz(mul(b[i],b[i]));
    float sum=vu_ftz(mul(1.0F,s[0]));
    sum=vu_ftz(add(sum,mul(1.0F,s[1])));
    sum=vu_ftz(add(sum,mul(1.0F,s[2])));
    sum=vu_ftz(add(sum,mul(1.0F,s[3])));
    const float root=static_cast<float>(std::sqrt(static_cast<double>(std::fabs(sum))));
    if(root==0) throw std::runtime_error("Player camera: 001ca840 normalises a zero blend (vrsqrt of 0 is not modelled)");
    const float r=vu_ftz(checked(1.0F/root));
    RootVector n{};
    for(std::size_t i=0;i<4;++i) n[i]=vu_ftz(mul(b[i],r));
    // 001ca928..001ca950: q * n (vmul; vopmula / vmaddaw / vmaddaw / vopmsub
    // for x, y, z; vsubaz / vmsubay / vmsubx for w).
    const auto& a=q;const auto& b2=n;
    RootVector p3{};
    for(std::size_t i=0;i<4;++i) p3[i]=vu_ftz(mul(a[i],b2[i]));
    RootVector out{};
    for(std::size_t i=0;i<3;++i) {
        const std::size_t j=(i+1)%3,l=(i+2)%3;
        float acc=vu_ftz(mul(a[j],b2[l]));
        acc=vu_ftz(add(acc,mul(a[i],b2[3])));
        acc=vu_ftz(add(acc,mul(b2[i],a[3])));
        out[i]=vu_ftz(sub(acc,mul(b2[j],a[l])));
    }
    float w=vu_ftz(sub(p3[3],p3[2]));
    w=vu_ftz(sub(w,mul(1.0F,p3[1])));
    out[3]=vu_ftz(sub(w,mul(1.0F,p3[0])));
    return out;
}

std::array<RootVector,4> camera_rotate_transform(const std::array<RootVector,4>& m,const RootVector& d90) {
    for(const auto& row:m) check(row);
    // 00280dac..00280e0c: the rotation rows of +d90 (vf8..vf10); row 3 is vf0.
    const RootVector& q=d90;check(q);
    RootVector d{},x{},y{},z{};
    for(std::size_t k=0;k<4;++k) d[k]=vu_ftz(add(q[k],q[k]));
    for(std::size_t k=0;k<4;++k) {
        x[k]=vu_ftz(mul(q[k],d[0]));y[k]=vu_ftz(mul(q[k],d[1]));z[k]=vu_ftz(mul(q[k],d[2]));
    }
    const auto ms=[](float a,float b,float c) { return vu_ftz(sub(vu_ftz(sub(a,b)),c)); };
    const std::array<RootVector,3> r{{
        {ms(1.0F,y[1],z[2]),vu_ftz(add(x[1],z[3])),vu_ftz(sub(x[2],y[3])),0},
        {vu_ftz(sub(y[0],z[3])),ms(1.0F,x[0],z[2]),vu_ftz(add(y[2],x[3])),0},
        {vu_ftz(add(z[0],y[3])),vu_ftz(sub(z[1],x[3])),ms(1.0F,x[0],y[1]),0}}};
    std::array<RootVector,4> out{};
    for(std::size_t i=0;i<3;++i)
        for(std::size_t k=0;k<4;++k) {
            // 00280e7c..00280e9c: vmulax / vmadday / vmaddz.
            float acc=vu_ftz(mul(m[0][k],r[i][0]));
            acc=vu_ftz(add(acc,mul(m[1][k],r[i][1])));
            out[i][k]=vu_ftz(add(acc,mul(m[2][k],r[i][2])));
        }
    out[3]=m[3];                                   // 00280ea0 vmove
    return out;
}

RootVector camera_aim_node_point(const std::array<RootVector,4>& node) {
    for(const auto& row:node) check(row);
    // 0027daf4..0027db30: v = (0, 0, 0.25, 1); vmulax / vmadday / vmaddaz / vmaddw.
    const RootVector v{0.0F,0.0F,std::bit_cast<float>(0x3e800000U),1.0F};
    RootVector out{};
    for(std::size_t k=0;k<4;++k) {
        float acc=vu_ftz(mul(node[0][k],v[0]));
        acc=vu_ftz(add(acc,mul(node[1][k],v[1])));
        acc=vu_ftz(add(acc,mul(node[2][k],v[2])));
        out[k]=vu_ftz(add(acc,mul(node[3][k],v[3])));
    }
    return out;
}

RootVector camera_game_euler_quaternion(float x,float y,float z) {
    const float hx=mul(half,x),hy=mul(half,y),hz=mul(half,z);
    const float cx=game_cos(hx),cy=game_cos(hy),cz=game_cos(hz);
    const float sx=game_sin(hx),sy=game_sin(hy),sz=game_sin(hz);
    const float cycz=mul(cy,cz),sysz=mul(sy,sz),cysz=mul(cy,sz),sycz=mul(sy,cz);
    return {sub(mul(sx,cycz),mul(cx,sysz)),add(mul(cx,sycz),mul(sx,cysz)),
            add(mul(cx,cysz),mul(sx,sycz)),sub(mul(cx,cycz),mul(sx,sysz))};
}

RootVector camera_euler_quaternion(float x,float y,float z) {
    const float hx=mul(half,x),hy=mul(half,y),hz=mul(half,z);
    const float cx=checked(std::cos(hx)),cy=checked(std::cos(hy)),cz=checked(std::cos(hz));
    const float sx=checked(std::sin(hx)),sy=checked(std::sin(hy)),sz=checked(std::sin(hz));
    const float cycz=mul(cy,cz),sysz=mul(sy,sz),cysz=mul(cy,sz),sycz=mul(sy,cz);
    return {sub(mul(sx,cycz),mul(cx,sysz)),add(mul(cx,sycz),mul(sx,cysz)),
            add(mul(cx,cysz),mul(sx,sycz)),sub(mul(cx,cycz),mul(sx,sysz))};
}

float camera_forward_yaw(const RootVector& f) {
    return checked(std::atan2(-f[0],f[1]));
}
float camera_forward_pitch(const RootVector& f) {
    return checked(std::atan2(f[2],checked(std::sqrt(add(mul(f[1],f[1]),mul(f[0],f[0]))))));
}

RootVector player_camera_look_offset(float stick_x,float stick_y,bool invert_yaw,bool invert_pitch) {
    const float degree=std::bit_cast<float>(0x3c8efa35U);
    const float yaw_range=mul(degree,160.0F), pitch_range=mul(degree,60.0F);   // 0048ae78 / 0048ae70
    const float yaw=mul(invert_yaw ? -1.0F : 1.0F,mul(yaw_range,-stick_x));
    const float pitch=mul(invert_pitch ? -1.0F : 1.0F,mul(pitch_range,-stick_y));
    return {pitch,0.0F,yaw,0.0F};
}

PlayerCameraState make_player_camera() {
    PlayerCameraState s;
    s.rig=make_camera_rig(0.0F);
    return s;
}

float player_camera_pitch_limit(std::uint32_t flags_cf4) {
    // 00280c50..00280c74: *004a9320 with bit 2, else *004a9318 (both set by
    // the static initialiser 004d2770).
    return ((flags_cf4>>2)&1U) ? aim_pitch_limit : pitch_limit;
}

std::uint32_t player_camera_aim_flags_cf8(std::uint32_t input_bits_3c,const std::function<std::uint8_t()>& hold_class) {
    if(((input_bits_3c>>8)&1U)==0) return 0;     // 00247b20..00247b2c
    std::uint32_t flags=4U;                       // 00247b34..00247b3c
    if(!hold_class) throw std::runtime_error("Player camera flags: 00247800 reads the held class 001437a0(+3524) at 00247b40 (not supplied)");
    if(hold_class()!=1) flags|=8U;                // 00247b48..00247b60 (andi 0xff)
    return flags;
}

std::uint32_t player_camera_pick_flag_cf8(std::uint32_t input_bits_3c) {
    return ((input_bits_3c>>5)&1U)!=0 ? 2U : 0U;   // 00247b68..00247b84
}

float player_camera_frame_height(std::uint32_t flags_cf4,float height_d88,float height_d18) {
    // 0027db78..0027dbd4 (s5 = bit 5 from 0027db6c).
    if((flags_cf4&0xc20U)!=0) return add(height_d88,std::bit_cast<float>(0x40100000U));   // 2.25
    return add(height_d88,height_d18);
}

std::array<float,2> player_camera_boom_af0(std::uint32_t flags_cf4,float distance_d14,float side_d1c) {
    const bool b3=((flags_cf4>>3)&1U)!=0,b5=((flags_cf4>>5)&1U)!=0,b6=((flags_cf4>>6)&1U)!=0;
    const bool b10=((flags_cf4>>10)&1U)!=0,b11=((flags_cf4>>11)&1U)!=0;
    float distance=distance_d14;                                    // 002808a0
    if(b3||b5) distance=0.0F;                                       // 00280860
    else if(b11) distance=2.0F;                                     // 00280878
    else if(b10) distance=5.0F;                                     // 00280890
    const float af4=std::min(0.0F,-distance);                       // 002808b0..002808bc (min.s)
    const float af0=(b3||b11||b10||b5||b6) ? 0.0F : side_d1c;        // 002808b8..0028094c
    return {af0,af4};
}

PlayerCameraRetimes player_camera_frame_retimes(std::uint32_t flags_cf0,std::uint32_t flags_cf4,
    std::int32_t old_kind,float old_height,std::int32_t kind,float height,
    std::int32_t mode_320c,std::uint32_t flags_3248,PlayerCameraRetimes r) {
    const auto bit=[](std::uint32_t w,int k){return ((w>>k)&1U)!=0;};
    if(bit(flags_cf4,2)) return r;                                  // 0027dd28..0027dd34
    // 0027dd3c..0027dd9c: a changed frame (+d84, a -1 kind, or +d88).
    if(old_kind!=kind||old_kind==-1||!(old_height==height)) {
        r.orientation_210=std::max(transition,r.orientation_210);
        r.pivot_2e0=std::max(transition,r.pivot_2e0);
    }
    if(!bit(flags_cf0,11)&&bit(flags_cf4,11)) {
        // 0027dde8..0027de38: bit 0xb rising (set, not raised).
        const float v=!bit(flags_cf4,5)&&mode_320c==2 ? aim_exit_time : std::bit_cast<float>(0x3ecccccdU);
        r.pivot_2e0=v;r.boom_910=v;
    } else if(bit(flags_cf0,11)&&!bit(flags_cf4,11)) {
        r.pivot_2e0=std::max(1.0F,r.pivot_2e0);                     // 0027de88..0027deb8
        r.boom_910=std::max(1.0F,r.boom_910);
    } else if(bit(flags_cf0,10)!=bit(flags_cf4,10)) {
        r.pivot_2e0=std::max(1.5F,r.pivot_2e0);                     // 0027df50..0027df7c
        r.boom_910=std::max(1.5F,r.boom_910);
    }
    if(bit(flags_cf4,5))                                            // 0027df80..0027dfcc
        r.boom_910=std::bit_cast<float>(bit(flags_3248,5) ? 0x3fa66666U : 0x3f666666U);
    if(bit(flags_cf4,1)) r.orientation_210=std::max(transition,r.orientation_210);        // 0027dfd0..0027e000
    else if(bit(flags_cf0,1)) r.orientation_210=std::max(1.0F,r.orientation_210);         // 0027e008..0027e054
    return r;
}

void update_player_camera(PlayerCameraState& s,const PlayerCameraInput& in,
    const std::optional<PlayerCameraTarget>& target,float elapsed,const CameraCollisionHost* host,const CameraZoomHost* zoom,
    const std::function<std::uint32_t()>* random) {
    checked(elapsed);
    if(!(elapsed>0)) throw std::runtime_error("Player camera elapsed time must be positive");
    if(!target) throw std::runtime_error("Player camera requires a followed actor");
    const auto& t=*target;
    if(t.mode_320c!=0||t.flags_3248!=0) throw std::runtime_error("Player camera supports only the ordinary actor mode");
    check(t.floor_root.orientation);check(t.floor_root.position);check(t.look_target);
    check(in.look_offset_d30);check(in.rates_d60);
    auto n=s;
    // 0024f9fc: field of view from the weapon query.
    n.rig.fov_c64=n.rig.fov_c68=checked(in.field_of_view);
    // 00247800 (pad stage, before the camera's preupdate).
    if(in.lock) {
        if(in.lock->object==0) throw std::runtime_error("Player camera lock requires an object");
        n.lock_d04=in.lock->object;          // 00247950
        n.lock_id_d08=in.lock->id;           // 00247980
        n.lock_time_d10=lock_time;           // 00247988, *004a9330
    }
    n.subtarget_d0c=in.subtarget_d0c;        // 00247990
    if(in.rotation_d90) {
        check(*in.rotation_d90);
        n.rotation_d90=*in.rotation_d90;     // 00247d14
    }
    // 00280f60.
    n.flags_cf0=n.flags_cf4;
    n.flags_cf4=in.flags_cf8;
    // 0017a610: the boom curve +910 still running (+9c4 != 0) keeps the lock.
    if(checked(n.rig.boom_910.remaining)!=0) n.lock_time_d10=lock_hold;
    else n.lock_time_d10=sub(n.lock_time_d10,elapsed);
    if(n.lock_time_d10<=0) {
        n.lock_d04=0;n.lock_id_d08=-1;n.lock_time_d10=0;
    }
    // 00281014: 0017e1b0(camera, dt) stores +64 = atan2f(T +20, T +24) of the +50 transform as it
    // stands (0017e204..0017e210; the sound pan 001c7ce8 reads it). The rest of 0017e1b0 (003ce650
    // and its +2c..+2e tail) is not ported.
    n.heading_64=game_atan2f(n.transform_50[1][0],n.transform_50[1][1]);
    // Only +cf4/+cf0 bits 1 (the pick camera), 2 (the aim view), 3 (aiming),
    // 0xa (crawl zone) and 0xb (crouched under a low ceiling) get past here.
    if(((n.flags_cf4|n.flags_cf0)&~0xc0eU)!=0) throw std::runtime_error("Player camera mode flags are not supported (0027d620: +cf4 bits other than 1, 2, 3, 0xa, 0xb)");
    const bool bit3=((n.flags_cf4>>3)&1U)!=0;
    if(bit3&&!in.rotation_d90)
        throw std::runtime_error("Player camera: +cf4 bit 3 reads +d90 (00280da4), which the pad writes from 001ca840 of the secondary component +3314 (00247cd8; not supplied)");
    // 00280690 at 002807f8..0028094c.
    const auto boom=player_camera_boom_af0(n.flags_cf4,in.distance_d14,in.side_d1c);
    n.rig.boom_af0[0]=boom[0];
    n.rig.boom_af0[1]=boom[1];
    n.yaw_d78=add(n.yaw_d78,mul(elapsed,in.rates_d60[2]));
    n.pitch_d70=add(n.pitch_d70,mul(elapsed,in.rates_d60[0]));
    if(n.yaw_d78>pi) n.yaw_d78=add(n.yaw_d78,-two_pi);
    else if(n.yaw_d78<-pi) n.yaw_d78=add(n.yaw_d78,two_pi);
    const float limit=player_camera_pitch_limit(n.flags_cf4);
    n.pitch_d70=std::max(n.pitch_d70,-limit);
    n.pitch_d70=std::min(n.pitch_d70,limit);

    // 0027d620; s2 = +cf4 bit 2 and not bit 3 (0027d670).
    const bool aim=((n.flags_cf4>>2)&1U)!=0;
    const bool s2=aim&&!bit3;
    const bool was_aim=((n.flags_cf0>>2)&1U)!=0;
    const std::int32_t old_kind=n.frame_kind_d84;
    const float old_height=n.height_d88;
    n.frame_kind_d84=0;
    n.height_d88=s2 ? 5.0F : 0.0F;           // 0027d750
    // 001b0f30 default branch (a3 = s2).
    RootVector position=t.floor_root.position;
    if(t.physical) position[2]=sub(position[2],t.offset_394);
    position[3]=1.0F;
    if(s2) {
        // 001b13ec: X/Y of skeleton node *0049d024 = 14.
        if(!t.aim_node_14) throw std::runtime_error("Player camera aim view requires skeleton node 14 (001b0f30 a3 = 1, 00132910)");
        check(*t.aim_node_14);
        position[0]=(*t.aim_node_14)[0];
        position[1]=(*t.aim_node_14)[1];
    }
    const auto rows=camera_quaternion_rows(normalize4(t.floor_root.orientation));
    RootVector q=camera_vu_matrix_to_quaternion(rows);
    float time_210=-1,time_2e0=-1,time_780=-1,time_910=-1;
    // 00280470 writes +100/+110 (not used by the transform).
    if(bit3) {
        // 0027dab0..0027db54: node 14's world matrix (00133ed0 of the followed
        // actor +cfc) times (0, 0, 0.25, 1) replaces the whole position;
        // 00280390 and the +d18 add are skipped.
        if(!t.aim_node_14_matrix) throw std::runtime_error("Player camera aim bit 3 requires skeleton node 14's world matrix (00133ed0 at 0027dadc)");
        position=camera_aim_node_point(*t.aim_node_14_matrix);
    } else {
        // 00280390 leaves +d88 for frame kind 0; then 2.25 or +d18 is added.
        n.height_d88=player_camera_frame_height(n.flags_cf4,n.height_d88,in.height_d18);
    }
    position[2]=add(position[2],n.height_d88);   // 0027dbd8
    // 0027dbe8..0027dc60: entering or leaving the aim view (bit 2; max.s);
    // the retime is 0.5 for s2, else 0.33 (0027dc78).
    if(aim!=was_aim) {
        const float v=s2 ? 0.5F : aim_exit_time;
        time_210=std::max(v,time_210);
        time_2e0=std::max(v,time_2e0);
        time_910=std::max(v,time_910);
    }
    // 0027dd28..0027e058 (skipped in the aim view): frame change, crouch
    // bits 0xa/0xb, bit 1.
    {
        const auto r=player_camera_frame_retimes(n.flags_cf0,n.flags_cf4,old_kind,old_height,n.frame_kind_d84,n.height_d88,
            static_cast<std::int32_t>(t.mode_320c),t.flags_3248,{time_210,time_2e0,time_910});
        time_210=r.orientation_210;time_2e0=r.pivot_2e0;time_910=r.boom_910;
    }
    const bool pick=((n.flags_cf4>>1)&1U)!=0;
    RootVector pitch_target;
    float heading_pitch=0,heading_duration=heading_time;
    if(aim) {
        // 0027e318 skips the yaw look offset; 0027ea98 then the aim branch 0027eb10.
        pitch_target=camera_aim_pitch_quaternion(n.pitch_d70);
        // 0027eb24: the +50 transform's refresh (00132900) only clears its dirty byte.
        // 0027eb5c: 00280070(camera, sp+4e0, actor, 4, t0 = s2, transform+40);
        // on 1, +d70 = out[0] and +d78 = out[2] (0027eb70..0027eb7c), after
        // the pitch quaternion above read the old +d70. With s2 the origin is
        // the transform's translation, the matrix the previous update's rig
        // sent (+da0's source; 00280d90 rotates only rows 0..2); with bit 3
        // (s2 clear) it is node 14's world position (00280278..002802b0).
        std::optional<RootVector> origin;
        if(s2) {
            const bool transform_set=n.rig.matrix[0]!=RootVector{}||n.rig.matrix[1]!=RootVector{}||n.rig.matrix[2]!=RootVector{};
            if(transform_set) origin=n.rig.matrix[3];
        } else if(n.lock_d04!=0) {
            if(!t.aim_node_14) throw std::runtime_error("Player camera aim bit 3 look-at requires skeleton node 14 (00132910 at 00280298)");
            origin=*t.aim_node_14;
        }
        if(const auto look=camera_lock_look_at(n.lock_d04,n.lock_id_d08,n.subtarget_d0c,origin,in.look_at)) {
            n.pitch_d70=(*look)[0];
            n.yaw_d78=(*look)[2];
        }
        q=camera_aim_orientation(q,n.yaw_d78);
        heading_pitch=n.pitch_d70;            // sp+54c
        heading_duration=0;                   // f20
    } else {
        // 0027e318: yaw look offset.
        q=camera_quaternion_multiply(q,camera_axis_quaternion({0,0,1,0},n.rig.offset_6c0.current[2]));
        n.yaw_d78=camera_forward_yaw(camera_quaternion_rows(q)[1]);
        float pitch=slope_pitch(in.pitch_d20,q,t);
        if(t.physical&&t.unsupported_dc) {
            // 0027ea28: without accepted floor support (+dc set) the pitch
            // follows the velocity elevation.
            const auto& v=t.velocity_310;
            const float length=checked(std::sqrt(add(add(mul(v[0],v[0]),mul(v[1],v[1])),0.0F)));
            pitch=mul(checked(std::atan2(v[2],length)),std::bit_cast<float>(0x3f4ccccdU));
        }
        if(pick) {
            // 0027ee78: 0027fa40(camera, sp+520, actor), then the frame's Euler
            // X becomes +d70 and its pitch quaternion (0027ee98..0027f214).
            // 0027fbf0: 00280070 with a3 = 10, t0 = 0: node *0049d02a = 15.
            if(n.lock_d04!=0&&!t.aim_node_15) throw std::runtime_error("Player camera pick look-at requires skeleton node 15 (00132910 at 00280298)");
            q=camera_pick_orientation(q,n.hold_d80,n.hold_d81,n.lock_d04,n.lock_id_d08,n.subtarget_d0c,
                t.aim_node_15,s.transform_50,in.look_at);
            const auto frame=camera_pick_frame(q);
            q=frame.orientation;
            pitch_target=frame.pitch_quaternion;
            n.pitch_d70=frame.pitch_d70;
            heading_pitch=n.pitch_d70;        // sp+54c
            heading_duration=0;               // f20
        } else {
            pitch_target=n.rig.heading_530.current;
            n.pitch_d70=camera_forward_pitch(camera_quaternion_rows(n.rig.pitch_780.current)[1]);
            pitch_target=camera_quaternion_multiply(pitch_target,camera_axis_quaternion({1,0,0,0},n.rig.offset_6c0.current[0]));
            heading_pitch=pitch;
        }
    }
    // 0027f418.
    n.rig.target_orientation_ad0=q;
    n.rig.target_pivot_ae0=position;
    n.rig.active_200=true;
    // 0027f444: the game's trig in the aim view and the pick camera (s1 = 1
    // at 0027ee6c / 0027f20c); the follow path keeps the host trig of the
    // earlier checkpoints.
    n.rig.heading_460.target=aim||pick ? camera_game_euler_quaternion(heading_pitch,0,0) : camera_euler_quaternion(heading_pitch,0,0);
    retime_camera_quaternion_curve(n.rig.heading_460,heading_duration);
    retime_camera_quaternion_curve(n.rig.heading_530,heading_duration);
    set_animation_vector_target(n.rig.offset_600,{in.look_offset_d30[0],in.look_offset_d30[1],in.look_offset_d30[2],0.0F});
    n.rig.pitch_780.target=pitch_target;
    retime_camera_quaternion_curve(n.rig.orientation_210,time_210);
    retime_animation_vector_curve(n.rig.pivot_2e0,time_2e0);
    retime_animation_vector_curve(n.rig.look_3a0,time_2e0);
    retime_camera_quaternion_curve(n.rig.pitch_780,time_780);
    retime_animation_vector_curve(n.rig.boom_910,time_910);
    n.rig.body_c60=true;
    set_animation_vector_target(n.rig.look_3a0,t.look_target);
    n.rig.look_c61=true;
    if(((n.flags_cf0^n.flags_cf4)&1)!=0) n.rig.reset_201=true;

    update_camera_rig(n.rig,elapsed,host,zoom,random);
    n.world_matrix=n.rig.matrix;              // 00280d40..00280d8c: +da0
    // 00280d90..00280ef0: with bit 3 the +50 transform's rows 0..2 are
    // rotated by +d90; otherwise it keeps the rig's matrix.
    n.transform_50=bit3 ? camera_rotate_transform(n.rig.matrix,n.rotation_d90) : n.rig.matrix;
    s=n;
}

const std::array<RootVector,4>& player_camera_render_view(const PlayerCameraState& s) { return s.transform_50; }
}
