#include "secondary_aim.hpp"
#include "upper_body_math.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using V=RootVector;
using M=std::array<float,16>;

[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Secondary aim: "+what);}
// IEEE binary32 with the EE/VU flush of denormal results (as the interpreter).
float fin(float x){
    if(!std::isfinite(x))fail("nonfinite arithmetic");
    if(x!=0.0F&&std::fabs(x)<std::bit_cast<float>(0x00800000U))return std::copysign(0.0F,x);
    return x;
}
float add(float a,float b){return fin(a+b);}
float sub(float a,float b){return fin(a-b);}
float mul(float a,float b){return fin(a*b);}
float dvd(float a,float b){if(b==0.0F)fail("division by zero");return fin(a/b);}
float fb(std::uint32_t b){return std::bit_cast<float>(b);}
// The interpreter's MAC sign flag: negative, or a negative zero.
bool sign_flag(float x){return x<0.0F||(x==0.0F&&std::signbit(x));}

V row(const M& m,int r){return {m[static_cast<std::size_t>(4*r)],m[static_cast<std::size_t>(4*r+1)],
    m[static_cast<std::size_t>(4*r+2)],m[static_cast<std::size_t>(4*r+3)]};}
void set_row(M& m,int r,const V& v){for(std::size_t c=0;c<4;++c)m[static_cast<std::size_t>(4*r)+c]=v[c];}

// vsub.xyz vf1,vf0,vf1 (1cfa70, 1bc280, 1cf5b0): 0 - xyz, w kept.
V conjugate(const V& q){return {sub(0.0F,q[0]),sub(0.0F,q[1]),sub(0.0F,q[2]),q[3]};}
// vmulax.xyzw ACC,vf0,vf0 / vmsubw.xyzw: 0 - v in every lane.
V negate(const V& v){return {sub(0.0F,v[0]),sub(0.0F,v[1]),sub(0.0F,v[2]),sub(0.0F,v[3])};}
// vmul.xyzw then x + y + z + w through the accumulator.
float dot4(const V& a,const V& b){
    return add(add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2])),mul(a[3],b[3]));
}
// vmul.xyz then (x + y) + z.
float dot3(const V& a,const V& b){return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}

// The inline rotation of (v.xyz, 0) by q: q * v * conj(q); w keeps v.w.
V rotate(const V& q,const V& v){
    const V c{sub(0.0F,q[0]),sub(0.0F,q[1]),sub(0.0F,q[2]),q[3]};
    // vf5 = q (x) (v, 0)
    const float px=sub(add(mul(q[1],v[2]),mul(v[0],q[3])),mul(v[1],q[2]));
    const float py=sub(add(mul(q[2],v[0]),mul(v[1],q[3])),mul(v[2],q[0]));
    const float pz=sub(add(mul(q[0],v[1]),mul(v[2],q[3])),mul(v[0],q[1]));
    const float pw=sub(sub(sub(mul(q[3],0.0F),mul(q[2],v[2])),mul(q[1],v[1])),mul(q[0],v[0]));
    // vf6.xyz = vf5 (x) conj
    const float rx=sub(add(add(mul(py,c[2]),mul(px,c[3])),mul(c[0],pw)),mul(c[1],pz));
    const float ry=sub(add(add(mul(pz,c[0]),mul(py,c[3])),mul(c[1],pw)),mul(c[2],px));
    const float rz=sub(add(add(mul(px,c[1]),mul(pz,c[3])),mul(c[2],pw)),mul(c[0],py));
    return {rx,ry,rz,v[3]};
}
// 00171b30: v as a row vector times the matrix, four lanes.
V transform4(const V& v,const M& m){
    V r{};
    for(std::size_t k=0;k<4;++k)
        r[k]=add(add(add(mul(m[k],v[0]),mul(m[4+k],v[1])),mul(m[8+k],v[2])),mul(m[12+k],v[3]));
    return r;
}
// 00133200: transpose the 3x3; a nonzero translation becomes -t through the
// transposed 3x3 (w of the translation row kept).
M rigid_inverse(const M& in){
    M m=in;
    std::swap(m[1],m[4]);std::swap(m[2],m[8]);std::swap(m[6],m[9]);
    const V t=row(m,3);
    if(!(t[0]==0.0F&&t[1]==0.0F&&t[2]==0.0F)){
        const V n{sub(0.0F,t[0]),sub(0.0F,t[1]),sub(0.0F,t[2]),t[3]};
        V r{};
        for(std::size_t k=0;k<3;++k)r[k]=add(add(mul(m[k],n[0]),mul(m[4+k],n[1])),mul(m[8+k],n[2]));
        r[3]=t[3];
        set_row(m,3,r);
    }
    return m;
}
// 00132060 / 00132030: xyz times VRSQRT(1, x2 + y2 + z2), w kept.
V normalize3(const V& v,const char* what){
    const float s=add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));
    if(s==0.0F)fail(what);
    const float r=fin(vu::rsqrt(s));
    return {mul(v[0],r),mul(v[1],r),mul(v[2],r),v[3]};
}
// 001320f0: vopmula / vopmsub, w = vf0.w - vf0.w.
V cross(const V& a,const V& b){
    return {sub(mul(a[1],b[2]),mul(b[1],a[2])),sub(mul(a[2],b[0]),mul(b[2],a[0])),sub(mul(a[0],b[1]),mul(b[0],a[1])),0.0F};
}
// 001320b0: the MAC zero flags of x, y, z all set (and no sign flags) after +0.
bool zero3(const V& v){return add(v[0],0.0F)==0.0F&&add(v[1],0.0F)==0.0F&&add(v[2],0.0F)==0.0F;}

