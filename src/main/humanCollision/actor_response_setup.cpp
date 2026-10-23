#include "float_bits.hpp"
#include "actor_response_setup.hpp"
#include "morph_resources.hpp"
#include "actor_specialized_body_query.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float check(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Response setup: unsupported nonfinite/subnormal arithmetic");
    return value;
}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
RootVector add4(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=add(a[i],b[i]);return r;}
RootVector sub4(const RootVector& a,const RootVector& b){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=sub(a[i],b[i]);return r;}
RootVector scale4(const RootVector& a,float s){RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=mul(a[i],s);return r;}
// vaddax/vmaddax/vmsubx: (a + 0) + b*w - a*w, lane by lane.
RootVector blend4(const RootVector& a,const RootVector& b,float w){
    RootVector r{};for(std::size_t i=0;i<4;++i)r[i]=sub(add(add(a[i],0.0F),mul(b[i],w)),mul(a[i],w));return r;}
}

float actor_response_height_delta(float top,float reference,float radius,float scale,bool flag1,
    const ActorResponseTuning& tuning){
    const float offset=flag1?tuning.height_offset_flag1:tuning.height_offset_unflagged;
    const float minimum=mul(radius,scale);
    float half=mul(sub(sub(add(top,radius),reference),offset),0.5F);
    half=minimum<half?half:minimum;
    return sub(sub(sub(add(top,radius),half),half),reference);
}

ActorResponseSetup prepare_actor_response(ActorResponsePhysical& p,const RootVector& support,
    float elapsed,const ActorResponseTuning& tuning,const ActorResponseSetupHost& host){
    if(!host.floor_root_parented||!host.root_weight||!host.component_scale_z||!host.model_anchor||
        !host.floor_root_world_z||!host.adjust_movement)
        throw std::runtime_error("Response setup: complete live root/component host required");
    check(elapsed);for(float lane:support)check(lane);
    ActorResponseSetup out;
    out.previous_e0=p.e0;
    out.vertical_offset=host.floor_root_parented()?0.0F:p.height_394;
    const float f21=out.vertical_offset;
    constexpr float lowest=-std::numeric_limits<float>::max();
    p.e0=lowest;p.e4=lowest;p.e8=0;
    const RootVector old_offset=p.offset_380;
    p.offset_380=scale4(tuning.offset,host.component_scale_z());
    const RootVector offset=p.use_new_offset_11?p.offset_380:old_offset;
    out.base=p.position_300;
    out.origin=add4(p.position_300,offset);
    if(p.shape_14>=2){
        const bool crouch=(p.flags_34&1U)!=0;
        const float rate=mul(crouch?tuning.crouch_rate:tuning.stand_rate,elapsed);
        const float target=crouch?tuning.crouch_target:tuning.stand_target;
        float step=sub(target,p.scale_390);
        step=step>-rate?step:-rate;   // max.S
        step=step<rate?step:rate;     // min.S
        p.scale_390=add(p.scale_390,step);
    }
    const float floor_weight=host.root_weight(ActorRoot::floor);
    const RootVector anchor=host.model_anchor();
    const RootVector lowered=sub4(anchor,scale4({0,0,f21,0},floor_weight));
    out.anchor=add4(lowered,support);
    out.anchor_top=add4(out.anchor,p.offset_380);
    if(host.root_weight(ActorRoot::floor)>0.0F){
        float penetration;
        if(p.shape_14>=2){
            const float top=add(f21,out.anchor_top[2]);
            const float reference=add(host.floor_root_world_z(),support[2]);
            penetration=actor_response_height_delta(top,reference,p.radius_370,p.scale_390,
                ((p.flags_34>>1)&1U)!=0,tuning);
        }else{
            const float root_z=host.floor_root_world_z();
            penetration=sub(sub(add(f21,out.anchor_top[2]),p.radius_370),add(root_z,support[2]));
        }
        if(penetration<0.0F){
            const float lift=mul(penetration,host.root_weight(ActorRoot::floor));
            out.anchor_top[2]=sub(out.anchor_top[2],lift);
            if(p.use_new_offset_11)out.origin[2]=sub(out.origin[2],lift);
            p.offset_380[2]=sub(p.offset_380[2],lift);
        }
    }
    out.saved_contact_vectors={p.contact_40,p.contact_50};out.saved_contact_60=p.contact_60;
    out.saved_contact_vectors_b={p.contact_70,p.contact_80};out.saved_contact_90=p.contact_90;
    out.previous_dc=p.byte_dc;
    if(!p.keep_contact_9c0){
        p.contact_40={};p.contact_50={};p.contact_60=0;p.contact_70={};p.contact_80={};p.contact_90=0;
    }
    p.floor_2c4=std::numeric_limits<float>::max();p.blend_2c8=0;p.blend_2cc=0;
    if(!p.enabled_12||p.mode_20==12)return out; // tail at 1e4dd8
    out.proceed=true;
    const float ladder=host.root_weight(ActorRoot::ladder);
    const float special=add(host.root_weight(ActorRoot::edge),ladder);
    out.specialized=special>0.0F;
    out.blended=out.specialized&&host.root_weight(ActorRoot::floor)>0.0F;
    out.supported=p.byte_dd&&!p.keep_contact_9c0;
    out.root_displacement=sub4(out.anchor_top,out.origin);
    if(out.specialized&&!out.blended){
        out.intended=out.root_displacement;
    }else if(out.supported){
        out.intended=out.root_displacement;
        const RootVector physical=sub4(p.predicted_2f0,p.position_300);
        const float d=physical[2],e=sub(d,support[2]);
        bool lower=false;
        if(e<=0.0F&&d<out.intended[2])lower=true;
        else if(!(e<0.0F)&&!(d<=out.intended[2])&&!(f21<=0.0F))lower=true;
        if(lower)out.intended[2]=d;
        if(out.blended)out.intended=blend4(out.root_displacement,out.intended,host.root_weight(ActorRoot::floor));
        out.blend_delta=sub4(out.intended,out.root_displacement);
    }else if(out.blended){
        const float w=host.root_weight(ActorRoot::floor);
        out.intended=blend4(out.root_displacement,sub4(p.predicted_2f0,p.position_300),w);
    }else{
        out.intended=sub4(p.predicted_2f0,p.position_300);
    }
    const auto adjusted=host.adjust_movement(add4(out.origin,out.intended));
    out.adjustment=adjusted.adjustment;out.adjusted=adjusted.adjusted;
    out.displacement=add4(out.intended,scale4(out.adjustment,host.root_weight(ActorRoot::floor)));
    return out;
}

