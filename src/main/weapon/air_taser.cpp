#include "air_taser.hpp"
#include "game_libm.hpp"
#include "player_targeting.hpp"
#include "projectile_candidates.hpp"
#include "thrown_projectile.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include "weapon_discharge.hpp"
#include <bit>
#include <cfloat>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Air taser: "+what);}
// Interpreter model (tools/ee/ee.py): binary32, denormal results flushed to
// zero; results must stay finite.
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){
    if(!std::isfinite(v))fail("value not finite");
    return ftz(v);
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
// div.s: x / 0 gives +-FLT_MAX on the EE.
float fdiv(float a,float b){
    if(b==0.0F)return (a!=0.0F&&std::signbit(a)!=std::signbit(b))?-FLT_MAX:FLT_MAX;
    return check(a/b);
}
// mula.s / madd.s: the product and the sum rounded to binary32, the result flushed.
float fmadd(float acc,float s,float t){const float p=s*t;return check(acc+p);}
// VU: results flushed (sign kept); a vmadd product is rounded, not flushed.
float vmul(float a,float b){return check(a*b);}
float vmadd(float acc,float s,float t){const float p=s*t;return check(acc+p);}
RootVector vadd(const RootVector& a,const RootVector& b){return {add(a[0],b[0]),add(a[1],b[1]),add(a[2],b[2]),add(a[3],b[3])};}
RootVector vsub(const RootVector& a,const RootVector& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
RootVector vscale(const RootVector& a,float s){return {vmul(a[0],s),vmul(a[1],s),vmul(a[2],s),vmul(a[3],s)};}
// vmul.xyz, then the sum through the accumulator (x, y, z).
float vsum_squares(const RootVector& d){
    const float x=vmul(d[0],d[0]),y=vmul(d[1],d[1]),z=vmul(d[2],d[2]);
    return vmadd(vmadd(vmul(1.0F,x),1.0F,y),1.0F,z);
}
// vmulax / vmadday / vmaddaz / vmaddw.xyzw.
RootVector transform(const AirTaserMatrix& m,const RootVector& v){
    RootVector r{};
    for(std::size_t j=0;j<4;++j)r[j]=vmadd(vmadd(vmadd(vmul(m[0][j],v[0]),m[1][j],v[1]),m[2][j],v[2]),m[3][j],v[3]);
    return r;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)fail(std::string(what)+" is not hosted");
    return f;
}
constexpr std::array<std::int8_t,21> node_table{0,2,5,9,14,8,13,17,19,10,15,18,20,1,4,7,12,3,6,11,16};   // 0049d020
std::int32_t node_of(std::int32_t bone){
    const auto b=static_cast<std::uint8_t>(bone);
    if(b>=node_table.size())fail("bone outside the node table 0049d020");
    return node_table[b];
}
constexpr float flight_speed=100.0F;     // 0048b0f0
constexpr float fire_droop=0.004F;       // 0048b0f8
constexpr float aim_clamp=0.03F;         // 0048b100 / 0048b108
constexpr float stiffness=300.0F;        // 0048b110
constexpr float knockout_time=2.0F;      // 0048b118
constexpr float burn_time=3.0F;          // 0048b120
constexpr float half=0.5F;               // 0048b128 (also the free-aim limit)
constexpr float droop_cap=0.25F;         // 0048b130
constexpr float small_force=7.0F;        // 0048b138
constexpr float reel_wobble_limit=50.0F; // 0048b140
constexpr float wobble_amplitude=6.0F;   // 0048b148
constexpr float wobble_period_lo=5.0F;   // 0048b150
constexpr float wobble_period_hi=10.0F;  // 0048b158
constexpr float wobble_y_damping=0.9F;   // 0048b160
constexpr std::int32_t reel_pops=3;      // 0048b168
constexpr std::int32_t wobble_divisor=10;// 0048b170
constexpr float width_factor=4.0F;       // 0048b178
constexpr float fire_light_range=5.0F;   // 0048b180
constexpr float shock_light_range=10.0F; // 0048b188
constexpr float spark_speed=13.0F;       // 0048b190
constexpr float spark_period=0.6F;       // 0048b198
constexpr float particle_weight=0.8F;    // 0048b1a0 (passed by the host)
constexpr float smoothing_decay=0.99F;   // 0048b1b0
constexpr float min_width=0.8F;          // 0048b1b8
constexpr float shock_smoothing=0.7F;    // 0048b1c0
constexpr float los_pull_back=2.0F;      // 0048b1c8
constexpr float wobble_start=std::bit_cast<float>(0x4048f5c3U);   // 3.14
constexpr float segment_width=std::bit_cast<float>(0x3f266666U);  // 0.65
constexpr float lethal_damage=10000.0F;

float unit(const AirTaserHost& h){                       // float(rand) / 2^31
    return fdiv(game_int_to_float(static_cast<std::int32_t>(need(h.random,"00127dd0")())),2147483648.0F);
}
AirTaserObjectFacts facts(const AirTaserHost& h,std::uint64_t o){return need(h.facts,"the object fields")(o);}
// +2c resolved against +30 (the weak handle).
std::uint64_t victim(const AirTaserObject& obj,const AirTaserHost& h){
    if(!obj.victim_2c)return 0;
    return facts(h,obj.victim_2c).id_c==obj.victim_id_30?obj.victim_2c:0;
}
LevelLight& light(const AirTaserHost& h,std::size_t index){
    if(!h.lights)fail("the light manager 00535d20 is not hosted");
    if(index>=h.lights->point.size())fail("light index outside the point pool");
    return h.lights->point[index];
}
void dirty(const AirTaserHost& h){h.lights->dirty_42=1;}
// vtable +8: 00144a60(light, 1) (id -1, vcall +30 00144980, released vtable, 00373d70).
void release_light(AirTaserObject& obj,const AirTaserHost& h){
    if(!obj.light_c)return;
    auto& l=light(h,*obj.light_c);
    l.id=-1;
    l.enabled=false;
    l.vtable=level_light_vtable_released;
    dirty(h);
    obj.light_c.reset();
}
// vtable +28: 00144b10 (vsub.xyzw; moves when a MAC zero flag is clear).
void move_light(const AirTaserHost& h,std::size_t index,const RootVector& position){
    auto& l=light(h,index);
    bool moved=false;
    for(std::size_t i=0;i<4;++i){
        const float d=l.position[i]-position[i];
        if(!std::isfinite(d))fail("light position not finite");
        if(std::fpclassify(d)==FP_SUBNORMAL)fail("denormal light position difference (VU zero flag not established)");
        if(d!=0.0F)moved=true;
    }
    if(moved){l.position=position;dirty(h);}
}
void set_light_colour(const AirTaserHost& h,std::size_t index,float r,float g){
    auto& l=light(h,index);
    l.colour={r,g,1.0F,0.0F};
    l.weight=fdiv(1.0F,vsum_squares(l.colour));
}
AirTaserSegment segment(const RootVector& pos){
    AirTaserSegment s;
    s.pos=pos;
    s.width_30=segment_width;s.width_34=segment_width;s.width_38=1.0F;s.width_3c=1.0F;
    return s;
}
// The frame both 00264530 / 00264720 and the fire build: 0025b38c's
// construction over last - first, row 3 the first segment.
void wire_frame(AirTaserObject& obj,const RootVector& origin){
    obj.wire_10.at(0).pos=origin;
    const auto d=vsub(obj.wire_10.back().pos,obj.wire_10[0].pos);
    obj.frame_60=projectile_scan_frame(d,obj.wire_10[0].pos);
}
// w: sp+6c, left by 00264720 just before: its last - first (vsub.xyzw) .w.
void wobble(AirTaserObject& obj,float dt,float w){                      // 00262f30
    const float step=fdiv(wobble_start,obj.wobble_c4);
    float angle=step;
    const float ax=mul(obj.wobble_c8,dt),az=mul(obj.wobble_cc,dt);
    const auto count=static_cast<std::int32_t>(obj.wire_10.size());
    const float divisor=game_int_to_float(std::max(wobble_divisor,count));
    for(std::int32_t i=1;i<count;++i){
        const float sx=check(game_sinf(angle));
        const float x=mul(mul(ax,fdiv(game_int_to_float(i),divisor)),sx);
        const float sz=check(game_sinf(angle));
        const float z=mul(mul(az,fdiv(game_int_to_float(i),divisor)),sz);
        auto& s=obj.wire_10[static_cast<std::size_t>(i)];
        s.local=vadd(s.local,RootVector{x,0.0F,z,w});
        angle=add(angle,step);
        s.local[1]=mul(s.local[1],wobble_y_damping);
    }
}
void reel(AirTaserObject& obj,float dt,const AirTaserContext& ctx,const AirTaserHost& h){   // 00262e00
    (void)ctx;
    auto s1=static_cast<std::int32_t>(obj.wire_10.size());
    for(std::int32_t popped=0;s1!=0&&popped<reel_pops;++popped,--s1)obj.wire_10.pop_back();
    if(s1>=2){
        const auto m=need(h.controller_matrix,"the controller's vcall +64")(false);
        air_taser_world_to_local(obj,m[3]);
        const float w=sub(obj.wire_10.back().pos[3],obj.wire_10[0].pos[3]);
        if(game_int_to_float(s1)<reel_wobble_limit)wobble(obj,dt,w);
        air_taser_local_to_world(obj,m[3]);
    }
    air_taser_smooth(obj,1.0F,true);
    if(s1==0)obj.state_8=0;
}
// 00264980(obj, victim, bone, &hit).
void begin_shock(AirTaserObject& obj,std::uint64_t v,std::int32_t bone,const RootVector& hit,const AirTaserContext& ctx,
    const AirTaserHost& h){
    obj.state_8=2;
    obj.shock_b4=0.0F;
    obj.knockout_bd=false;
    obj.lethal_bc=false;
    obj.victim_2c=v;
    obj.victim_id_30=v?facts(h,v).id_c:-1;
    obj.spark_b8=lethal_damage;
    obj.bone_3c=bone;
    obj.hit_40=hit;
    need(h.effect,"00185d40")(victim(obj,h),2,1,1.0F);
    need(h.sound,"0020f990")(air_taser_buzz_sound,victim(obj,h));
    const auto position=need(h.position,"001ae3a0 / 001a91a0")(v);
    need(h.noise,"0016b420")(ctx.owner_id,position,ctx.noise_fc);
}
void attach(AirTaserObject& obj,std::uint64_t v,std::int32_t bone,const AirTaserContext& ctx,const AirTaserHost& h){   // 00264bb0
    RootVector hit{};
    if(bone>=0){
        const auto m=need(h.node_matrix,"00133ed0")(v,node_of(bone));
        ProjectileQueryMatrix q{};
        for(std::size_t i=0;i<16;++i)q[i]=m[i/4][i%4];
        const auto inv=prepare_projectile_query_frame(q);
        AirTaserMatrix mi{};
        for(std::size_t i=0;i<16;++i)mi[i/4][i%4]=inv[i];
        hit=transform(mi,obj.frame_60[3]);
    }
    if(!obj.no_target_34)need(h.camera_target,"00131540")(v,ctx.owner);
    need(h.stun_action,"the stun action")(v,1);
    begin_shock(obj,v,bone,hit,ctx,h);
}
void flight(AirTaserObject& obj,float dt,const AirTaserContext& ctx,const AirTaserHost& h){   // 00264e10
    if(ctx.owned_14&&air_taser_los(obj,ctx,h)){air_taser_release(obj,ctx,h);return;}
    if(!victim(obj,h))obj.no_target_34=true;
    if(!obj.no_target_34)obj.aim_a0=need(h.target_point,"the target's vcall +c0")(victim(obj,h),obj.subtarget_38);
    TargetingMatrix frame{};
    for(std::size_t k=0;k<4;++k)frame[k]=obj.frame_60[k];
    RootVector angles=target_angles(frame,obj.aim_a0).angles;
    if(std::fabs(angles[0])<=half&&std::fabs(angles[2])<=half){
        angles[0]=std::min(std::max(angles[0],-aim_clamp),aim_clamp);
        angles[2]=std::min(std::max(angles[2],-aim_clamp),aim_clamp);
    }else angles=obj.angles_50;
    obj.angles_50=angles;
    {
        ShotFrame f{},src{};
        for(std::size_t k=0;k<4;++k)src[k]=obj.frame_60[k];
        f=src;
        rotate_shot_frame(f,src,angles[0],0.0F,angles[2]);
        for(std::size_t k=0;k<3;++k)obj.frame_60[k]=f[k];
    }
    float step=mul(dt,flight_speed);
    BulletHitRequest req;
    if(ctx.owned_14){
        req.shooter_0=ctx.owner;
        for(std::size_t k=0;k<4;++k)req.frame_4[k]=obj.frame_60[k];
        req.designated_8=0;req.rolled_c=false;req.aim_view_d=false;
        req.subtarget_10=-1;req.step_14=step;req.record_18=ctx.record;req.travelled_1c=0.0F;
        req.skip_damage_20=true;req.penetration_24=0;req.volume_28=std::bit_cast<float>(0x3e4ccccdU);
        req.statistics_2c=true;req.bullet_type_2d=true;
        need(h.scan,"0022ba30")(req);
        if(req.category_30)step=req.distance_34;
    }
    obj.travelled_b0=add(obj.travelled_b0,step);
    const auto mid=transform(obj.frame_60,RootVector{0.0F,fdiv(step,2.0F),0.0F,1.0F});
    const auto tip=transform(obj.frame_60,RootVector{0.0F,step,0.0F,1.0F});
    obj.wire_10.push_back(segment(mid));
    obj.wire_10.push_back(segment(tip));
    obj.frame_60[3]=tip;
    need(h.update_effect,"003a2400")(obj.particle_c0,obj.frame_60,dt);
    if(ctx.owned_14&&req.category_30&&req.victim_38){
        const auto hit=req.victim_38;
        obj.victim_2c=0;
        obj.victim_id_30=-1;
        const auto life=need(h.life,"the hit object's vcall +108")(hit);
        const auto f=facts(h,hit);
        if(life==3||life==2||!f.actor_4c)air_taser_release(obj,ctx,h);
        else{
            const bool own=!obj.no_target_34&&hit==victim(obj,h)&&obj.subtarget_38==-1;
            const auto player=need(h.player,"the hit object's controller vcall +54")(hit);
            if((player&&*player)||f.state_320c==2||f.state_320c==0x1a||f.invulnerable_33a4||f.byte_346a||
               air_taser_los(obj,ctx,h))
                air_taser_release(obj,ctx,h);
            else if((own||req.category_30==3)&&f.flak_33a0){
                need(h.flak,"00242fa0")(hit);
                air_taser_release(obj,ctx,h);
            }else attach(obj,hit,req.node_3c,ctx,h);
        }
    }
    const auto m=need(h.controller_matrix,"the controller's vcall +64")(false);
    obj.wire_10.at(0).pos=m[3];
    if(obj.light_c)move_light(h,*obj.light_c,tip);
    air_taser_smooth(obj,1.0F,true);
    if(ctx.owned_14&&obj.state_8==1&&!(obj.travelled_b0<=game_int_to_float(ctx.range_3c))){
        obj.victim_2c=0;
        obj.victim_id_30=-1;
        air_taser_release(obj,ctx,h);
    }
}
void shock(AirTaserObject& obj,float dt,const AirTaserContext& ctx,const AirTaserHost& h){   // 00263890
    if(!victim(obj,h)||(ctx.owned_14&&air_taser_los(obj,ctx,h))||obj.wire_10.empty()){
        air_taser_release(obj,ctx,h);
        return;
    }
    if(obj.bone_3c>=0){
        std::uint64_t o=obj.victim_2c;
        if(o&&!need(h.registered,"00174080")(o,obj.victim_id_30))o=0;
        const auto m=need(h.node_matrix,"00133ed0")(o,node_of(obj.bone_3c));
        obj.wire_10.back().pos=transform(m,obj.hit_40);
    }
    const auto m=need(h.controller_matrix,"the controller's vcall +64")(false);
    air_taser_world_to_local(obj,m[3]);
    air_taser_physics(obj,true,dt);
    const auto count=obj.wire_10.size();
    for(std::size_t i=1;i<count;++i){
        auto& s=obj.wire_10[i];
        s.local=vadd(s.local,vscale(s.vel,dt));
    }
    air_taser_local_to_world(obj,m[3]);
    air_taser_smooth(obj,shock_smoothing,false);
    const float f=fdiv(game_int_to_float(static_cast<std::int32_t>(need(h.random,"00127dd0")())),add(1.0F,2147483648.0F));
    if(obj.light_c){
        light(h,*obj.light_c).outer=shock_light_range;
        set_light_colour(h,*obj.light_c,f,f);
    }
    obj.shock_b4=add(obj.shock_b4,dt);
    obj.spark_b8=add(obj.spark_b8,dt);
    for(std::size_t k=0;k<obj.sparks_20.size();){
        auto& sp=obj.sparks_20[k];
        sp.time=add(sp.time,mul(dt,spark_speed));
        const auto n=static_cast<std::int32_t>(obj.wire_10.size());
        if(!(sp.time<game_int_to_float(n-1))){
            need(h.destroy_effect,"003a2ba0")(sp.effect,0x229);
            obj.sparks_20.erase(obj.sparks_20.begin()+static_cast<std::ptrdiff_t>(k));
            continue;
        }
        auto trunc=[](float v){
            if(!(std::fabs(v)<2147483648.0F))fail("cvt.w.s outside the int range");
            return static_cast<std::int32_t>(v);
        };
        const auto i0=trunc(sp.time),i1=trunc(add(1.0F,sp.time));
        const float frac=sub(sp.time,game_int_to_float(i0));
        if(i0<0||i1>=n)fail("spark position outside the wire");
        const auto b=vscale(obj.wire_10[static_cast<std::size_t>(i1)].pos,frac);
        const auto a=vscale(obj.wire_10[static_cast<std::size_t>(i0)].pos,sub(1.0F,frac));
        auto mm=m;
        mm[3]=vadd(a,b);
        sp.matrix=mm;
        need(h.update_effect,"003a2400")(sp.effect,mm,dt);
        ++k;
    }
    if(!(obj.spark_b8<=spark_period)){
        obj.spark_b8=0.0F;
        const auto e=need(h.create_effect,"0036b180 / 003a2dd0")(0x23b,m);
        obj.sparks_20.push_back(AirTaserSpark{e,0.0F,m});
    }
    if(obj.lethal_bc)return;
    if(!(obj.shock_b4<=burn_time)){
        const auto v=victim(obj,h);
        need(h.sound,"0020f990")(air_taser_burn_sound,v);
        need(h.sound,"0020f990")(air_taser_hiss_sound,victim(obj,h));
        AirTaserMatrix burn{RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},RootVector{0.0F,0.0F,1.0F,0.0F},
                            RootVector{}};
        burn[3]=need(h.node_position,"00132910")(victim(obj,h),node_table[0]);
        need(h.burn,"00232120")(burn);
        need(h.effect,"00185d40")(victim(obj,h),0,1,1.0F);
        obj.lethal_bc=true;
    }else if(!(obj.shock_b4<=knockout_time))obj.knockout_bd=true;
}
}

