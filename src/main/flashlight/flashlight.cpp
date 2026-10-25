#include "flashlight.hpp"
#include "vu_float.hpp"
#include "weapon_shot_frame.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// Interpreter model (as level_lights.cpp): binary32, round to nearest,
// denormal results flushed to zero; compiled with -ffp-contract=off.
// Results must stay finite (the EE clamps instead; not modelled).
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Flashlight: value not finite");
    return ftz(v);
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float fdiv(float a,float b){
    if(b==0.0F)throw std::runtime_error("Flashlight: division by zero");
    return check(vu::div(a,b));
}
// EE max.s / min.s fd, fs, ft as the interpreter runs them: fs unless ft is
// strictly greater / smaller.
float emax(float s,float t){return t>s?t:s;}
float emin(float s,float t){return t<s?t:s;}
void require(bool ok,const std::string& what){if(!ok)throw std::runtime_error("Flashlight: "+what);}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string("Flashlight: ")+what);}   // 132vs1: no string unless it fails
std::int32_t trunc_word(float v){                       // cvt.w.s
    require(std::isfinite(v)&&std::fabs(v)<2147483648.0F,"value outside the integer range");
    return static_cast<std::int32_t>(v);
}
float word_float(std::int32_t v){                       // cvt.s.w; exact below 2^24
    require(v<=(1<<24)&&v>=-(1<<24),"integer not exactly representable as a float");
    return static_cast<float>(v);
}
// VU sum of squares of xyz: vmul.xyz, then ACC = 1*x2, +1*y2, +1*z2.
float sum_squares(const RootVector& v){return add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));}
// vmulax / vmadday / vmaddaz / vmaddw of a matrix (rows c0..c3) by v.
RootVector transform(const ShotFrame& m,const RootVector& v){
    RootVector r{};
    for(std::size_t i=0;i<4;++i)r[i]=add(add(add(mul(m[0][i],v[0]),mul(m[1][i],v[1])),mul(m[2][i],v[2])),mul(m[3][i],v[3]));
    return r;
}
// vmul.xyz by vrsqrt(vf0.w, |v|^2); w unchanged.
RootVector normalized(const RootVector& v,const char* what){
    const float s=sum_squares(v);
    if(s==0.0F)require(false,std::string("zero-length ")+what+" (the VU reciprocal square root of 0)");
    const float q=check(vu::rsqrt(1.0F,s));
    return {mul(v[0],q),mul(v[1],q),mul(v[2],q),v[3]};
}
// Light vcall +28 = 00144b10: writes the position only when one lane of the
// VU difference is non-zero (flushed differences count as equal), and then
// the manager's +42 (00535d62).
void set_light_position(LevelLights& lights,LevelLight& l,const RootVector& p){
    bool changed=false;
    for(std::size_t i=0;i<4;++i)if(ftz(p[i]-l.position[i])!=0.0F)changed=true;
    if(changed){l.position=p;lights.dirty_42=1;}
}
LevelLight& point_entry(LevelLights& lights,std::size_t index){
    require(index<lights.point.size(),"light index outside the point pool");
    return lights.point[index];
}
// 00373e70(manager, 4): the first point entry with id -1 gets id index + 1.
std::optional<std::size_t> take_point(LevelLights& lights){
    for(std::size_t i=0;i<lights.point.size();++i)
        if(lights.point[i].id==-1){lights.point[i].id=static_cast<std::int32_t>(i+1);return i;}
    return std::nullopt;
}
// Light vcall +8(1): 00144a60 (beam) / 0045b8e0 (glare): id -1, vcall +30
// (00144980: disabled, +42 = 1 when it was enabled), vtable 004d9280, then
// 00373d70 (table removal; for type 4 it also writes id -1).
void release(LevelLights& lights,LevelLight& l){
    if(l.enabled)lights.dirty_42=1;
    l.id=-1;l.enabled=false;l.vtable=level_light_vtable_released;
}
void event(std::vector<FlashlightEvent>& events,FlashlightEvent::Kind kind,std::int32_t value=0,std::uint64_t object=0,const RootVector& p={}){
    events.push_back({kind,value,object,p});
}
}