// 001cf660(out, direction, up, matrix 0055c9f0 (identity), limit).
M look_at(const V& direction,const V& up,float limit){
    const M identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const V d=normalize3(direction,"zero look direction");
    auto frame=[&](const V& dir,const V& axis){
        const V x=cross(dir,axis);
        if(zero3(x))fail("look-at fallback frame (00131c70 / 00131c40) is not native");
        const V forward=normalize3(dir,"zero look direction");
        const V side=normalize3(x,"zero side axis");
        const V third=cross(side,forward);
        M m{};
        set_row(m,0,side);set_row(m,1,forward);set_row(m,2,third);set_row(m,3,{0.0F,0.0F,0.0F,1.0F});
        return m;
    };
    if(std::fabs(dot3(d,up))<limit)return frame(direction,up);
    if(std::fabs(dot3(d,row(identity,2)))<limit)return frame(d,row(identity,2));
    // Third axis (the identity's row 0), then the rows reordered (1cf960..1cf9d8).
    const M m=frame(d,row(identity,0));
    M r=m;
    set_row(r,0,row(m,2));set_row(r,1,row(m,1));set_row(r,2,negate(row(m,0)));
    return r;
}

std::uint8_t aims(std::uint8_t state){
    // 0049f790 rows 0..24, byte +4d, as the boot image's static initializer leaves them.
    static constexpr std::array<std::uint8_t,25> table{0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,1,1,0};
    if(state>=table.size())fail("secondary state outside the 0049f790 table");
    return table[state];
}

// The clamp block (1cbbe8..1cbd40 with cos 45, 1cbe0c..1cbf28, 1cc028..1cc144,
// 1cc3f8..1cc514): when w < limit and the axis is not zero, w = limit and
// xyz = scale * the normalized axis. Returns whether it applied.
bool clamp_quaternion(V& q,float limit,float scale){
    if(!(q[3]<limit))return false;
    if(q[0]==0.0F&&q[1]==0.0F&&q[2]==0.0F)return false;
    const V axis=normalize3({q[0],q[1],q[2],0.0F},"zero clamp axis");
    q={mul(scale,axis[0]),mul(scale,axis[1]),mul(scale,axis[2]),limit};
    return true;
}