bool air_taser_los(const AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& h){   // 00265910
    if(obj.wire_10.size()<2)return false;
    const auto first=obj.wire_10[0].pos;
    const auto& last=obj.wire_10.back().pos;
    const auto d=vsub(last,first);
    const float len=vu::sqrt(vsum_squares(d));
    const auto pull=vscale(vscale(d,los_pull_back),fdiv(1.0F,len));
    auto start=vsub(first,pull);
    auto disp=vsub(last,start);
    start[3]=1.0F;
    disp[3]=0.0F;
    return need(h.los,"the LOS service 004ea0b0")(start,disp,ctx.owner);
}

void air_taser_smooth(AirTaserObject& obj,float factor,bool halfway){   // 00265790
    const auto count=static_cast<std::int32_t>(obj.wire_10.size());
    std::int32_t t2=count-1;
    if(halfway){
        const float v=mul(game_int_to_float(count-1),half);
        t2=static_cast<std::int32_t>(v);
    }
    const float f4=game_int_to_float(t2-1);
    for(std::int32_t i=1;i<t2;++i){
        const auto& prev=obj.wire_10[static_cast<std::size_t>(i-1)].pos;
        const auto& next=obj.wire_10[static_cast<std::size_t>(i+1)].pos;
        const auto avg=vscale(vadd(prev,next),half);
        float f2=halfway?fdiv(game_int_to_float(t2-i),f4):1.0F;
        f2=mul(f2,smoothing_decay);
        const float f6=mul(factor,f2);
        const float g=sub(1.0F,f6);
        auto& p=obj.wire_10[static_cast<std::size_t>(i)].pos;
        p[0]=fmadd(check(avg[0]*f6),g,p[0]);
        p[1]=fmadd(check(g*p[1]),avg[1],f6);
        p[2]=fmadd(check(g*p[2]),avg[2],f6);
    }
}

