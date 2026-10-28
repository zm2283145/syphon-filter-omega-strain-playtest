#include "thrown_projectile.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Thrown projectile: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float div(float a,float b){
    if(b==0.0F)throw std::runtime_error("Thrown projectile: division by zero");
    return check(vu::div(a,b));
}
std::int32_t trunc_word(float v){
    check(v);
    if(!(std::fabs(v)<2147483648.0F))throw std::runtime_error("Thrown projectile: value outside the integer range");
    return static_cast<std::int32_t>(v);
}
RootVector vsub(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=sub(a[i],b[i]);return r;}
RootVector vadd(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=add(a[i],b[i]);return r;}
RootVector vscale(const RootVector& a,float s){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=mul(a[i],s);return r;}
// vmul.xyz, then ACC = x (times vf0.w = 1), + y, + z.
float dot3(const RootVector& a){return add(add(mul(a[0],a[0]),mul(a[1],a[1])),mul(a[2],a[2]));}
// vopmula / vopmsub, W = vf0.w - vf0.w = 0.
RootVector cross(const RootVector& a,const RootVector& b){
    return {sub(mul(a[1],b[2]),mul(b[1],a[2])),sub(mul(a[2],b[0]),mul(b[2],a[0])),sub(mul(a[0],b[1]),mul(b[0],a[1])),0.0F};
}
// VU RSQRT Q = vf0.w / sqrt(|v|^2), vmulq.xyz (W kept). A zero radicand
// gives Q = +FLT_MAX (the VU's division-by-zero result, as the interpreter
// models it), so a zero vector stays zero: a vertical step builds a
// degenerate scan frame in the original (00131c70 tests the raw D.z).
RootVector normalize3(RootVector v){
    const float s=dot3(v);
    const float q=s==0.0F?std::numeric_limits<float>::max():check(vu::rsqrt(s));
    for(std::size_t i=0;i<3;++i)v[i]=mul(v[i],q);
    return v;
}
bool zero3(const RootVector& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}   // 001320b0 (MAC zero flags x, y, z)
// vmulax / vmadday / vmaddaz / vmaddw: rows weighted by v, in row order.
RootVector row_combination(const std::array<RootVector,4>& m,const RootVector& v){
    RootVector r{};
    for(std::size_t i=0;i<4;++i)
        r[i]=add(add(add(mul(m[0][i],v[0]),mul(m[1][i],v[1])),mul(m[2][i],v[2])),mul(m[3][i],v[3]));
    return r;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(what);
    return f;
}
constexpr float mass=2.0F;                    // 0048af30
constexpr float retire_delay=5.0F;            // 0048af28
constexpr float cutoff=-1000.0F;              // 0048af88
constexpr float rest_length=0.2F;             // 0048af50
constexpr float bounce_normal=0.5F;           // 0048af48
constexpr float bounce_tangent=0.5F;          // 0048af58
constexpr float hold7_gravity=10.0F;          // 0048af98
constexpr float hold1_spin_x=-1.0F;           // 0048af38
constexpr float hold1_spin_z=3.0F;            // 0048af40
constexpr float hold7_spin_z=10.0F;           // 0048af90
constexpr float blast_strength=3.0F;          // 0048af20
constexpr float scan_volume=0.4F;             // 0025b4d8 (lui 0x3ecc / ori 0xcccd)
constexpr float landed_radius=15.0F;          // 0025ba74 (lui 0x4170)
constexpr float sound_gap=0.5F;               // 0025ba44 (lui 0x3f00)
constexpr float effect_fraction=std::bit_cast<float>(0x3f666666U);   // 0.9 (0025bfc4)
constexpr float half_axis=0.5F;               // 0025b65c (lui 0x3f00)
ObjectWeakHandle resolved(const ObjectWeakHandle& h,const ThrownProjectileHost& host){
    if(h.object==0)return {0,-1};
    return need(host.valid,"Thrown projectile: no weak-handle check")(h)?h:ObjectWeakHandle{0,-1};
}
// 0025af08..0025b000 and 0025b96c..0025ba3c: one Verlet step with the given
// ratio dt / previous dt, dt^2 and 1 / dt.
void integrate(ProjectileIntegrationState& s,float ratio,float dt_squared,float inverse_dt){
    const auto drift=vadd(s.position,vscale(vsub(s.position,s.previous_position),ratio));
    const auto next=vadd(drift,vscale(s.force,mul(s.inverse_mass,dt_squared)));
    s.previous_position=s.position;
    s.position=next;s.position[3]=1.0F;                                  // 00183000
    const auto old_velocity=s.velocity;
    s.velocity=vscale(vsub(s.position,s.previous_position),inverse_dt);
    s.acceleration=vscale(vsub(s.velocity,old_velocity),inverse_dt);
}
ThrownProjectileEvent event(ThrownProjectileEvent::Kind k){ThrownProjectileEvent e;e.kind=k;return e;}
}