// 001cff50(component, quaternion, dt): the recoil kick.
bool recoil_kick(SecondaryAimState& s,V& q,const std::function<std::uint32_t()>& random){
    if(s.recoil_324<=0.0F)return false;
    if(!random)fail("the recoil kick needs the game RNG (00127dd0)");
    auto draw=[&]{
        const std::uint32_t r=random();
        return dvd(static_cast<float>(static_cast<std::int32_t>(r)),2147483648.0F);
    };
    const float u1=draw();
    const float pitch=mul(s.recoil_324,add(add(mul(fb(0x3c730fc1U),u1),fb(0xbbf30fc1U)),fb(0x3dc57ccdU)));
    const float u2=draw();
    const float yaw=add(mul(fb(0x3f32b8c2U),u2),fb(0xbe32b8c2U));
    const float sy=fin(game_sinf(yaw)),cy=fin(game_cosf(yaw));
    // 00140e20 rows (cy, 0, -sy) / (0, 1, 0) / (sy, 0, cy) applied to (1, 0, 0).
    const V axis{add(add(mul(cy,1.0F),mul(0.0F,0.0F)),mul(sy,0.0F)),
                 add(add(mul(0.0F,1.0F),mul(1.0F,0.0F)),mul(0.0F,0.0F)),
                 add(add(mul(-sy,1.0F),mul(0.0F,0.0F)),mul(cy,0.0F)),0.0F};   // row 0 z = neg.s sin
    const float h1=mul(0.5F,pitch);
    const float c1=fin(game_cosf(h1)),s1=fin(game_sinf(h1));
    const V q1{mul(s1,axis[0]),mul(s1,axis[1]),mul(s1,axis[2]),c1};
    const float u3=draw();
    const float roll=add(mul(fb(0x3d364bd1U),u3),fb(0xbcb64bd1U));
    const float h2=mul(0.5F,roll);
    const float c2=fin(game_cosf(h2)),s2=fin(game_sinf(h2));
    const V q2{mul(s2,0.0F),mul(s2,1.0F),mul(s2,0.0F),c2};
    q=secondary_quaternion_multiply(q,secondary_quaternion_multiply(q1,q2));
    s.recoil_324=0.0F;
    return true;
}

void step_layer(SecondaryAimLayer& layer,std::span<const M> nodes,float dt,float inverse_dt){
    V q=layer.orientation,p=layer.position;
    if(layer.node!=-1){
        if(layer.node<0||static_cast<std::size_t>(layer.node)>=nodes.size())fail("layer node outside the node collection");
        const M& n=nodes[static_cast<std::size_t>(layer.node)];
        const V nq=secondary_matrix_quaternion(n);
        const V t=row(n,3);
        q=secondary_quaternion_multiply(nq,q);
        const V r=rotate(nq,p);
        p={add(r[0],t[0]),add(r[1],t[1]),add(r[2],t[2]),r[3]};
    }
    layer.rotation.target=q;
    step_camera_quaternion_curve(layer.rotation,dt,inverse_dt);
    layer.translation.target=p;
    step_animation_vector_curve(layer.translation,dt);
}

// One step of a retained quaternion curve as 001cb160 runs it for +160 / +230:
// the retime (001cf530 on the embedded segment, 001cf520) when the duration is
// not negative, then the five stages of the class.
void retime(CameraQuaternionCurve& c,float duration){
    if(duration<0.0F)return;
    retime_camera_quaternion_curve(c,duration);
}
}

RootVector secondary_aim_moved_target(const RootVector& old_look,const RootVector& new_look,
    const RootVector& target,const RootVector& origin){
    const V r=secondary_quaternion_multiply(new_look,conjugate(old_look));            // 001cc1a8
    const V offset{sub(target[0],origin[0]),sub(target[1],origin[1]),sub(target[2],origin[2]),sub(target[3],origin[3])};
    const V s=upper_body_math::rotate_offset(r,offset);                                // 001cc1e4..001cc228
    return {add(origin[0],s[0]),add(origin[1],s[1]),add(origin[2],s[2]),add(origin[3],s[3])};   // 00173d00
}