void air_taser_physics(AirTaserObject& obj,bool flag,float dt){   // 00264370
    const float k=mul(stiffness,dt);
    const auto a2=static_cast<std::int32_t>(obj.wire_10.size())-1;
    if(a2<2)return;
    auto sign=[](float v){return v<=0.0F?-1:1;};
    for(std::int32_t i=1;i<a2;++i){
        auto& s=obj.wire_10[static_cast<std::size_t>(i)];
        float fx=mul(k,-s.local[0]);
        if(sign(s.vel[0])!=sign(fx))fx=mul(fx,2.0F);
        if(flag&&std::fabs(fx)<half)fx=mul(fx,small_force);
        const float w=std::min(std::max(std::fabs(mul(fx,width_factor)),min_width),1.0F);
        s.width_34=w;
        s.width_30=w;
        s.vel[0]=add(s.vel[0],fx);
        float fz=mul(k,-s.local[2]);
        if(sign(s.vel[2])!=sign(fz))fz=mul(fz,2.0F);
        if(flag&&std::fabs(fz)<half)fz=mul(fz,small_force);
        s.vel[2]=add(s.vel[2],fz);
    }
}

void air_taser_local_to_world(AirTaserObject& obj,const RootVector& origin){   // 00264530
    wire_frame(obj,origin);
    for(auto& s:obj.wire_10)s.pos=transform(obj.frame_60,s.local);
}

