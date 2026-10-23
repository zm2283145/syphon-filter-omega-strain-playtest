#include "takedown_camera.hpp"
#include "actor_object_action.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Takedown camera: "+what);}
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float mul(float a,float b){return check(a*b);}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
constexpr float degree=std::bit_cast<float>(0x3c8efa35U);         // pi / 180
constexpr float angle_degrees=std::bit_cast<float>(0x41200000U);  // *00489bd8 (10.0)
constexpr float height=std::bit_cast<float>(0x40200000U);         // *00489bd0 (2.5)
constexpr float distance=std::bit_cast<float>(0x41200000U);       // *00489bc8 (10.0)
constexpr float pull=std::bit_cast<float>(0x41000000U);           // 8.0
constexpr float ninth=std::bit_cast<float>(0x3de38e39U);          // 1 / 9
constexpr float steep=std::bit_cast<float>(0x3f34fdf4U);          // 00131c70's |z| bound (0.70710677)
constexpr float pi=std::bit_cast<float>(0x40490fdbU);
// div.s: x / 0 gives FLT_MAX with the sign of x * 0 (+ for 0 / 0).
float ee_div(float a,float b){
    if(b==0.0F)return std::copysign(std::numeric_limits<float>::max(),a!=0.0F?(std::signbit(a)!=std::signbit(b)?-1.0F:1.0F):1.0F);
    return check(a/b);
}
// acc + s * t / acc - s * t: the multiply-accumulate product is rounded but
// not flushed; only the sum written to ACC or a register is.
float madd(float acc,float s,float t){return check(acc+s*t);}
float msub(float acc,float s,float t){return check(acc-s*t);}
// The VU product M x p (vmulax / vmadday / vmaddaz / vmaddw).
RootVector transform(const TargetingMatrix& m,const RootVector& p){
    RootVector r{};
    for(std::size_t k=0;k<4;++k)r[k]=madd(madd(madd(mul(m[0][k],p[0]),m[1][k],p[1]),m[2][k],p[2]),m[3][k],p[3]);
    return r;
}
// 001320f0 / 00131c70: a x b (vopmula ACC, a, b; vopmsub b, a), w = vf0.w - vf0.w = 0.
RootVector cross(const RootVector& a,const RootVector& b){
    return {msub(mul(a[1],b[2]),b[1],a[2]),msub(mul(a[2],b[0]),b[2],a[0]),msub(mul(a[0],b[1]),b[0],a[1]),0.0F};
}
// 00132030 / 00132060 / 00131c70: xyz * 1/sqrt((x*x + y*y) + z*z) (vrsqrt
// with vf0.w), w kept. A zero radicand gives the VU's Q = +FLT_MAX (the
// interpreter's and the hardware's division-by-zero result), so a zero
// vector stays zero.
RootVector normalized(RootVector v){
    const float s=madd(madd(mul(v[0],v[0]),1.0F,mul(v[1],v[1])),1.0F,mul(v[2],v[2]));
    const float q=s==0.0F?std::numeric_limits<float>::max():check(vu::rsqrt(s));
    for(std::size_t k=0;k<3;++k)v[k]=mul(v[k],q);
    return v;
}
// 001320b0: vaddx.xyz v + 0, then the MAC flags equal 0xe (x, y, z zero, no sign).
bool zero(const RootVector& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}
}

