#include "camera_rig.hpp"
#include "actor_specialized_body_query.hpp"
#include "game_libm.hpp"
#include "water_effects.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float v) {
    if(!std::isfinite(v)) throw std::runtime_error("Camera rig requires finite arithmetic");
    return v;
}
float add(float a,float b) { return checked(a+b); }
float sub(float a,float b) { return checked(a-b); }
float mul(float a,float b) { return checked(a*b); }
void check(const RootVector& v) { for(float x:v) checked(x); }
RootVector add3(RootVector a,const RootVector& b) { for(int i=0;i<3;++i) a[i]=add(a[i],b[i]); return a; }
RootVector sub4(const RootVector& a,const RootVector& b) {
    return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};
}
float dot3(const RootVector& a,const RootVector& b) {
    return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));
}
AnimationVectorCurveState vector_curve(float damping) {
    AnimationVectorCurveState c;                       // 0017f520((0,0,0,1), zero)
    c.previous=c.current=c.target={0,0,0,1};
    c.rate={};
    c.damping=damping;
    c.segment={{0,0,0,1},{},{0,0,0,1},{},0,0};
    c.remaining=0;
    return c;
}
CameraScalarCurve scalar_curve(float damping) {
    CameraScalarCurve c;                               // 0017f480(0, 0)
    c.damping=damping;
    return c;
}
void settle(AnimationVectorCurveState& c) {
    c.current=c.target;
    c.previous=c.current;
    c.rate={};                                          // vtable +14 (001800c0)
}
// 0017a640 inline vector step: previous, preupdate 0017d100, timer 0017d0a0,
// sampler 0017cea0 + post 00180110, rate 0017cdd0.
void step(AnimationVectorCurveState& c,float elapsed) { step_animation_vector_curve(c,elapsed); }
}

RootVector camera_rotate(const RootVector& q,const RootVector& v) {
    // 001a7370 pattern: (q * (v.xyz, 0)) * conj(q) with conj = 0 - q.xyz.
    check(q);check(v);
    const float ax=sub(add(mul(q[1],v[2]),mul(v[0],q[3])),mul(v[1],q[2]));
    const float ay=sub(add(mul(q[2],v[0]),mul(v[1],q[3])),mul(v[2],q[0]));
    const float az=sub(add(mul(q[0],v[1]),mul(v[2],q[3])),mul(v[0],q[1]));
    const float aw=sub(sub(sub(mul(q[3],0.0F),mul(q[2],v[2])),mul(q[1],v[1])),mul(q[0],v[0]));
    const float cx=sub(0,q[0]),cy=sub(0,q[1]),cz=sub(0,q[2]),cw=q[3];
    RootVector r{};
    r[0]=sub(add(add(mul(ay,cz),mul(ax,cw)),mul(cx,aw)),mul(cy,az));
    r[1]=sub(add(add(mul(az,cx),mul(ay,cw)),mul(cy,aw)),mul(cz,ax));
    r[2]=sub(add(add(mul(ax,cy),mul(az,cw)),mul(cz,aw)),mul(cx,ay));
    r[3]=v[3];
    return r;
}

RootVector camera_place(const RootParentTransform& frame,const RootVector& v) {
    RootVector r=camera_rotate(frame.orientation,v);
    if(r[3]==1.0F) return add3(r,frame.position);
    RootVector s{};
    for(int i=0;i<4;++i) s[i]=mul(frame.position[i],r[3]);
    r=add3(r,s);
    r[3]=s[3];
    return r;
}

std::array<RootVector,3> camera_quaternion_rows(const RootVector& q) {
    check(q);
    const RootVector d{add(q[0],q[0]),add(q[1],q[1]),add(q[2],q[2]),add(q[3],q[3])};
    const RootVector x{mul(q[0],d[0]),mul(q[1],d[0]),mul(q[2],d[0]),mul(q[3],d[0])}; // vf4
    const RootVector y{mul(q[0],d[1]),mul(q[1],d[1]),mul(q[2],d[1]),mul(q[3],d[1])}; // vf5
    const RootVector z{mul(q[0],d[2]),mul(q[1],d[2]),mul(q[2],d[2]),mul(q[3],d[2])}; // vf6
    std::array<RootVector,3> m{};
    m[0]={sub(sub(1.0F,y[1]),z[2]),add(x[1],z[3]),sub(x[2],y[3]),0};
    m[1]={sub(y[0],z[3]),sub(sub(1.0F,x[0]),z[2]),add(y[2],x[3]),0};
    m[2]={add(z[0],y[3]),sub(z[1],x[3]),sub(sub(1.0F,x[0]),y[1]),0};
    return m;
}