void air_taser_world_to_local(AirTaserObject& obj,const RootVector& origin){   // 00264720
    wire_frame(obj,origin);
    ProjectileQueryMatrix q{};
    for(std::size_t i=0;i<16;++i)q[i]=obj.frame_60[i/4][i%4];
    const auto inv=prepare_projectile_query_frame(q);
    AirTaserMatrix m{};
    for(std::size_t i=0;i<16;++i)m[i/4][i%4]=inv[i];
    for(auto& s:obj.wire_10)s.local=transform(m,s.pos);
}

void air_taser_fire(AirTaserObject& obj,std::uint64_t target,bool aim_view,std::int32_t subtarget,const RootVector& point,
    const AirTaserContext& ctx,const AirTaserHost& h){   // 00265b90
    (void)ctx;
    obj.state_8=1;
    obj.aim_a0=point;
    obj.angles_50[0]=0.0F;obj.angles_50[1]=0.0F;obj.angles_50[2]=0.0F;
    obj.frame_60=need(h.controller_matrix,"the controller's vcall +64")(false);
    const RootVector hand=obj.frame_60[3];
    auto d=vsub(obj.aim_a0,hand);
    const float s=vsum_squares(d);
    const float len=vu::sqrt(s);
    const float droop=std::min(mul(fire_droop,s),mul(droop_cap,len));
    d[2]=add(d[2],droop);
    obj.frame_60=projectile_scan_frame(d,hand);
    obj.frame_60[3]=hand;
    need(h.positional_sound,"0020f860")(air_taser_fire_sound,hand);
    obj.wire_10.clear();
    obj.wire_10.push_back(segment(hand));
    obj.travelled_b0=0.0F;
    if(target&&!facts(h,target).actor_4c)target=0;
    obj.victim_2c=target;
    obj.victim_id_30=target?facts(h,target).id_c:-1;
    obj.subtarget_38=subtarget;
    obj.no_target_34=aim_view||!victim(obj,h);
    if(!obj.light_c){                                                   // 00373e70(00535d20, 4)
        if(!h.lights)fail("the light manager 00535d20 is not hosted");
        auto& p=h.lights->point;
        for(std::size_t i=0;i<p.size();++i){
            if(p[i].id!=-1)continue;
            p[i].id=static_cast<std::int32_t>(i+1);
            obj.light_c=i;
            break;
        }
        if(obj.light_c){                                                // 00179540 / 00179530 / 00179520
            auto& l=p[*obj.light_c];
            l.intensity=0.0F;l.enabled=false;l.aux=3;l.type=4;
            l.colour={0.0F,0.0F,0.0F,0.0F};
            l.position=obj.wire_10.back().pos;
            l.weight=FLT_MAX;
            l.vtable=level_light_vtable_point;
            l.inner=0.0F;
            l.outer=fire_light_range;
            dirty(h);
        }
    }
    if(obj.light_c){
        auto& l=light(h,*obj.light_c);
        l.outer=fire_light_range;
        set_light_colour(h,*obj.light_c,half,half);
        if(!l.enabled){l.enabled=true;dirty(h);}                         // vcall +2c: 00144b80
    }
    obj.particle_c0=need(h.create_effect,"0036b180 / 003a2dd0")(0xc6,obj.frame_60);
}