namespace {
RootVector transform_point(const RootParentTransform& placement,const RootVector& point){
    const auto matrix=root_placement_matrix(placement); // 0019d2c0, then ordered 00182f30
    RootVector result{};
    for(unsigned row=0;row<4;++row){
        result[row]=mul(matrix[row],point[0]);
        for(unsigned column=1;column<4;++column)result[row]=add(result[row],mul(matrix[column*4+row],point[column]));
    }
    return result;
}
}

namespace {
RootVector rotate_by_quaternion(const RootVector& v,const RootVector& q);
// 0017ccb0 / 001e5710 share the full placement inverse.
RootParentTransform invert_placement(const RootParentTransform& placement){return invert_specialized_body_placement(placement);}
// Quaternion -> matrix first column at 1e34b8 (vf8): image of local X.
RootVector quaternion_x_axis(const RootVector& q){
    const float y2=add(q[1],q[1]),z2=add(q[2],q[2]); // the x column needs no doubled x
    return {sub(sub(1.0F,mul(q[1],y2)),mul(q[2],z2)),add(mul(q[0],y2),mul(q[3],z2)),sub(mul(q[0],z2),mul(q[3],y2)),0.0F};
}
float vu_min(float a,float b){return b<a?b:a;} // min.S
RootVector world_from_placement(const RootVector& local,const RootParentTransform& placement){ // 001a2b20
    RootVector v=rotate_by_quaternion(local,placement.orientation);
    if(v[3]==1.0F){for(std::size_t i=0;i<3;++i)v[i]=add(v[i],placement.position[i]);}
    else{const RootVector scaled=scale4(placement.position,v[3]);for(std::size_t i=0;i<3;++i)v[i]=add(v[i],scaled[i]);v[3]=scaled[3];}
    return v;
}
struct LedgeResult {RootVector target,resolved,reference,delta;};
LedgeResult resolve_ledge(const ActorResponsePhysical& p,const ActorResponseSetup& s,const RootVector& support,
    const RootVector& correction,bool query_hit,const ActorResponseResolveHost& host){
    if(!host.root_weight||!host.node_position||!host.edge_root_resolved||!host.edge_root_world||
        !host.edge_root_parented||!host.edge_root_base_position||!host.set_edge_root_base_position)
        throw std::runtime_error("Response position: complete edge/ladder host required");
    LedgeResult r;
    r.delta=add4(correction,support);                                   // 1070
    r.resolved=add4(add4(s.anchor,s.blend_delta),correction);          // 1090
    const RootVector foot12=add4(host.node_position(12),r.delta);       // 1050
    const RootVector foot16=add4(host.node_position(16),r.delta);       // 1040
    const float lowest=vu_min(foot12[2],foot16[2]);
    const RootVector pelvis=add4(host.node_position(0),r.delta);        // 1030
    const float floor_height=sub(lowest,s.previous_e0)<1.25F?s.previous_e0:lowest;
    r.target=pelvis;r.target[2]=floor_height;                           // 10a0
    r.reference=r.target;                                               // 1080
    if(host.root_weight(ActorRoot::edge)>0.0F){
        const RootParentTransform inverse=invert_placement(host.edge_root_resolved());
        const RootVector l12=transform_point(inverse,foot12);
        const RootVector l16=transform_point(inverse,foot16);
        RootVector lt=transform_point(inverse,r.target);
        const float wall=vu_min(l12[1],vu_min(l16[1],lt[1]));
        lt[1]=wall;                                                     // 001e5700
        const RootVector world=world_from_placement(lt,host.edge_root_world());
        r.reference=blend4(r.reference,world,host.root_weight(ActorRoot::edge));
    }
    r.reference[2]=add4(add4(s.anchor_top,s.blend_delta),correction)[2];
    if(query_hit&&!host.edge_root_parented()){
        RootVector move=correction;
        switch(p.mode_20){
        case 10:break;
        case 11:case 9:move={};break;
        case 1:case 5:{
            const RootVector axis=quaternion_x_axis(host.edge_root_world().orientation);
            move=scale4(axis,add(add(mul(correction[0],axis[0]),mul(correction[1],axis[1])),mul(correction[2],axis[2])));
            break;}
        default:break;
        }
        host.set_edge_root_base_position(add4(host.edge_root_base_position(),move));
    }
    if(p.mode_20==0||p.mode_20==10||p.mode_20==11)
        host.set_edge_root_base_position(add4(host.edge_root_base_position(),support));
    return r;
}
}