TakedownCamera player_takedown_camera(std::uint64_t actor,const RootVector& head,const TargetingMatrix& matrix_f4,
    const std::function<LosServiceResult(const LosServiceRequest&)>& los,const TakedownCameraTarget* target){
    if(!los)fail("the LOS service (003caa50(004ffb50, 004ea0b0) virtual +c) is not hosted");
    TakedownCamera out;
    out.head=head;
    // 001316b0: no target. f21 = 10 degrees, f20 = 2.5; sp+2d0 = (0, 10, 2.5, 1).
    float angle=mul(degree,angle_degrees);
    float lift=height;
    if(target){
        // 00131590: f22 = 10 degrees; d = target +e4 - head; f20 = |d| (vsqrt Q, xyz).
        const float f22=angle;
        RootVector d{};
        for(std::size_t k=0;k<4;++k)d[k]=sub(target->target_e4[k],head[k]);
        const float s=madd(madd(mul(d[0],d[0]),1.0F,mul(d[1],d[1])),1.0F,mul(d[2],d[2]));
        const float len=check(vu::sqrt(s));
        // f12 = 10 sinf(f22) / |d|; asinf (00121aa0) of |f12| > 1 is fdlibm's
        // (x - x) / (x - x): the EE FPU's 0 / 0 gives +FLT_MAX.
        const float f12=ee_div(mul(distance,game_sinf(f22)),len);
        const float as=std::fabs(f12)>1.0F?std::numeric_limits<float>::max():game_asinf(f12);
        angle=sub(pi,add(as,f22));
        angle=add(target->relative_z,angle);                                              // 0022ab20 out +8
        lift=ee_div(mul(distance,sub(target->actor_e4[2],target->target_e4[2])),len);
    }
    const RootVector offset{0.0F,distance,lift,1.0F};
    // 00132130 for the axis (0, 0, 1) (getters 00132270 / 00132260 / 00132250
    // of sp+150 = (0, 0, 1, 1)); row 3 (0, 0, 0, 1).
    const auto rows=actor_object_node_rows(angle);
    const TargetingMatrix rotation{rows[0],rows[1],rows[2],RootVector{0.0F,0.0F,0.0F,1.0F}};
    // 001316e4: the matrix's translation row is replaced by the head (all four words).
    TargetingMatrix frame=matrix_f4;
    frame[3]=head;
    RootVector position=transform(frame,transform(rotation,offset));
    RootVector dir{};                                                                     // sp+280 = head - position
    for(std::size_t k=0;k<4;++k)dir[k]=sub(head[k],position[k]);
    // The request: +30 the head (w 1.0), +40 = 0 - dir (vmulax ACC = 0,
    // vmsubw; w 0.0), +50 the actor, +54 0, +58 4, +5c 0.
    auto& q=out.los;
    q.origin_30={head[0],head[1],head[2],1.0F};
    q.displacement_40={sub(0.0F,dir[0]),sub(0.0F,dir[1]),sub(0.0F,dir[2]),0.0F};
    q.object_50=actor;q.object_54=0;q.mask_58=4;q.mode_5c=0;q.value_3c=1.0F;q.value_4c=0.0F;
    out.result=los(q);
    if(out.result.hit){
        // 00131920..001319bc: ((point * 8) + head) * (1/9), xyzw.
        for(std::size_t k=0;k<4;++k)position[k]=mul(add(mul(out.result.point[k],pull),head[k]),ninth);
    }
    out.position=position;
    // The frame. dir is not reread: the pull-in changes only the position.
    auto& m=out.view;
    const RootVector up{0.0F,0.0F,1.0F,1.0F};                                             // sp+100
    const RootVector c=cross(dir,up);                                                     // sp+90
    const RootVector last{0.0F,0.0F,0.0F,1.0F};                                           // 00132000(0, 0, 0, 1) / vf0
    if(!zero(c)){
        const auto n=normalized(dir);                               // 00132060
        const auto cn=normalized(c);                                   // 00132030
        m={cn,n,cross(cn,n),last};                                                        // 00131f00
    }else if(!zero(dir)){
        // 00131c70: the reference axis (0, 0, 1, 0) (00497f30) when |z| <
        // 0.70710677, else (1, 0, 0, 0) (00497f20).
        const RootVector ref=std::fabs(dir[2])<steep?RootVector{0.0F,0.0F,1.0F,0.0F}:RootVector{1.0F,0.0F,0.0F,0.0F};
        const auto n=normalized(dir);
        const auto cn=normalized(cross(dir,ref));
        m={cn,n,cross(cn,n),last};                                                        // 00131e90, 00131e70
    }else{
        m={RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},last};   // 00131c40
    }
    for(std::size_t k=0;k<3;++k)m[3][k]=add(m[3][k],position[k]);                        // vadd.xyz
    return out;
}
}