SecondaryAimBody secondary_aim_body(const RootVector& aim_cf0,const RootVector& curve_250,const RootVector& look_d90){
    SecondaryAimBody r{aim_cf0,{}};
    if(!(std::fabs(sub(1.0F,curve_250[3]))<=fb(0x33d6bf95U)))r.aim_cf0=secondary_quaternion_multiply(r.aim_cf0,curve_250);
    r.body_cd0=secondary_quaternion_multiply(r.aim_cf0,conjugate(look_d90));             // 001cc80c..001cc858
    return r;
}

RootVector secondary_aim_root_offset(const RootVector& base_orientation,const std::array<float,16>& inverse){
    const V r=rotate(base_orientation,{fb(0x3e99999aU),0.0F,0.0F,0.0F}); // 001cdec0..001cdeec
    V offset{0.0F,0.0F,0.0F,r[3]};                                        // 0013ed60: xyz only
    for(std::size_t k=0;k<3;++k)
        offset[k]=add(add(mul(inverse[k],r[0]),mul(inverse[4+k],r[1])),mul(inverse[8+k],r[2]));
    return offset;
}

std::array<float,16> secondary_aim_inverse(const std::array<float,16>& component){return rigid_inverse(component);}

RootVector secondary_quaternion_multiply(const RootVector& a,const RootVector& b){
    const float x=sub(add(add(mul(a[1],b[2]),mul(a[0],b[3])),mul(b[0],a[3])),mul(b[1],a[2]));
    const float y=sub(add(add(mul(a[2],b[0]),mul(a[1],b[3])),mul(b[1],a[3])),mul(b[2],a[0]));
    const float z=sub(add(add(mul(a[0],b[1]),mul(a[2],b[3])),mul(b[2],a[3])),mul(b[0],a[1]));
    const float w=sub(sub(sub(mul(a[3],b[3]),mul(a[2],b[2])),mul(a[1],b[1])),mul(a[0],b[0]));
    return {x,y,z,w};
}

RootVector secondary_matrix_quaternion(const std::array<float,16>& m){
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    const float trace=add(add(m00,m11),m22);
    if(!(trace>0.0F)){
        const float xy=add(sub(m00,m11),0.0F),yz=add(sub(m11,m22),0.0F),zx=add(sub(m22,m00),0.0F);
        int block;
        if(sign_flag(xy))block=sign_flag(yz)?2:1;
        else block=sign_flag(zx)?0:2;
        if(block==2){                                     // 1cb540
            const float s=fin(vu::sqrt(add(sub(zx,m11),1.0F)));
            const float inv=dvd(1.0F,add(0.0F,s));
            return {mul(mul(add(0.0F,add(m02,m20)),0.5F),inv),mul(mul(add(0.0F,add(m21,m12)),0.5F),inv),
                    mul(add(0.0F,s),0.5F),mul(mul(sub(m01,m10),0.5F),inv)};
        }
        if(block==1){                                     // 1cb594
            const float s=fin(vu::sqrt(add(sub(yz,m00),1.0F)));
            const float inv=dvd(1.0F,add(0.0F,s));
            return {mul(mul(add(0.0F,add(m10,m01)),0.5F),inv),mul(add(0.0F,s),0.5F),
                    mul(mul(add(0.0F,add(m21,m12)),0.5F),inv),mul(mul(sub(m20,m02),0.5F),inv)};
        }
        const float s=fin(vu::sqrt(add(sub(xy,m22),1.0F)));   // 1cb5e0
        const float inv=dvd(1.0F,add(0.0F,s));
        return {mul(add(0.0F,s),0.5F),mul(mul(add(0.0F,add(m10,m01)),0.5F),inv),
                mul(mul(add(0.0F,add(m02,m20)),0.5F),inv),mul(mul(sub(m12,m21),0.5F),inv)};
    }
    const float s=fin(vu::sqrt(add(trace,1.0F)));               // 1cb4bc
    const float inv=dvd(1.0F,s);
    return {mul(mul(add(0.0F,sub(m12,m21)),0.5F),inv),mul(mul(add(0.0F,sub(m20,m02)),0.5F),inv),
            mul(mul(add(0.0F,sub(m01,m10)),0.5F),inv),mul(s,0.5F)};
}