ActorResponsePosition resolve_actor_response_position(ActorResponsePhysical& p,const ActorResponseSetup& s,
    const RootVector& support,const RootVector& resolved_displacement,bool query_hit,const ActorResponseResolveHost& host){
    if(!host.floor_root_pose||!host.floor_root_base_position||!host.anchor_local||!host.floor_root_motion||
        !host.component_translation||!host.set_component_position)
        throw std::runtime_error("Response position: complete live root/component host required");
    if(!s.proceed)throw std::runtime_error("Response position: setup took the tail branch");
    for(float lane:resolved_displacement)check(lane);
    ActorResponsePosition out;
    const RootVector vertical{0,0,s.vertical_offset,0};
    const RootVector correction=sub4(resolved_displacement,s.intended);             // 1060 / fa0
    std::optional<LedgeResult> ledge;
    if(s.specialized)ledge=resolve_ledge(p,s,support,correction,query_hit,host);
    if(s.specialized&&!s.blended){
        out.root_target=ledge->target;out.resolved=ledge->resolved;
        out.probe_reference=ledge->reference;out.root_delta=ledge->delta;
    }else{
        if(s.supported){
            const RootVector support_base=add4(host.floor_root_base_position(),support);    // 940
            out.root_target=add4(add4(sub4(support_base,vertical),s.blend_delta),correction);
            out.resolved=add4(add4(s.anchor,s.blend_delta),correction);
            out.probe_reference=out.root_target;
            out.probe_reference[2]=add4(add4(s.anchor_top,s.blend_delta),correction)[2];
        }else{
            out.resolved=add4(s.base,resolved_displacement);
            const RootParentTransform pose=host.floor_root_pose();
            const RootVector anchor=transform_point(pose,host.anchor_local());
            const RootVector offset=sub4(sub4(out.resolved,anchor),vertical);
            out.root_target=add4(host.floor_root_base_position(),offset);
            out.probe_reference=out.root_target;
            out.probe_reference[2]=add4(s.origin,resolved_displacement)[2];
        }
        out.root_delta=sub4(out.root_target,host.floor_root_base_position());
        if(s.blended){                                                     // 1e39a8..3b2c
            if(!host.root_weight)throw std::runtime_error("Response position: blend requires root weights");
            out.root_target=blend4(ledge->target,out.root_target,host.root_weight(ActorRoot::floor));out.root_target[3]=1.0F;
            out.resolved=blend4(ledge->resolved,out.resolved,host.root_weight(ActorRoot::floor));out.resolved[3]=1.0F;
            out.probe_reference=blend4(ledge->reference,out.probe_reference,host.root_weight(ActorRoot::floor));out.probe_reference[3]=1.0F;
            out.root_delta=blend4(ledge->delta,out.root_delta,host.root_weight(ActorRoot::floor));
        }
    }
    // 1e3b30..3c5c: blocked fraction of platform-relative motion.
    if((query_hit||s.adjusted)&&s.supported){
        const RootVector motion=add4(host.floor_root_motion(),support);
        if(!(motion[0]==0&&motion[1]==0&&motion[2]==0&&motion[3]==0)){
            const float length=std::sqrt(add(add(mul(motion[0],motion[0]),mul(motion[1],motion[1])),mul(motion[2],motion[2])));
            if(check(length)!=0.0F){
                // 001d10c0: VU SQRT length, VU DIV 1/length, XYZ scaled by Q.
                const float reciprocal=check(1.0F/length);
                RootVector unit{};for(std::size_t i=0;i<3;++i)unit[i]=mul(motion[i],reciprocal);
                const float along=add(add(mul(unit[0],resolved_displacement[0]),mul(unit[1],resolved_displacement[1])),
                    mul(unit[2],resolved_displacement[2]));
                float blocked=sub(1.0F,check(along/length));
                blocked=blocked>0.0F?blocked:0.0F;
                p.blend_2c8=blocked<1.0F?blocked:1.0F;
                p.blend_2cc=query_hit?1:0;
            }
        }
    }
    out.published=add4(host.component_translation(),out.root_delta);
    host.set_component_position(out.published);
    return out;
}