std::array<RootVector,3> camera_shake_matrix(const RootVector& e) {
    check(e);
    // 0017dd20: cos / sin of y, x, z (00121688 / 00121928) in that order.
    const float cy=checked(game_cosf(e[1])),sy=checked(game_sinf(e[1]));
    const float cx=checked(game_cosf(e[0])),sx=checked(game_sinf(e[0]));
    const float cz=checked(game_cosf(e[2])),sz=checked(game_sinf(e[2]));
    std::array<RootVector,3> m{};
    m[0]={sub(mul(cz,cy),mul(mul(sz,sy),sx)),add(mul(sz,cy),mul(mul(cz,sy),sx)),mul(-cx,sy),0};
    m[1]={mul(-sz,cx),mul(cz,cx),sx,0};
    m[2]={add(mul(cz,sy),mul(mul(sz,cy),sx)),sub(mul(sz,sy),mul(mul(cz,cy),sx)),mul(cx,cy),0};
    return m;
}

RootVector camera_shake_target(float amplitude,const std::function<std::uint32_t()>& random) {
    checked(amplitude);
    const float x=game_int_to_float(static_cast<std::int32_t>(random()));   // 0017a6fc, 0017a70c
    const float z=game_int_to_float(static_cast<std::int32_t>(random()));   // 0017a708, 0017a71c
    const auto m=camera_shake_matrix({x,0.0F,z,0.0F});                     // 0017de50, 0017dd20
    RootVector t{};
    for(int i=0;i<3;++i) t[i]=add(add(mul(m[0][i],0.0F),mul(m[1][i],amplitude)),mul(m[2][i],0.0F));   // 0013ed60
    t[3]=1.0F;                                                             // 0014b1e0
    return t;
}
CameraRigState make_camera_rig(float fov) {
    checked(fov);
    CameraRigState s;
    const RootVector identity{0,0,0,1};
    s.orientation_210=make_camera_quaternion_curve(identity,{},true,0);
    s.pivot_2e0=vector_curve(0);
    s.look_3a0=vector_curve(0);
    s.heading_460=make_camera_quaternion_curve(identity,{},false,15);
    s.heading_530=make_camera_quaternion_curve(identity,{},false,15);
    s.offset_600=vector_curve(15);
    s.offset_6c0=vector_curve(15);
    s.pitch_780=make_camera_quaternion_curve(identity,{},true,15);
    s.boom_850=vector_curve(15);
    s.boom_910=vector_curve(15);
    s.boom_9d0=vector_curve(15);
    s.fov_a90=scalar_curve(15);
    s.shake_b20=scalar_curve(0);
    s.shake_b58=scalar_curve(0);
    s.shake_ba0=vector_curve(0);
    s.half_fov_60=mul(fov,0.5F);
    s.fov_c64=s.fov_c68=fov;
    s.active_200=false;
    s.reset_201=true;
    return s;
}

void translate_camera_follow_curves(CameraRigState& state,const RootVector& delta) {
    check(delta);
    auto translate=[&](AnimationVectorCurveState curve){
        check(curve.previous);check(curve.current);check(curve.target);
        check(curve.segment.start);check(curve.segment.end);
        for(int i=0;i<4;++i){
            curve.previous[i]=add(curve.previous[i],delta[i]);
            curve.current[i]=add(curve.current[i],delta[i]);
            curve.target[i]=add(curve.target[i],delta[i]);
            curve.segment.start[i]=add(curve.segment.start[i],delta[i]);
            curve.segment.end[i]=add(curve.segment.end[i],delta[i]);
        }
        return curve;
    };
    auto pivot=translate(state.pivot_2e0);
    auto look=translate(state.look_3a0);
    state.pivot_2e0=std::move(pivot);
    state.look_3a0=std::move(look);
}