std::array<float,16> secondary_look_at(const RootVector& direction,const RootVector& up,float limit){
    return look_at(direction,up,limit);
}

RootVector bone_rotation_cache(const RootVector& accumulator,const RootVector& cache,const std::array<float,16>& inverse_bind){
    auto all_zero=[](const V& v){return add(v[0],0.0F)==0.0F&&add(v[1],0.0F)==0.0F&&add(v[2],0.0F)==0.0F&&add(v[3],0.0F)==0.0F;};
    if(!all_zero(accumulator)){                                    // 003ba304..003ba32c
        const float s=dot4(accumulator,accumulator);
        if(s==0.0F)fail("rotation accumulator underflow");
        const float r=fin(vu::rsqrt(s));
        return {mul(accumulator[0],r),mul(accumulator[1],r),mul(accumulator[2],r),mul(accumulator[3],r)};
    }
    if(!all_zero(cache))return cache;
    return conjugate(secondary_matrix_quaternion(inverse_bind));   // 003ba40c..003ba5d0
}

SecondaryAimLayer make_secondary_aim_layer(){
    SecondaryAimLayer l;
    l.rotation=make_camera_quaternion_curve({0,0,0,1},{0,0,0,0},false,0.0F);
    l.translation.previous=l.translation.current=l.translation.target={0,0,0,1};
    l.translation.rate={0,0,0,0};
    l.translation.damping=0.0F;
    l.translation.segment={{0,0,0,1},{0,0,0,0},{0,0,0,1},{0,0,0,0},0.0F,0.0F};
    l.translation.remaining=0.0F;
    return l;
}

bool secondary_aim_layer_settled(const SecondaryAimLayer& layer){
    return layer.rotation.remaining==0.0F&&layer.translation.remaining==0.0F;
}

SecondaryAimState make_secondary_aim_state(){
    SecondaryAimState s;
    s.curve_160=make_camera_quaternion_curve({0,0,0,1},{0,0,0,0},false,0.0F);
    s.curve_230=make_camera_quaternion_curve({0,0,0,1},{0,0,0,0},false,0.0F);
    s.layers={make_secondary_aim_layer(),make_secondary_aim_layer()};
    return s;
}

void request_secondary_aim_recoil(SecondaryAimState& state,float amount,float record_a4){
    state.recoil_324=fin(amount);
    if(fin(record_a4)<=4.0F){state.recoil_328=0.0F;return;}
    float v=mul(add(record_a4,30.0F),fb(0x3c93aef5U));     // f13 - (-30)
    v=std::max(v,0.0F);
    v=std::min(v,1.0F);
    state.recoil_328=v;
}