namespace {
// 001a7370: q * (v, 0) * conj(q), VU accumulation order, W preserved.
RootVector rotate_by_quaternion(const RootVector& v,const RootVector& q){
    const float ax=sub(add(mul(q[1],v[2]),mul(v[0],q[3])),mul(v[1],q[2]));
    const float ay=sub(add(mul(q[2],v[0]),mul(v[1],q[3])),mul(v[2],q[0]));
    const float az=sub(add(mul(q[0],v[1]),mul(v[2],q[3])),mul(v[0],q[1]));
    const float aw=sub(sub(sub(0.0F,mul(q[2],v[2])),mul(q[1],v[1])),mul(q[0],v[0]));
    const float cx=-q[0],cy=-q[1],cz=-q[2],cw=q[3];
    RootVector r{};
    r[0]=sub(add(add(mul(ay,cz),mul(ax,cw)),mul(cx,aw)),mul(az,cy));
    r[1]=sub(add(add(mul(az,cx),mul(ay,cw)),mul(cy,aw)),mul(ax,cz));
    r[2]=sub(add(add(mul(ax,cy),mul(az,cw)),mul(cz,aw)),mul(ay,cx));
    r[3]=v[3];
    return r;
}
RootVector conjugate(const RootVector& q){return {-q[0],-q[1],-q[2],q[3]};} // 001cf5b0
float dot3v(const RootVector& a,const RootVector& b){                  // 00192fb0
    return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}
float dot4v(const RootVector& a,const RootVector& b){                  // 001e53a0
    return add(add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2])),mul(a[3],b[3]));}
// Plane through point with normal, evaluated as height at (x, y); 0 when nz == 0.
float plane_height(const RootVector& normal,const RootVector& point,float x,float y){
    const RootVector plane{normal[0],normal[1],normal[2],-dot3v(normal,point)};
    if(plane[2]==0.0F)return 0.0F;
    return check(-dot4v(plane,{x,y,0,1})/plane[2]);
}
float hermite(const ActorHeightFilter& f){                              // 0017c7c0
    const float u=f.duration==0.0F?1.0F:mul(sub(f.duration,f.timer),f.inverse_duration);
    const float u2=mul(u,u),u3=mul(u2,u);
    const float three_u2=mul(3.0F,u2);
    const float h00=add(1.0F,sub(mul(2.0F,u3),three_u2));
    const float h01=add(mul(-2.0F,u3),three_u2);
    const float h10=add(u,sub(u3,mul(2.0F,u2)));
    const float h11=sub(u3,u2);
    const float base=add(mul(h00,f.p0),mul(h01,f.p1));
    return add(add(mul(h10,f.m0),base),mul(h11,f.m1));
}
}