void update_camera_rig(CameraRigState& s,float elapsed,const CameraCollisionHost* host,const CameraZoomHost* zoom,
    const std::function<std::uint32_t()>* random) {
    if(elapsed==0||!s.active_200) return;
    checked(elapsed);
    if(!(elapsed>0)) throw std::runtime_error("Camera rig elapsed time must be positive");
    const float inverse=checked(1.0F/elapsed);
    auto n=s;
    // Shake: the amplitude curve +b20 (the tremor manager's 002726f0 target)
    // steps with 0017dff0, the delayed curve +b58 with 0017dea0 (nothing in the
    // port writes it: only its rest state is hosted).
    step_camera_scalar_curve(n.shake_b20,elapsed,inverse);
    if(n.shake_b58.current!=0||n.shake_b58.target!=0)
        throw std::runtime_error("unhosted(0x17dea0): the camera shake curve +b58");
    const float amplitude=add(n.shake_b20.current,n.shake_b58.current);   // 0017de90 + 0017de80
    if(!(amplitude<=0)) {
        // 0017a6ec..0017a764: (0, amplitude, 0, 0) by the transpose of the
        // Euler matrix 0017dd20 of (cvt.s.w(00127dd0()), 0, cvt.s.w(00127dd0()))
        // (0013ed60), w = 1 (0014b1e0), as the target of +ba0 (0017dd10).
        if(!random||!*random) throw std::runtime_error("unhosted(0x17a6fc): the camera shake's game random 00127dd0");
        n.shake_ba0.target=camera_shake_target(amplitude,*random);
    }
    retime_animation_vector_curve(n.shake_ba0,0.01F);                 // 0017dd00(0.01)
    step(n.shake_ba0,elapsed);                                        // 0017d8e0
    if(n.reset_201) {
        retime_camera_quaternion_curve(n.orientation_210,0);
        retime_animation_vector_curve(n.pivot_2e0,0);
        retime_animation_vector_curve(n.look_3a0,0);
        retime_camera_quaternion_curve(n.heading_460,0);
        retime_camera_quaternion_curve(n.heading_530,0);
        retime_animation_vector_curve(n.offset_600,0);
        retime_animation_vector_curve(n.offset_6c0,0);
        retime_camera_quaternion_curve(n.pitch_780,0);
        retime_animation_vector_curve(n.boom_850,0);
        retime_animation_vector_curve(n.boom_910,0);
        retime_animation_vector_curve(n.boom_9d0,0);
    }
    step_camera_quaternion_curve(n.heading_460,elapsed,inverse);
    n.heading_530.target=n.heading_460.current;
    step_camera_quaternion_curve(n.heading_530,elapsed,inverse);
    retime_animation_vector_curve(n.offset_600,0.15F);
    step(n.offset_600,elapsed);
    n.offset_6c0.target=n.offset_600.current;
    retime_animation_vector_curve(n.offset_6c0,0.15F);
    step(n.offset_6c0,elapsed);
    step_camera_quaternion_curve(n.pitch_780,elapsed,inverse);
    n.orientation_210.target=camera_quaternion_multiply(n.target_orientation_ad0,n.pitch_780.current);
    n.pivot_2e0.target=n.target_pivot_ae0;
    if(!n.look_c61) n.look_3a0.target=n.pivot_2e0.target;
    step_camera_quaternion_curve(n.orientation_210,elapsed,inverse);
    step(n.pivot_2e0,elapsed);
    step(n.look_3a0,elapsed);

    const bool boom_910_settled=n.boom_910.remaining==0;               // read before +910 steps
    const RootParentTransform rig{n.orientation_210.current,n.pivot_2e0.current};
    const RootVector shake=n.shake_ba0.current;
    CameraBody body;
    auto world_point=[&](const RootVector& local) {
        RootVector p=local;p[3]=1.0F;
        return add3(camera_place(rig,p),shake);
    };
    auto local_point=[&](const RootVector& world) {
        // 0017ccb0 inverse placement; the shake is removed first.
        const auto inverse_rig=invert_specialized_body_placement(rig);
        RootVector p=world;
        const RootVector negative{sub(0,shake[0]),sub(0,shake[1]),sub(0,shake[2]),sub(0,shake[3])};
        p=add3(p,negative);
        return camera_place(inverse_rig,p);
    };
    if(n.body_c60) {
        if(!host||!host->prepare||!host->limit) throw std::runtime_error("Camera collision host required");
        body.forward_10=camera_quaternion_rows(n.orientation_210.current)[1];
        body.orientation_20=n.orientation_210.current;
        body.look_40=n.look_3a0.current;
        body.pivot_50=n.pivot_2e0.current;
        body.points[0]=world_point(n.boom_af0);
        body.points[1]=world_point(n.boom_910.current);
        host->prepare(body);
    }
    if(!n.body_c60) n.boom_850.target=n.boom_af0;
    else {
        const auto limited=host->limit(body,body.points[0]);
        const RootVector local=local_point(limited.point);
        if(!limited.hit) {
            if(boom_910_settled) retime_animation_vector_curve(n.boom_850,0.5F);
            else if(n.boom_850.remaining<=0.067F) retime_animation_vector_curve(n.boom_850,0.067F);
        } else {
            const RootVector& reference=boom_910_settled ? n.boom_850.current : n.boom_850.target;
            const float along=dot3(sub4(local,reference),sub4(local,n.boom_af0));
            retime_animation_vector_curve(n.boom_850,(along>0||n.boom_af0[1]==0) ? 0.067F : 0.5F);
        }
        n.boom_850.target=local;
    }
    step(n.boom_850,elapsed);
    n.boom_910.target=n.boom_850.current;
    step(n.boom_910,elapsed);
    if(!n.body_c60) n.boom_9d0.target=n.boom_910.current;
    else {
        const auto limited=host->limit(body,world_point(n.boom_910.current));
        const RootVector local=local_point(limited.point);
        retime_animation_vector_curve(n.boom_9d0,0.067F);
        if(limited.hit) rebase_animation_vector_current(n.boom_910,local);  // 0017cae0
        n.boom_9d0.target=local;
    }
    step(n.boom_9d0,elapsed);
    if(n.reset_201) {
        settle_camera_quaternion_curve(n.orientation_210);
        settle(n.pivot_2e0);
        settle(n.look_3a0);
        settle_camera_quaternion_curve(n.heading_460);
        settle_camera_quaternion_curve(n.heading_530);
        settle(n.offset_600);
        settle(n.offset_6c0);
        settle_camera_quaternion_curve(n.pitch_780);
        settle(n.boom_850);
        settle(n.boom_910);
        settle(n.boom_9d0);
        n.reset_201=false;
    }
    // View transform sent to +50 vtable +c and kept at +b10.
    const auto rows=camera_quaternion_rows(n.orientation_210.current);
    const RootVector& b=n.boom_9d0.current;
    RootVector offset{};
    for(int i=0;i<3;++i) offset[i]=add(add(mul(rows[0][i],b[0]),mul(rows[1][i],b[1])),mul(rows[2][i],b[2]));
    RootVector position=n.pivot_2e0.current;
    position=add3(position,offset);
    position=add3(position,shake);
    n.matrix={rows[0],rows[1],rows[2],position};
    n.position_b10=position;
    // Field of view: +c64 approaches +c68 at up to 7 * 0.3 rad/s.
    const float limit=std::bit_cast<float>(0x3e99999aU);               // 0048a170 = 0.3
    float delta=sub(n.fov_c68,n.fov_c64);
    delta=std::max(delta,sub(0,limit));
    delta=std::min(delta,limit);
    float fov=add(n.fov_c64,mul(7.0F,mul(delta,elapsed)));             // 0048a178 = 7
    fov=std::max(fov,std::bit_cast<float>(0x393702d3U));
    fov=std::min(fov,std::bit_cast<float>(0x40490cffU));
    n.fov_c64=n.fov_c68=fov;
    float view_fov=n.fov_c64;
    if(n.fov_c64<std::bit_cast<float>(0x3f8efa35U)) {
        // 0017c570: +50 (the static transform component) holds n.matrix;
        // 0020d3e0(+170, +50 +10, +c64), 001811c0(+1c0, camera, elapsed).
        setup_camera_zoom(n.zoom_170,n.matrix,n.fov_c64);
        if(!zoom) throw std::runtime_error("Camera zoom (0017c540) requires the zoom host: the gather 00181260 and the sweep 003bcee0 / 003bced0");
        update_camera_zoom(n.zoom_170,*zoom);
        view_fov=n.zoom_170.fov_8;                                     // lwc1 +178
    }
    n.fov_a90.target=checked(1.0F/checked(1.0F/checked(std::tan(mul(0.5F,view_fov)))));
    retime_camera_scalar_curve(n.fov_a90,0.15F);
    step_camera_scalar_curve(n.fov_a90,elapsed,inverse);
    const float slope=checked(1.0F/checked(1.0F/n.fov_a90.current));
    n.half_fov_60=mul(0.5F,mul(2.0F,checked(std::atan(slope))));
    s=n;
}
}