ThrownProjectile construct_thrown_projectile(std::uint64_t object,std::int32_t id_c,const ObjectWeakHandle& owner,
    const std::array<RootVector,4>& frame,const WeaponRecord& record,std::int32_t record_id,const ThrownProjectileTuning& tuning){
    ThrownProjectile p;
    p.object=object;p.id_c=id_c;
    p.motion.position=frame[3];                                          // 0025d890 / 0025d97c: +70 = frame +30
    p.motion.previous_position={0.0F,0.0F,0.0F,1.0F};                    // +80
    p.motion.velocity={};p.motion.acceleration={};                       // +90 / +a0 = 00136080 (zero)
    p.motion.force={};p.motion.launch={};                                // +b0 (+c0, +d0), +f0
    p.mass_60=mass;p.motion.inverse_mass=div(1.0F,mass);                 // 001ec2a0
    p.owner_124=owner.object?owner:ObjectWeakHandle{0,-1};
    const auto hold=record.hold_class_f0();
    if(hold==1)p.spin_110={hold1_spin_x,check(tuning.hold1_spin_y),hold1_spin_z,0.0F};
    else if(hold==7)p.spin_110={0.0F,0.0F,hold7_spin_z,0.0F};
    else p.spin_110={};
    if(hold==1)p.node=frame;                                             // sp+b0, node vt +c (00131c10: a copy)
    else p.node={RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},frame[3]};   // 0025d910..0025d940
    p.motion.first_launch=true;                                          // +e6
    p.motion.retired=false;p.motion.motion_disabled=false;p.stuck_e8=false;
    p.record_120=record_id;
    p.fuse_e0=check(record.real(0xc0));
    p.motion.age=0.0F;p.motion.previous_dt=0.0F;                         // +100 is not written (set by the first step)
    return p;
}

void launch_thrown_projectile(ThrownProjectile& p,const WeaponRecord& record,const RootVector& target,float arc,
    float gravity_z,const ThrownProjectileTuning& tuning){
    ProjectileLaunchTuning t;
    t.gravity=gravity_z;t.type1_minimum_angle=tuning.hold1_minimum_angle;t.type7_gravity_offset=hold7_gravity;
    p.motion.launch=prepare_projectile_launch(p.motion.position,target,arc,record.hold_class_f0(),record.power_104(),t);
}

ThrowArcPreview throw_arc_preview(const RootVector& hand,const RootVector& target,float power,float speed_104,
    const RootVector& gravity,float gravity_z,const ThrownProjectileTuning& tuning){   // 0025cdc0
    ThrowArcPreview r;
    const auto d=normalize3(vsub(target,hand));                                          // 0025ced0..0025cf24
    const auto side=cross(d,RootVector{0.0F,0.0F,1.0F,1.0F});                            // 004f8010
    const auto start=vadd(hand,vscale(side,std::bit_cast<float>(0x3f800000U)));          // 0048afd8
    r.side=vscale(side,div(std::bit_cast<float>(0x3f19999aU),3.0F));                     // 0048afa8 / 3.0
    auto v=solve_projectile_trajectory(start,target,gravity_z,power,tuning.hold1_minimum_angle).initial_velocity;   // 003a8c80
    if(!(dot3(v)<=mul(speed_104,speed_104)))v=vscale(normalize3(v),speed_104);         // 0025d0ac..0025d134
    const float dt=div(1.0F,std::bit_cast<float>(0x41200000U));                          // 0048af80
    const float inverse_dt=div(1.0F,dt);
    (void)inverse_dt;                                                                    // the unread +30 / +40
    const float dt_squared=mul(dt,dt);
    constexpr float inverse_mass=1.0F;                                                   // 001ec2a0(sp+290, 1.0)
    RootVector position=start;                                                           // sp+2a0
    RootVector previous=vsub(start,vscale(v,dt));                                        // sp+2b0
    const auto force=vadd(RootVector{0.0F,0.0F,0.0F,0.0F},vscale(gravity,1.0F));         // sp+2e0 += 004f5430 * +0
    const float target_z=target[2];
    r.points.push_back(position);
    bool rising=true;
    do{
        const auto drift=vsub(vscale(position,2.0F),previous);                           // 0017dc00, 00183380
        const auto next=vadd(drift,vscale(force,mul(inverse_mass,dt_squared)));          // 0017f670, 00134080
        previous=position;
        position=next;position[3]=1.0F;                                                  // 001325e0, 00183000
        rising=!(sub(position[2],previous[2])<0.0F);                                     // sp+1b8 c.olt.s
        r.points.push_back(position);                                                    // 00180bd0
        // PORT-ONLY guard: the original steps until the arc falls below the target (gravity is negative).
        if(r.points.size()>100000)throw std::runtime_error("Throw arc preview (0025cdc0): no end after 100000 steps");
    }while(rising||!(position[2]<=target_z));
    return r;
}