bool flashlight_created_for_role(std::int32_t role){return role==1||role==3;}

Flashlight make_flashlight(std::uint64_t owner,std::uint64_t spot){
    Flashlight fl;
    fl.owner=owner;fl.spot=spot;                                         // 0045bb88..0045bbc4
    return fl;
}

void set_flashlight_state(Flashlight& fl,LevelLights& lights,bool on,const std::optional<FlashlightOwner>& owner,
    std::vector<FlashlightEvent>& events){
    require(owner.has_value()==(fl.owner!=0),"owner description does not match the flashlight's owner");
    if(on==fl.on)return;                                                 // 0045b630
    if(owner){
        event(events,FlashlightEvent::Kind::click_sound,1,flashlight_click_descriptor,owner->position_e4);   // 0045b644..0045b680
        if(owner->slot7_item!=-1)event(events,FlashlightEvent::Kind::light_byte,on?1:0);                     // 00142e00(inv, 7, on)
    }
    fl.on=on;                                                            // 0045b6c8
    if(on){
        fl.beam=take_point(lights);                                      // 0045b6dc
        if(fl.beam){
            LevelLight& l=lights.point[*fl.beam];
            l.vtable=level_light_vtable_point;                           // 004d9280, 004d9240, then 004d9200
            l.intensity=0.0F;l.enabled=false;l.aux=3;l.type=4;
            l.colour={0.0F,0.0F,0.0F,0.0F};l.position={0.0F,0.0F,0.0F,1.0F};l.weight=0.0F;
        }
        fl.glare=take_point(lights);                                     // 0045b774
        if(fl.glare){
            LevelLight& l=lights.point[*fl.glare];                       // 00179540(l, 0x40, (1,1,1,1), (0,0,0,0), 0x10)
            l.vtable=level_light_vtable_flashlight;
            l.intensity=0.0F;l.enabled=false;l.aux=0x10;l.type=0x40;
            l.colour={1.0F,1.0F,1.0F,1.0F};l.position={0.0F,0.0F,0.0F,0.0F};
            l.weight=fdiv(1.0F,sum_squares(l.colour));                    // 001795dc..00179610
            fl.glare_40=0.0F;                                            // 0045b7e8 (the pool-stride write)
        }
        fl.length_10=-1.0F;                                              // 0045b7f8
        if(fl.beam){
            event(events,FlashlightEvent::Kind::spot_registered,0,fl.spot);
            lights.point[*fl.beam].enabled=true;                         // vcall +2c (00144b80)
            lights.dirty_42=1;
        }
        if(fl.glare){lights.point[*fl.glare].enabled=true;lights.dirty_42=1;}
        return;
    }
    if(fl.beam){                                                         // 0045b858
        event(events,FlashlightEvent::Kind::spot_unregistered,0,fl.spot);
        release(lights,point_entry(lights,*fl.beam));
        fl.beam.reset();
    }
    if(fl.glare){
        release(lights,point_entry(lights,*fl.glare));
        fl.glare.reset();
    }
}

void destroy_flashlight(Flashlight& fl,LevelLights& lights,std::vector<FlashlightEvent>& events){   // 0045b990
    event(events,FlashlightEvent::Kind::spot_unregistered,0,fl.spot);
    if(fl.beam){release(lights,point_entry(lights,*fl.beam));fl.beam.reset();}
    if(fl.glare){release(lights,point_entry(lights,*fl.glare));fl.glare.reset();}
    event(events,FlashlightEvent::Kind::spot_deleted,0,fl.spot);
    fl.spot=0;
}