SecondaryAimResult step_secondary_aim(SecondaryAimState& state,ActorAimState& aim,const SecondaryAimInput& in){
    const float dt=fin(in.dt);
    if(!(dt>0.0F))fail("dt must be positive");
    const auto& model=in.model;
    if(model.nodes.size()<15)fail("the aim solve reads node 0xe: fewer than 15 nodes");
    const std::uint8_t aim310=aims(in.current_310),aim311=aims(in.next_311);
    SecondaryAimState s=state;
    ActorAimState a=aim;
    const float inverse_dt=dvd(1.0F,dt);

    // 1cb1f8..1cb294: the component matrix, its rigid inverse and world Z in skeleton space.
    const M& component=model.component;
    const M inverse=rigid_inverse(component);
    const V up=transform4({0.0F,0.0F,1.0F,0.0F},inverse);

    // 1cb298..1cb334: the weights move toward +304 (their eased values are not used).
    {
        const float f5=mul(fb(0x3fe66666U),dt),f4=mul(3.0F,dt);
        float d=sub(s.weight_304,s.weight_308);
        d=std::min(std::max(d,sub(0.0F,f5)),f5);
        s.weight_308=add(s.weight_308,d);
        d=sub(s.weight_304,s.weight_30c);
        d=std::min(std::max(d,sub(0.0F,f4)),f4);
        s.weight_30c=add(s.weight_30c,d);
    }
    // 1cb368..1cb3e8: the two layers and the settled bytes.
    step_layer(s.layers[0],model.nodes,dt,inverse_dt);
    step_layer(s.layers[1],model.nodes,dt,inverse_dt);
    s.settled_820=s.settled_821;
    s.settled_821=(secondary_aim_layer_settled(s.layers[0])&&secondary_aim_layer_settled(s.layers[1]))?1:0;

    // 1cb3ec..1cb6c4: the body orientation relative to the base aggregate.
    const V q0=model.node0_rotation;
    const V q0_conj=conjugate(q0);
    const V component_q=secondary_matrix_quaternion(component);
    V body=secondary_quaternion_multiply(conjugate(model.base_orientation),secondary_quaternion_multiply(component_q,q0));
    if(sign_flag(dot4(body,s.body_150)))body=negate(body);

    // 1cb6c8..1cb798: aim slot 0 is replaced unless a state aims (always for +311 = 0x10).
    if(in.next_311==0x10||!(aim310||aim311)){
        const V ahead=rotate(model.base_orientation,{0.0F,2000.0F,0.0F,1.0F});
        a.aim_point_3b0={add(ahead[0],model.base_position[0]),add(ahead[1],model.base_position[1]),
                         add(ahead[2],model.base_position[2]),ahead[3]};
        a.aim_active_3a5=1;
    }
    const V target=transform4(a.aim_point_3b0,inverse);

    // 1cb7d8..1cb9a8: the origin node and offset, the look direction.
    std::size_t node=0xe;
    V offset{0.0F,0.0F,0.0F,1.0F};
    if(in.next_311==0xc||in.next_311==0x10||in.next_311==6||in.next_311==7){
        node=0;
        if(in.next_311==0xc||in.next_311==0x10){
            const V r=rotate(model.base_orientation,{fb(0x3fb9999aU),0.0F,0.0F,1.0F});
            for(std::size_t k=0;k<3;++k)
                offset[k]=add(add(mul(inverse[k],r[0]),mul(inverse[4+k],r[1])),mul(inverse[8+k],r[2]));
            offset[3]=r[3];
        }
    }
    const V t=row(model.nodes[node],3);
    const V origin{add(t[0],offset[0]),add(t[1],offset[1]),add(t[2],offset[2]),t[3]};
    const V direction{sub(target[0],origin[0]),sub(target[1],origin[1]),sub(target[2],origin[2]),sub(target[3],origin[3])};

    // 1cb9b0..1cbd5c: the look-at, its quaternion and the 0xc pre-clamp.
    V look=secondary_matrix_quaternion(look_at(direction,up,fb(0x3f7fffe7U)));
    if(in.next_311==0xc){
        look=secondary_quaternion_multiply(body,look);
        if(look[3]<0.0F)look=negate(look);
        clamp_quaternion(look,fb(0x3f350481U),fb(0x3f350481U));
        look=secondary_quaternion_multiply(conjugate(body),look);
    }
    // 1cbd60..1cbdec: the per-state limits.
    float limit,scale;
    if(in.current_310==0xa||in.next_311==0xa){limit=fb(0x3ec3f141U);scale=fb(0x3f6c84b6U);}
    else if(in.current_310==0x17||in.next_311==0x17){limit=fb(0x3ed85f07U);scale=fb(0x3f680347U);}
    else{limit=fb(0x3c8ef34dU);scale=fb(0x3f7ff2e5U);}
    // 1cbdf0..1cbf28: the last frame's aim in this frame's body frame, clamped.
    V previous=secondary_quaternion_multiply(conjugate(body),s.aim_140);
    clamp_quaternion(previous,limit,scale);
    // 1cbf30..1cc020: the solved aim, aligned with it, and the +300 timer.
    V solved=secondary_quaternion_multiply(q0_conj,look);
    if(dot4(solved,previous)<=0.0F)solved=negate(solved);
    if(solved[3]<0.0F){
        if(solved[3]<=fb(0xbeaf1aa0U)||!(s.timer_300<=0.25F))solved=negate(solved);
        s.timer_300=add(s.timer_300,dt);
    }else s.timer_300=0.0F;
    // 1cc028..1cc17c.
    SecondaryAimResult result;
    result.clamped=clamp_quaternion(solved,limit,scale);
    result.target_d70=target;
    result.up_df0=up;
    result.look_d90=look;
    if(result.clamped){
        solved=secondary_quaternion_multiply(q0,solved);
        // 001cc154..001cc264: sp+d90 becomes the clamped aim and sp+d70 the
        // target turned about the origin by the change.
        result.look_d90=solved;
        result.target_d70=secondary_aim_moved_target(look,solved,target,origin);
    }

    // 1cc268..1cc3e4: the +160 curve.
    s.curve_160.damping=10.0F;
    retime(s.curve_160,(aim310||aim311)?fb(0x3e19999aU):0.0F);
    s.curve_160.target=solved;
    s.curve_160.current=previous;
    s.curve_160.segment=make_animation_vector_segment(s.curve_160.segment.duration,s.curve_160.current,
        s.curve_160.rate,s.curve_160.target,{0.0F,0.0F,0.0F,0.0F});        // 001cf400 (end rate 001802f0)
    step_camera_quaternion_curve(s.curve_160,dt,inverse_dt);
    V smoothed=s.curve_160.current;
    clamp_quaternion(smoothed,limit,scale);
    const V aim_140=secondary_quaternion_multiply(body,smoothed);
    const V aim_cf0=secondary_quaternion_multiply(q0,smoothed);             // 001cc528..001cc534

    // 1cc538..1cc788: the +230 recoil spring in steps of at most 1/60 s.
    {
        const bool aiming=aim310||aim311;
        s.curve_230.damping=5.0F;
        float left=dt;
        while(!(left<=0.0F)){
            const float step=std::min(fb(0x3c888889U),left);
            V kicked=s.curve_230.current;
            const bool kick=recoil_kick(s,kicked,in.random);
            float duration;
            if(aiming)duration=kick?add(mul(s.recoil_328,fb(0x3d48b439U)),fb(0x3ca3d70aU))
                                   :add(mul(sub(1.0F,s.recoil_328),fb(0x3e19999aU)),fb(0x3d4ccccdU));
            else duration=fb(0x3e19999aU);
            retime(s.curve_230,duration);
            s.curve_230.target=aiming?(kick?kicked:conjugate(kicked)):V{0.0F,0.0F,0.0F,1.0F};
            step_camera_quaternion_curve(s.curve_230,step,dvd(1.0F,step));
            left=sub(left,step);
        }
    }
    {
        const SecondaryAimBody b=secondary_aim_body(aim_cf0,s.curve_230.current,result.look_d90);
        result.aim_cf0=b.aim_cf0;
        result.body_cd0=b.body_cd0;
    }
    result.root_offset_9f0=secondary_aim_root_offset(model.base_orientation,inverse);
    s.aim_140=aim_140;
    s.body_150=body;

    // 1cc834..1cc8d0: the angle of the Y axis rotated by the solved aim.
    const V y=rotate(solved,{0.0F,1.0F,0.0F,0.0F});
    result.angle=fin(game_atan2f(-y[0],y[1]));                          // neg.s: exact sign flip
    state=s;
    aim=a;
    return result;
}

}