std::vector<ThrowArcVertex> throw_arc_ribbon(const ThrowArcPreview& arc){   // 0025c650
    std::vector<ThrowArcVertex> out;
    if(arc.points.empty())return out;
    const float step=div(sub(1.0F,std::bit_cast<float>(0x3dcccccdU)),static_cast<float>(arc.points.size()));
    const auto byte=[](float c){return static_cast<std::int32_t>(mul(128.0F,c));};      // 0025c9d0: cvt.w.s truncates
    float alpha=std::bit_cast<float>(0x3f800000U);                                       // 0048afc8
    for(const auto& p:arc.points){
        const std::array<std::int32_t,4> rgba{byte(std::bit_cast<float>(0x3e99999aU)),byte(std::bit_cast<float>(0x3f19999aU)),
                                              byte(std::bit_cast<float>(0x3e99999aU)),byte(alpha)};
        out.push_back({vsub(p,arc.side),rgba});
        out.push_back({vadd(p,arc.side),rgba});
        alpha=sub(alpha,step);
    }
    return out;
}

std::array<RootVector,4> projectile_scan_frame(const RootVector& d,const RootVector& origin){
    std::array<RootVector,4> f{};
    const auto side=cross(d,RootVector{0.0F,0.0F,1.0F,1.0F});            // 0025b38c: 001320f0(D, up)
    if(!zero3(side)){
        const auto forward=normalize3(d);                                // 00132060
        const auto x=normalize3(side);                                   // 00132030
        const auto z=cross(x,forward);
        f={x,forward,z,RootVector{0.0F,0.0F,0.0F,1.0F}};                 // 00131f00
    }else if(!zero3(d)){
        // 00131c70: helper (0, 0, 1, 0) when |D.z| < 0.70710677 (0x3f34fdf4), else (1, 0, 0, 0).
        const RootVector helper=std::fabs(d[2])<std::bit_cast<float>(0x3f34fdf4U)?RootVector{0.0F,0.0F,1.0F,0.0F}:RootVector{1.0F,0.0F,0.0F,0.0F};
        const auto n=normalize3(d);
        const auto a=normalize3(cross(d,helper));
        f={a,n,cross(a,n),RootVector{0.0F,0.0F,0.0F,1.0F}};              // 00131e90, +30 = (0, 0, 0, 1)
    }else{
        f={RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},RootVector{0.0F,0.0F,0.0F,1.0F}};   // 00131c40
    }
    f[3]=origin;                                                         // 0025b438: +30 = +80
    return f;
}

void spin_thrown_projectile_node(ThrownProjectile& p,float dt){
    auto w=p.node;w[3]=p.motion.position;                                // sp+940, +30 = +70
    const float ax=mul(p.spin_110[0],dt),ay=mul(p.spin_110[1],dt),az=mul(p.spin_110[2],dt);
    const float cy=check(game_cosf(ay)),sy=check(game_sinf(ay));         // 00140e80 / 00140e70
    const float cx=check(game_cosf(ax)),sx=check(game_sinf(ax));
    const float cz=check(game_cosf(az)),sz=check(game_sinf(az));
    const float cxcy=mul(cx,cy),szcy=mul(sz,cy),szsy=mul(sz,sy);
    const float r10=mul(-sz,cx),r02=mul(-cx,sy),czcy=mul(cz,cy);
    const float r00=sub(czcy,mul(szsy,sx)),r21=sub(szsy,mul(czcy,sx));
    const float czsy=mul(cz,sy);
    const float r20=add(czsy,mul(szcy,sx)),r11=mul(cz,cx),r01=add(szcy,mul(czsy,sx));
    const std::array<RootVector,4> r{RootVector{r00,r01,r02,0.0F},RootVector{r10,r11,sx,0.0F},   // 00140e20
        RootVector{r20,r21,cxcy,0.0F},RootVector{0.0F,0.0F,0.0F,1.0F}};
    for(std::size_t i=0;i<4;++i)p.node[i]=row_combination(w,r[i]);       // 0025bcbc..0025bcf8, node vt +c
}