std::uint64_t flashlight_lit_spot(const Flashlight* fl){return fl&&fl->on?fl->spot:0;}
bool flashlight_is_on(const Flashlight* fl){return fl&&fl->on;}

FlashlightToggleActor flashlight_toggle_actor(const WeaponInventory& inv,const WeaponDatabase& db,const RootVector& position_e4,
    bool controller_player,bool local_14){
    FlashlightToggleActor a;
    a.owner.position_e4=position_e4;
    a.owner.slot7_item=inv.slots[7].item;                                // +78
    a.slot6_item=inv.slots[6].item;                                      // +68
    if(a.slot6_item!=-1)a.slot6_record_100=static_cast<std::int8_t>(db.record(a.slot6_item).byte(0x100));
    a.controller_player=controller_player;a.local_14=local_14;
    return a;
}

void toggle_flashlight(Flashlight& fl,FlashlightGoggles& g,LevelLights& lights,const FlashlightToggleActor& a,
    std::vector<FlashlightEvent>& events){
    if(g.on_33e7){                                                       // 00199788
        event(events,FlashlightEvent::Kind::goggle_mode,g.mode_33e6==0?1:0);
        return;
    }
    const bool ok=a.owner.slot7_item>=0;                                 // 001997b8
    const bool value=ok&&!fl.on;
    event(events,FlashlightEvent::Kind::set_state_call,value?1:0);
    set_flashlight_state(fl,lights,value,a.owner,events);
    if(a.controller_player&&a.local_14&&ok)event(events,FlashlightEvent::Kind::hud_flashlight,value?1:0);   // 00244a80(hud, 0, new)
}

void toggle_goggles(Flashlight& fl,FlashlightGoggles& g,LevelLights& lights,const FlashlightToggleActor& a,
    std::vector<FlashlightEvent>& events){
    const bool was=g.on_33e7;
    if(was){
        if(g.locked_33e5)return;                                         // 00199880
        if(g.mode_33e6==1)event(events,FlashlightEvent::Kind::goggle_mode,0);
        g.on_33e7=false;
    }else if(a.slot6_item!=-1){                                          // 0013fb50
        g.on_33e7=true;
        if(fl.on){
            event(events,FlashlightEvent::Kind::set_state_call,0);
            set_flashlight_state(fl,lights,false,a.owner,events);
        }
    }
    if(g.on_33e7==was)return;
    if(!(a.controller_player&&a.local_14))return;
    std::int32_t value=0;
    if(g.on_33e7&&a.slot6_item!=-1)value=a.slot6_record_100;            // lb record +100
    event(events,FlashlightEvent::Kind::hud_goggles,value);              // 00244b20(hud, value)
}

FlashlightPadRequest update_flashlight_pad(FlashlightPadState& s,float value,float dt,const FlashlightPadGates& g){
    constexpr float threshold=0.5F;                                      // 0048ae88
    const bool pressed=!(value==0.0F);                                   // c.eq.s with 0
    FlashlightPadRequest request=FlashlightPadRequest::none;
    if(!(g.hud_6a9||g.hud_6a8||g.hud_6aa||g.priority_bit7)){
        if(pressed){
            if(!s.previous_a6)s.timer_104=0.0F;                          // 0024c9d4
            else if(s.timer_104<threshold){
                s.timer_104=add(s.timer_104,dt);                         // 0024c9f0
                if(!(s.timer_104<=threshold))request=FlashlightPadRequest::goggles;
            }
        }else if(s.previous_a6&&s.timer_104<threshold)request=FlashlightPadRequest::flashlight;
    }
    s.previous_a6=pressed;                                               // 0024fab0..0024fac8
    return request;
}

const WeaponAttachPoint& flashlight_attach_point(const ModelAttachSlots& slots){
    const auto& p=slots[flashlight_attach_index];
    require(p.has_value(),"attach point 7 is not set (the default frames of 0038e100, node 5, are not ported)");
    return *p;
}