bool air_taser_entry(AirTaserObject& obj,std::uint64_t target,bool aim_view,std::int32_t subtarget,
    const AirTaserContext& ctx,const AirTaserHost& h){   // 00266200
    if(obj.state_8!=0)return false;
    RootVector point{0.0F,0.0F,0.0F,1.0F};
    if(target&&!aim_view)point=need(h.target_point,"the target's vcall +c0")(target,subtarget);
    else{
        const float range=aim_view?need(h.aim_range,"the aim range (inventory +f4)")():game_int_to_float(ctx.range_3c);
        const auto m=need(h.controller_matrix,"the controller's vcall +64")(aim_view);
        point=transform(m,RootVector{0.0F,range,0.0F,1.0F});
    }
    air_taser_fire(obj,target,aim_view,subtarget,point,ctx,h);
    return true;
}

void air_taser_update(AirTaserObject& obj,float dt,const AirTaserContext& ctx,const AirTaserHost& h){   // 00262d90
    switch(obj.state_8){
    case 1:flight(obj,dt,ctx,h);break;
    case 2:shock(obj,dt,ctx,h);break;
    case 3:reel(obj,dt,ctx,h);break;
    default:break;
    }
}

void air_taser_release(AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& h){   // 00263080
    if(obj.state_8==0)return;
    if(const auto v=victim(obj,h)){
        need(h.effect,"00185d40")(v,2,0,1.0F);
        need(h.effect,"00185d40")(victim(obj,h),1,0,1.0F);
        need(h.stun_action,"the stun action")(victim(obj,h),0);
        need(h.stop_sound,"0020eba0")(air_taser_buzz_sound,victim(obj,h));
        need(h.stop_sound,"0020eba0")(air_taser_hiss_sound,victim(obj,h));
    }
    for(const auto& sp:obj.sparks_20)need(h.destroy_effect,"003a2ba0")(sp.effect,0x295);
    obj.sparks_20.clear();
    if(obj.particle_c0){
        need(h.destroy_effect,"003a2ba0")(obj.particle_c0,0x29b);
        obj.particle_c0=0;
    }
    release_light(obj,h);
    if(obj.state_8==2){
        need(h.camera_release,"00131510")();
        if(const auto v=victim(obj,h)){
            if(obj.lethal_bc)need(h.damage,"the victim's vcall +100")(v,ctx.owner,1,4,lethal_damage);
            else if(obj.knockout_bd){
                if(ctx.owned_14){
                    const auto p=need(h.player,"the owner's controller vcall +54")(ctx.owner);
                    if(p&&*p)need(h.statistic,"00333f10")();
                }
                need(h.damage,"the victim's vcall +100")(victim(obj,h),ctx.owner,1,7,0.0F);
            }
        }
    }
    obj.state_8=3;
    obj.wobble_c4=add(wobble_period_lo,mul(sub(wobble_period_hi,wobble_period_lo),unit(h)));
    obj.wobble_c8=add(0.0F,mul(sub(wobble_amplitude,0.0F),unit(h)));
    obj.wobble_cc=sub(wobble_amplitude,obj.wobble_c8);
    if(need(h.random,"00127dd0")()&1U)obj.wobble_c8=-obj.wobble_c8;
    if(need(h.random,"00127dd0")()&1U)obj.wobble_cc=-obj.wobble_cc;
    if(obj.wire_10.size()>=2)need(h.noise,"0016b420")(ctx.owner_id,obj.wire_10.back().pos,ctx.noise_fc);
}

void air_taser_reset(AirTaserObject& obj,const AirTaserContext& ctx,const AirTaserHost& h){   // 00266410
    obj.state_8=0;
    air_taser_release(obj,ctx,h);
    obj.wire_10.clear();
    release_light(obj,h);
}

}