std::optional<ThrownProjectileDraw> thrown_projectile_draw_0025ac50(const ThrownProjectile& p,const WeaponRecord& record){
    if(p.motion.retired)return std::nullopt;                             // 0025ac64
    ThrownProjectileDraw d;
    d.light_reference=p.node[3];                                         // 003cd420
    d.model=record.word(0x11c);                                          // 0025ad0c
    if(d.model==0)d.model=record.word(0x13c);
    if(d.model!=0)d.matrix=p.node;                                       // 0025ad54: node +10
    return d;
}

std::int32_t thrown_projectile_bounds_model_0025ac20(const WeaponRecord& record){return record.word(0x13c);}

ThrownProjectileStep update_thrown_projectile(ThrownProjectile& p,float dt,RootVector& gravity,
    const WeaponRecord& record,const ThrownProjectileHost& host,bool scan_aim_view_byte){
    ThrownProjectileStep out;
    auto& ev=out.events;
    if(p.motion.retired)return out;                                      // 0025adbc
    check(dt);
    const auto hold=record.hold_class_f0();
    const bool impact=record.projectile_d4()==2;                         // s1
    bool hit=false,detonate=false;                                       // s0, s2
    std::optional<RootVector> impact_point;                              // sp+f50 (written only by a hit)
    if(p.motion.motion_disabled){
        p.motion.age=add(p.motion.age,dt);
    }else{
        ProjectileIntegrationInput in;
        in.dt=dt;in.gravity=gravity;in.launch_force_scale=mass;in.definition_type=hold;in.type7_gravity_offset=hold7_gravity;
        const float previous_dt=p.motion.first_launch?dt:p.motion.previous_dt;
        const auto draft=prepare_projectile_integration(p.motion,in);
        if(draft.boundary!=ProjectileIntegrationBoundary::await_world_continuation||!draft.displacement)
            throw std::runtime_error("Thrown projectile: integration did not reach the world query");
        p.motion=draft.state;
        if(draft.gravity_z_after)gravity[2]=*draft.gravity_z_after;      // 0025b030..0025b044
        const float dt_squared=mul(dt,dt);                               // f23
        const float inverse_dt=div(1.0F,dt);                             // f25
        const float ratio=div(dt,previous_dt);                           // f24
        const RootVector d=*draft.displacement;                          // sp+f40 (= sp+f60)
        if(p.motion.previous_position[2]<cutoff){                        // 0025b048
            ev.push_back(event(ThrownProjectileEvent::Kind::unregister));
            auto r=event(ThrownProjectileEvent::Kind::retire);r.amount=retire_delay;ev.push_back(r);
            p.motion.retired=true;
            return out;
        }
        RootVector origin=p.motion.previous_position;origin[3]=1.0F;     // request +30, +3c = 1
        RootVector displacement=d;displacement[3]=0.0F;                  // request +40, +4c = 0
        auto q=event(ThrownProjectileEvent::Kind::los);q.position=origin;q.vector=displacement;q.object=p.object;q.value=0x10;
        ev.push_back(q);
        const auto los=need(host.los,"Thrown projectile: no LOS manager (004ea0b0)")(origin,displacement);
        if(los){
            p.stuck_e8=false;hit=true;                                   // 0025b170
            impact_point=los->point;
            if(hold==7){
                if(los->object==0)throw std::runtime_error("Thrown projectile: hold-7 hit without a hit object (the original dereferences it)");
                const auto owner=resolved(p.owner_124,host);
                const float amount=game_int_to_float(weapon_band_damage(record,0.0F));   // 0025b1f8..0025b2d4
                const auto type=static_cast<std::int8_t>(record.damage_type_bc());
                auto s=event(ThrownProjectileEvent::Kind::static_damage);s.object=los->object;s.owner=owner.object;s.value=type;s.amount=amount;
                ev.push_back(s);
                need(host.static_damage,"Thrown projectile: no static-object damage (+100)")(los->object,owner.object,type,amount);
            }
            if(dot3(d)<mul(rest_length,rest_length))p.motion.motion_disabled=true;   // 0025b2f0..0025b33c
        }else if(!p.stuck_e8){
            BulletHitRequest req;                                        // 0025b34c..0025b518 (sp+df0)
            req.shooter_0=resolved(p.owner_124,host).object;
            req.frame_4=projectile_scan_frame(d,p.motion.previous_position);
            req.designated_8=0;req.rolled_c=false;req.aim_view_d=scan_aim_view_byte;
            req.subtarget_10=-1;
            req.step_14=check(vu::sqrt(dot3(d)));
            req.record_18=&record;req.travelled_1c=0.0F;
            req.skip_damage_20=hold!=7;req.penetration_24=0;req.volume_28=scan_volume;
            req.statistics_2c=true;req.bullet_type_2d=true;
            auto s=event(ThrownProjectileEvent::Kind::scan);s.position=req.frame_4[3];s.amount=req.step_14;s.value=req.skip_damage_20?1:0;
            ev.push_back(s);
            need(host.scan,"Thrown projectile: no actor scan (0022ba30)")(req);
            if(req.category_30!=0){                                      // 0025b51c
                if(hold==7)p.motion.retired=true;
                hit=true;p.stuck_e8=true;
                p.motion.previous_position=p.motion.position;
                p.motion.velocity={};
                impact_point=row_combination(req.frame_4,RootVector{0.0F,req.distance_34,0.0F,1.0F});
            }
        }
        if(hit&&impact)detonate=true;                                    // 0025b5c8
        else{
            if(hit){
                const auto p0=p.motion.previous_position;                // sp+da0
                p.motion.position=p0;
                if(!p.stuck_e8){
                    // 0025b620..0025b9d4: reflect the step at half speed about the hit plane.
                    const auto n=los->normal;
                    const RootVector h=std::fabs(n[2])<=half_axis?RootVector{0.0F,0.0F,1.0F,1.0F}:RootVector{1.0F,0.0F,0.0F,1.0F};
                    const auto u=normalize3(cross(n,h));
                    const auto v=normalize3(cross(n,u));
                    const auto w=normalize3(cross(u,v));
                    const std::array<RootVector,4> m{u,v,w,RootVector{0.0F,0.0F,0.0F,1.0F}};
                    std::array<RootVector,4> t=m;                        // 0025b840..0025b86c: 3x3 transpose
                    std::swap(t[0][1],t[1][0]);std::swap(t[0][2],t[2][0]);std::swap(t[1][2],t[2][1]);
                    auto local=row_combination(t,d);
                    local[0]=mul(local[0],bounce_tangent);
                    local[1]=mul(local[1],bounce_tangent);
                    local[2]=mul(-local[2],bounce_normal);                 // neg.s, then 0048af48
                    const auto r=row_combination(m,local);
                    p.motion.previous_position=vsub(p0,r);
                    integrate(p.motion,ratio,dt_squared,inverse_dt);
                    if(p.motion.age>sound_gap){                          // 0025ba40 (c.ole.s age, 0.5)
                        auto a=event(ThrownProjectileEvent::Kind::landed_ai_sound);a.object=p.object;a.value=5;a.amount=landed_radius;
                        ev.push_back(a);
                        auto b=event(ThrownProjectileEvent::Kind::bounce_sound);b.object=p.object;b.name=record.text(0x20,16);
                        ev.push_back(b);
                        p.motion.age=0.0F;
                    }
                }
            }
            spin_thrown_projectile_node(p,dt);                           // 0025bb50..0025bd3c
        }
    }
    if(!impact){                                                         // 0025bd60
        p.fuse_e0=sub(p.fuse_e0,dt);
        detonate=p.fuse_e0<=0.0F;
    }
    if(!detonate)return out;
    out.detonated=true;
    const auto point=p.node[3];                                          // 0025bdb8: node +40
    const auto owner=resolved(p.owner_124,host);
    const auto type=record.damage_type_bc();
    if(type==2||type==4||type==13){                                      // 0025be20
        std::int32_t inner=trunc_word(record.real(0xc4)),outer=trunc_word(record.real(0xc8));
        if(type==4){inner/=2;outer/=2;}                                  // sra with the +1 for negatives: toward zero
        ProjectileBlastCall b;b.owner=owner;b.damage=record.damage_mid_64();b.point=point;b.strength=blast_strength;
        b.inner=game_int_to_float(inner);b.outer=game_int_to_float(outer);
        auto e=event(ThrownProjectileEvent::Kind::explosion);e.position=point;e.object=owner.object;e.value=b.damage;e.vector={b.strength,b.inner,b.outer,0.0F};
        ev.push_back(e);
        need(host.explode,"Thrown projectile: no explosion (0021a200)")(b);
        auto s=event(ThrownProjectileEvent::Kind::screen_effect);s.position=point;ev.push_back(s);
        const auto id=need(host.effect_id,"Thrown projectile: no effect id lookup (00236f20)")(0x25);
        if(id>=0){
            // sp+f50 is written only by a hit in this step; a fuse detonation without one reads stale stack
            // on the EE (presentation only: the effect's position). Port substitute (132zf7): the
            // detonation point, so the effect sits at prev + 0.9 (point - prev).
            const RootVector at=impact_point?*impact_point:point;
            auto f=event(ThrownProjectileEvent::Kind::effect);f.value=id;f.amount=5.0F;
            f.position=vadd(p.motion.previous_position,vscale(vsub(at,p.motion.previous_position),effect_fraction));
            ev.push_back(f);
        }
    }
    if(owner.object){                                                    // 0025c048
        auto a=event(ThrownProjectileEvent::Kind::detonation_ai_sound);a.object=owner.object;a.position=point;a.value=4;a.amount=record.real(0xfc);
        ev.push_back(a);
    }
    if(type>=3&&type<=8){                                                // jump table 004a7cc0
        ProjectileAreaCall c;c.owner=owner;c.record=p.record_120;
        if(type==3){c.point=point;c.flag=true;}
        else if(hit){
            if(!impact_point)throw std::runtime_error("Thrown projectile: hit without an impact point");
            c.point=vadd(p.motion.previous_position,vscale(vsub(*impact_point,p.motion.previous_position),effect_fraction));
            c.flag=false;
        }else{c.point=point;c.flag=false;}
        auto e=event(ThrownProjectileEvent::Kind::area);e.position=c.point;e.object=owner.object;e.value=c.flag?1:0;
        ev.push_back(e);
        need(host.area,"Thrown projectile: no area object (0025a2d0)")(c);
    }
    if(record.impact_effect_d8()!=-1){                                   // 0025c500
        auto e=event(ThrownProjectileEvent::Kind::impact_effect);e.value=record.impact_effect_d8();e.position=point;ev.push_back(e);
    }
    auto snd=event(ThrownProjectileEvent::Kind::detonation_sound);snd.object=p.object;
    snd.name=hold==7?std::string("T_STAR_ROCK"):record.weapon_class_30()==8?record.text(0x10,16):std::string("G_GREN_EXP");
    ev.push_back(snd);
    ev.push_back(event(ThrownProjectileEvent::Kind::unregister));
    auto r=event(ThrownProjectileEvent::Kind::retire);r.amount=retire_delay;ev.push_back(r);
    p.motion.retired=true;
    return out;
}