std::function<void(BulletHitRequest&)> flashlight_candidate_scan(const BulletPoolHost& host,std::vector<BulletEvent>& events){
    return [&host,&events](BulletHitRequest& r){scan_bullet_candidates(r,0,host,events);};
}

BulletHitRequest flashlight_scan_request(const FlashlightProbeRequest& p,float step,const WeaponRecord* held){
    BulletHitRequest r;
    r.shooter_0=p.owner;                                                 // 0022aac0
    r.frame_4=p.frame;
    r.designated_8=0;r.rolled_c=false;r.subtarget_10=-1;
    r.step_14=step;
    r.record_18=held;                                                    // 0022aacc
    r.travelled_1c=0.0F;r.skip_damage_20=true;r.penetration_24=0;
    r.volume_28=p.hit_radius;r.statistics_2c=true;r.bullet_type_2d=true;
    return r;
}

float flashlight_beam_probe(const FlashlightProbeRequest& p,const FlashlightProbeHost& host){
    return flashlight_beam_probe_result(p,host).step;
}

FlashlightProbeResult flashlight_beam_probe_result(const FlashlightProbeRequest& p,const FlashlightProbeHost& host){
    require(static_cast<bool>(host.los),"no LOS service for the beam probe (the world LOS service is not native)");
    require(static_cast<bool>(host.scan),"no candidate scan (0022ba30) for the beam probe");
    FlashlightLosRequest q;
    q.origin=p.frame[3];                                                 // request +30
    for(std::size_t i=0;i<4;++i)q.displacement[i]=mul(p.frame[1][i],p.range);   // vmulx.xyzw
    q.owner=p.owner;
    q.mask=p.los_mask;                                                   // 0022a9cc
    float step=p.range;
    if(const auto hit=host.los(q)){                                      // result byte 0, point +10
        const RootVector d{sub((*hit)[0],q.origin[0]),sub((*hit)[1],q.origin[1]),sub((*hit)[2],q.origin[2]),0.0F};
        step=check(vu::sqrt(sum_squares(d)));                            // vsqrt, vaddq.x
    }
    BulletHitRequest r=flashlight_scan_request(p,step,host.held_record);
    host.scan(r);
    if(r.category_30!=0)step=r.distance_34;                              // 0022aae0
    return {step,r.category_30,r.victim_38};
}