ActorFloorFilterResult update_actor_floor_filter(ActorResponsePhysical& p,const ActorResponseSetup& s,
    const ActorResponsePosition& position,const RootVector& support,const std::optional<ActorFloorSample>& floor,
    bool current_dc,float elapsed,float inverse_elapsed,const std::function<RootVector()>& orientation){
    if(!orientation)throw std::runtime_error("Floor filter: floor-root orientation host required");
    check(elapsed);check(inverse_elapsed);
    ActorFloorFilterResult out;
    const RootVector& target=position.root_target;
    RootVector point{},normal{};
    float retime=-1.0F;
    if(!floor){
        const RootVector below{0,0,std::bit_cast<float>(0xbe4ccccdU),1}; // (0,0,-0.2,1)
        RootVector world=rotate_by_quaternion(below,orientation());          // 001a2b20
        if(world[3]==1.0F){for(std::size_t i=0;i<3;++i)world[i]=add(world[i],target[i]);}
        else{const RootVector scaled=scale4(target,world[3]);for(std::size_t i=0;i<3;++i)world[i]=add(world[i],scaled[i]);world[3]=scaled[3];}
        point=world;out.goal_local=below;
        retime=std::bit_cast<float>(0x3e4ccccdU); // 0.2
        normal={0,0,1,0}; // original W lane is stale stack data
    }else{
        point=floor->point;normal=floor->face_normal;
        const auto& old_point=s.saved_contact_vectors[0];
        const auto& old_normal=s.saved_contact_vectors[1];
        const RootVector previous=add4({old_point[0],old_point[1],p.floor_height_d8,old_point[3]},support);
        if(!(old_normal[0]==0&&old_normal[1]==0&&old_normal[2]==0&&old_normal[3]==0)){
            const float drop_old=sub(previous[2],plane_height(normal,point,previous[0],previous[1]));
            const float drop_new=sub(point[2],plane_height(old_normal,previous,point[0],point[1]));
            const bool walkable_new=!(std::fabs(normal[2])<=0.5F);
            const bool walkable_old=!(std::fabs(old_normal[2])<=0.5F);
            const bool falling=point[2]<previous[2];
            const bool rising=!(point[2]<=previous[2]);
            const bool deep=add(std::bit_cast<float>(0x3e4ccccdU),sub(point[2],target[2]))<0.0F;
            const float step=std::bit_cast<float>(0x3e19999aU); // 0.15
            const bool decided=[&]{
                if(p.byte_3d4&&rising)return true;
                if(s.previous_dc&&!current_dc&&p.velocity_310[2]<-10.0F){
                    const float scale=check(1.0F/sub(-10.0F,-20.0F));
                    float t=mul(scale,sub(p.velocity_310[2],-20.0F));
                    t=t>0.0F?t:0.0F;t=t<1.0F?t:1.0F;
                    retime=add(0.0F,mul(t,sub(std::bit_cast<float>(0x3e4ccccdU),0.0F)));
                    return true;
                }
                if(drop_old<step&&drop_new<step&&walkable_new&&walkable_old&&!current_dc)return true;
                if(drop_old<-0.25F&&rising){retime=std::bit_cast<float>(0x3e99999aU);return true;}
                if(!walkable_old&&walkable_new){retime=std::bit_cast<float>(0x3e99999aU);return true;}
                if(drop_new<-0.25F&&falling){retime=0.5F;return true;}
                if(!walkable_new&&walkable_old){retime=0.5F;return true;}
                if(current_dc&&!s.previous_dc&&deep&&rising)retime=0.5F;
                return true;
            }();
            (void)decided;
        }
        // 001e52e0: conj(q) * (point - target), W preserved.
        RootVector relative=point;for(std::size_t i=0;i<3;++i)relative[i]=sub(point[i],target[i]);
        out.goal_local=rotate_by_quaternion(relative,conjugate(orientation()));
    }
    out.retime=retime;
    // 1e49c8..4a7c: filter update.
    auto& f=p.height_filter;
    f.damping=15.0F;
    if(!(retime<0.0F)){
        const float factor=mul(retime,f.inverse_duration);                 // 001e5280
        f.m0=mul(f.m0,factor);f.m1=mul(f.m1,factor);f.duration=retime;
        f.inverse_duration=retime!=0.0F?check(1.0F/retime):0.0F;
        f.timer=f.duration;                                                 // 001e5270
    }
    f.goal=out.goal_local[2];
    f.previous=f.value;
    if(f.timer==f.duration&&f.duration>0.0F){                               // 0017c900
        const float T=f.duration;const float end_slope=0.0F;               // vtable +14 -> 0
        f.p0=f.value;f.m0=mul(f.velocity,T);f.p1=f.goal;f.m1=mul(end_slope,T);
        f.inverse_duration=T!=0.0F?check(1.0F/T):0.0F;
    }else if(f.timer!=0.0F){
        f.p1=f.goal;f.m1=mul(0.0F,f.duration);
    }
    {                                                                       // 0017c8a0
        const float t=sub(f.timer,elapsed);f.timer=t>0.0F?t:0.0F;
    }
    if(f.timer!=0.0F)f.value=hermite(f);else f.value=f.goal;                // vtable +1c is empty
    {                                                                       // 0017c760
        f.velocity=mul(inverse_elapsed,sub(f.value,f.previous));
        float keep=sub(1.0F,mul(f.damping,elapsed));keep=keep>0.0F?keep:0.0F;
        f.velocity=mul(f.velocity,keep);
    }
    p.floor_height_d8=point[2];
    RootVector filtered=rotate_by_quaternion({0,0,f.value,1},orientation()); // 001a7370
    if(filtered[3]==1.0F)filtered=add4(filtered,target);                    // 001bc490 (all lanes)
    else{const RootVector scaled=scale4(target,filtered[3]);filtered=add4(filtered,scaled);filtered[3]=scaled[3];}
    point[2]=filtered[2];
    const float offset=add(std::bit_cast<float>(0x3e4ccccdU),sub(filtered[2],target[2]));
    p.contact_40=point;p.contact_50=normal;p.contact_60=offset;
    p.contact_70=p.contact_40;p.contact_80=p.contact_50;p.contact_90=p.contact_60;
    p.predicted_2f0=position.resolved;
    out.floor_point=point;
    return out;
}