InventoryLaunchResult launch_inventory_projectile(WeaponInventory& inv,const WeaponDatabase& db,
    const InventoryLaunchInput& in,float gravity_z,const ThrownProjectileTuning& tuning){
    if(inv.current_84>=inv.slots.size())throw std::runtime_error("Inventory launch: no held slot");
    auto& slot=inv.slots[inv.current_84];
    const auto& record=db.record(slot.item);
    InventoryLaunchResult result;
    result.events.push_back({InventoryLaunchEvent::Kind::detach_slot_0,in.owner.object,0.0F,false});   // 0038c7c0
    result.projectile=construct_thrown_projectile(in.object,in.id_c,in.owner,in.hand_frame,record,
        static_cast<std::int32_t>(db.index(slot.item)),tuning);
    RootVector target{};
    if(in.target_position)target=*in.target_position;                    // 00141fe8: target +e4
    else{
        target=in.point;
        if(target[3]==0.0F){target[3]=1.0F;target=row_combination(in.actor_matrix,target);}   // 00141f70..00141fc8
    }
    launch_thrown_projectile(result.projectile,record,target,in.arc,gravity_z,tuning);
    result.target=target;
    if(in.ai_controller){                                                // 00142010
        result.events.push_back({InventoryLaunchEvent::Kind::fire_notice,in.target_object,0.0F,false});
        result.events.push_back({InventoryLaunchEvent::Kind::ai_sound,in.owner.object,record.noise_radius_b8(),false});
    }
    if(!inv.infinite_reserve_99){slot.spare-=1;result.events.push_back({InventoryLaunchEvent::Kind::spare_used,0,0.0F,false});}
    if(record.hold_class_f0()==7)
        result.events.push_back({InventoryLaunchEvent::Kind::star_statistics,0,0.0F,in.point[3]==1.0F});
    return result;
}

}