FlashlightFrameResult update_flashlight(Flashlight& fl,LevelLights& lights,const FlashlightTuning& t,const FlashlightFrameInput& in){
    FlashlightFrameResult out;
    if(!fl.beam)return out;                                              // 0045aeb8
    require(fl.owner!=0,"update without an owner actor (0045ae70 reads +3524 / +3204 through it)");
    require(std::isfinite(in.dt),"time step not finite");
    for(const auto& row:in.attach)for(float x:row)require(std::isfinite(x),"attach matrix not finite");
    LevelLight& beam=point_entry(lights,*fl.beam);
    const ShotFrame& M=in.attach;
    if(fl.glare){                                                        // 0045aec0
        LevelLight& glare=point_entry(lights,*fl.glare);
        require(in.camera_position.has_value(),"no camera position for the glare (list 004ffc14)");
        const RootVector& cam=*in.camera_position;
        const RootVector a=normalized({sub(M[3][0],cam[0]),sub(M[3][1],cam[1]),sub(M[3][2],cam[2]),sub(M[3][3],cam[3])},"camera-to-attach vector");
        const RootVector b=normalized(M[1],"beam axis");
        const float d=add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));
        float v=0.0F;
        if(d<=0.0F)v=-d;                                                 // c.ole.s, neg.s (not |d|)
        if(v<t.squeeze)v=0.0F;
        else v=fdiv(sub(v,t.squeeze),sub(1.0F,t.squeeze));
        for(int k=0;k<4;++k)v=mul(v,v);                                  // v^16
        fl.glare_40=v;
        set_light_position(lights,glare,M[3]);                           // vcall +28
    }
    const RootVector lens=transform(M,{0.0F,-1.5F,0.0F,1.0F});          // 0045b068 (-1.5)
    ShotFrame Mp=M;Mp[3]=lens;
    FlashlightProbeRequest probe;
    probe.owner=fl.owner;probe.frame=Mp;probe.hit_radius=t.hit_rad;
    probe.range=in.short_range?t.short_dist:t.dist;                      // 0045b148..0045b19c
    require(static_cast<bool>(in.probe),"no beam probe (0022a8c0)");
    out.probe=probe;
    out.hit=check(in.probe(probe));
    const float target=emin(emax(sub(out.hit,t.adjust_y),0.0F),t.dist);  // max.s / min.s
    out.target=target;
    if(fl.length_10<=0.0F){fl.length_10=target;fl.speed_14=0.0F;}        // 0045b21c
    else{
        const float e=sub(target,fl.length_10);
        const float vdt=mul(t.vel,in.dt),adt=mul(t.acc,in.dt);
        const float f2=emin(sub(mul(vdt,mul(e,e)),fl.speed_14),adt);
        fl.speed_14=add(fl.speed_14,f2);
        const float step=emin(emax(e,-fl.speed_14),fl.speed_14);
        fl.length_10=add(fl.length_10,step);
    }
    const RootVector end=transform(Mp,{0.0F,fl.length_10,0.0F,1.0F});
    out.end=end;
    const float ratio=fdiv(target,t.dist);                               // t from the target
    constexpr float max_rg=1.0F,max_b=1.0F;                              // 00497a00 / 00497a08
    const float inner=add(t.min_inner,mul(ratio,sub(t.max_inner,t.min_inner)));
    const float outer=add(t.min_outer,mul(ratio,sub(t.max_outer,t.min_outer)));
    const float rg=add(t.min_rg,mul(sub(1.0F,ratio),sub(max_rg,t.min_rg)));
    const float bl=add(t.min_b,mul(sub(1.0F,ratio),sub(max_b,t.min_b)));
    set_light_position(lights,beam,end);                                 // vcall +28
    fl.spot_position=end;                                                // spot +50 vcall +14
    beam.colour={rg,rg,bl,0.0F};
    beam.weight=fdiv(1.0F,add(add(mul(rg,rg),mul(rg,rg)),mul(bl,bl)));
    beam.inner=inner;beam.outer=outer;
    const float outer2=mul(outer,outer);
    for(std::size_t i=0;i<in.think_list.size();++i){                     // 004ea190
        const FlashlightThinkEntry& o=in.think_list[i];
        require(o.handle_valid,"think-list entry with a stale handle (0045b530 calls through a null object)");
        const RootVector d{sub(o.position_e4[0],end[0]),sub(o.position_e4[1],end[1]),sub(o.position_e4[2],end[2]),0.0F};
        if(sum_squares(d)<outer2&&o.controller_58)out.sight.push_back({i,o.object,fl.owner,6,0.0F});
    }
    return out;
}

std::int32_t actor_stealth_value(const RootVector& c,bool player,bool flashlight_on,std::int32_t dd,std::int32_t ndd){
    const RootVector s{fdiv(c[0],128.0F),fdiv(c[1],128.0F),fdiv(c[2],128.0F),0.0F};
    const float l=check(vu::sqrt(sum_squares(s)));                      // vsqrt, vaddq.x
    std::int32_t v=trunc_word(mul(100.0F,sub(1.0F,l)));
    const std::int32_t thr=player?dd:ndd;
    if(v<thr)v=0;
    else{
        require(thr!=100,"darkness threshold 100 (00133d70 divides by 100 - threshold)");
        v=trunc_word(mul(fdiv(100.0F,word_float(100-thr)),word_float(v-thr)));
    }
    if(flashlight_on)v=0;                                                // 00199740
    return v;
}

}