namespace {
void reset_integrator(ActorResponsePhysical& p){
    p.force_330={};p.acceleration_320={};        // 001e50a0, 001e5090
    p.position_300=p.predicted_2f0;p.velocity_310={}; // 001e5070
}
bool xyz_zero(const RootVector& v){return v[0]==0&&v[1]==0&&v[2]==0;} // VU MAC zero flags 0xe
}

void skip_actor_response(ActorResponsePhysical& p,const RootVector& anchor_world){
    for(float lane:anchor_world)check(lane);
    p.byte_dc=0;p.byte_dd=1;p.floor_2c4=0.0F;
    p.predicted_2f0=anchor_world;
    reset_integrator(p);
    p.height_394=0.0F;
}

void finish_actor_response(ActorResponsePhysical& p,const ActorResponseSetup& s,float support_scaled_z){
    check(support_scaled_z);
    const bool left_ground=s.previous_dc&&!p.byte_dc;
    bool airborne=left_ground;
    if(!airborne&&p.airborne_de)airborne=sub(p.velocity_310[2],support_scaled_z)<0.0F;
    p.airborne_de=airborne?1:0;
    RootVector axis=add4(p.contact_50,p.contact_80);
    if(!xyz_zero(axis)){
        const float length2=add(add(mul(axis[0],axis[0]),mul(axis[1],axis[1])),mul(axis[2],axis[2]));
        const float q=check(1.0F/std::sqrt(length2)); // VU RSQRT 001e4f34: float sqrt, float 1/x
        for(std::size_t i=0;i<3;++i)axis[i]=mul(axis[i],q);
        p.up_axis_9f0=axis;
    }else if(!xyz_zero(p.contact_motion_1e0)){
        p.up_axis_9f0=p.contact_motion_1e0;
    }else{
        p.up_axis_9f0={0,0,1,0};
    }
    if(!p.byte_13)reset_integrator(p);
}
}
