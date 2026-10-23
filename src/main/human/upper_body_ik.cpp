#include "upper_body_ik.hpp"
#include "upper_body_math.hpp"
#include "game_libm.hpp"
#include "secondary_aim.hpp"
#include <array>
#include <cmath>
#include <limits>

namespace omega {
namespace {
using namespace upper_body_math;

UpperBodyEntry& entry_at(std::span<UpperBodyEntry> entries,std::size_t i,const char* where){
    if(i>=entries.size())fail(std::string(where)+" entry outside the model");
    return entries[i];
}
const UpperBodyEntry* parent_of(std::span<const UpperBodyEntry> entries,const UpperBodyEntry& e,const char* where){
    if(e.parent<0)return nullptr;
    if(static_cast<std::size_t>(e.parent)>=entries.size())fail(std::string(where)+" parent outside the model");
    return &entries[static_cast<std::size_t>(e.parent)];
}
// The vmul.xyz / vmulax.w / vmadday.w / vmaddz.w dot of xyz with itself.
float length_squared(const V& v){
    const float x=fin(v[0]*v[0]),y=fin(v[1]*v[1]),z=fin(v[2]*v[2]);
    return madd(madd(fin(1.0F*x),1.0F,y),1.0F,z);
}
V difference(const V& a,const V& b){return {fin(a[0]-b[0]),fin(a[1]-b[1]),fin(a[2]-b[2]),fin(a[3]-b[3])};}
// vsqrt / vaddq.x / vdiv Q,vf0.w / vmulq.xyz: xyz scaled by 1 / |xyz|; the
// length is returned.
float normalize_by_length(V& v,const char* where){
    const float length=fin(0.0F+fin(vu::sqrt(length_squared(v))));
    if(length==0.0F)fail(std::string(where)+" divides by a zero length");
    const float q=fin(vu::div(1.0F,length));
    for(std::size_t k=0;k<3;++k)v[k]=fin(v[k]*q);
    return length;
}
// vrsqrt Q,vf0.w / vmulq.xyz.
void normalize_by_rsqrt(V& v,const char* where){
    const float s=length_squared(v);
    if(s==0.0F)fail(std::string(where)+" RSQRT of zero");
    const float q=fin(vu::rsqrt(s));
    for(std::size_t k=0;k<3;++k)v[k]=fin(v[k]*q);
}
// vopmula.xyz ACC,a,b / vopmsub.xyz vd,b,a: a x b.
V cross(const V& a,const V& b){
    return {msub(fin(a[1]*b[2]),b[1],a[2]),msub(fin(a[2]*b[0]),b[2],a[0]),msub(fin(a[0]*b[1]),b[0],a[1]),0.0F};
}
float dot3(const V& a,const V& b){
    const V p{fin(a[0]*b[0]),fin(a[1]*b[1]),fin(a[2]*b[2]),fin(a[3]*b[3])};
    return madd(madd(fin(1.0F*p[0]),1.0F,p[1]),1.0F,p[2]);
}
// 001d19f0..001d1a68 and 001d2448..001d24c0: the rotation taking unit a onto
// unit b. s = sqrt(2 + 2 a.b); (a x b / s, s / 2), or (1, 0, 0, 0) when
// 2 + 2 a.b sets the w zero or sign flag.
V rotation_between(const V& a,const V& b,const char* where){
    const float d=dot3(a,b);
    const float acc=fin(2.0F+0.0F);         // vaddax.w ACC,vf10(w 2),vf0
    const float product=2.0F*d;             // vmaddw.w vf5,vf10,vf4
    const float sum=acc+product;
    if(!std::isfinite(product)||!std::isfinite(sum))fail("nonfinite arithmetic");
    if(sum==0.0F||sign_flag(sum))return {1.0F,0.0F,0.0F,0.0F};
    const float s=fin(1.0F*fin(vu::sqrt(fin(sum)))); // vmulq.w vf7,vf0,Q
    if(s==0.0F)fail(std::string(where)+" divides by a zero half-angle");
    const float q=fin(vu::div(1.0F,s));
    const V c=cross(a,b);
    return {fin(c[0]*q),fin(c[1]*q),fin(c[2]*q),fin(s*0.5F)};
}
// point - origin (xyz, w kept) rotated by q, plus origin (xyz); w = point.w.
V swing_point(const V& q,const V& point,const V& minus_origin,const V& origin){
    V offset=point;
    for(std::size_t k=0;k<3;++k)offset[k]=fin(point[k]+minus_origin[k]);
    V r=rotate_offset(q,offset);
    for(std::size_t k=0;k<3;++k)r[k]=fin(r[k]+origin[k]);
    return r;
}
// The inline 001d1100 test and 001d29f0 clamp between the reference
// conjugates (001d1ee0..001d20c0, 001d25e8..001d27d8).
V clamp_local(const V& world,const V& parent_world,const V& reference,const V& limit){
    V local=multiply(conjugate(parent_world),world);
    local=multiply(conjugate(reference),local);
    V clamped{};
    upper_body_limit_rotation(clamped,local,limit);
    return multiply(reference,clamped);
}
// Python max(s, t) / min(s, t) of the interpreter's max.s / min.s.
float fpu_max(float s,float t){return t>s?t:s;}
float fpu_min(float s,float t){return t<s?t:s;}
}

void upper_body_ik_reset(std::span<UpperBodyEntry> entries,std::size_t i){
    UpperBodyEntry& e=entry_at(entries,i,"001d2880");
    e.target=e.local;
    e.ik_world=e.world;
    e.ik_position=e.position;
}

void upper_body_ik_world(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,std::size_t i){
    UpperBodyEntry& e=entry_at(entries,i,"001d5fa0");
    if(i>=nodes.size())fail("001d5fa0 node outside the model");
    e.ik_world=e.target;
    e.ik_position=nodes[i].offset;
    const UpperBodyEntry* p=parent_of(entries,e,"001d5fa0");
    if(!p)return;
    e.ik_world=multiply(p->ik_world,e.ik_world);
    e.ik_position=place(p->ik_world,p->ik_position,e.ik_position);
}

void upper_body_ik_position(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,std::size_t i){
    UpperBodyEntry& e=entry_at(entries,i,"001d5e90");
    if(i>=nodes.size())fail("001d5e90 node outside the model");
    e.ik_position=nodes[i].offset;
    const UpperBodyEntry* p=parent_of(entries,e,"001d5e90");
    if(!p)return;
    e.ik_position=place(p->ik_world,p->ik_position,e.ik_position);
}

void upper_body_ik_set_rotation(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& q,std::size_t i){
    UpperBodyEntry& e=entry_at(entries,i,"001d28d0");
    if(i>=nodes.size())fail("001d28d0 node outside the model");
    V t=multiply(e.inverse,q);                  // 001a7d80(t, +30)
    const V u=multiply(e.inverse,nodes[i].composed);
    hemisphere(t,u);                            // 001d2ba0
    upper_body_limit_rotation(e.target,t,e.limit); // 001d29f0; v0 unused
    e.target=multiply(e.reference,e.target);    // 001a7d80(+d0, +20)
    upper_body_ik_world(entries,nodes,i);
}

bool upper_body_left_hand_target(std::span<const UpperBodyEntry> entries,const RootVector& component_490,
    const RootVector& component_5c0,std::uint32_t state,RootVector& out){
    const std::uint32_t s=state&0xffU;
    if(s==3||(s>=10&&s<=16)){
        if(entries.size()<=20)fail("001d3e30 entry 20 outside the model");
        out=place(entries[20].ik_world,entries[20].ik_position,component_490); // 001a2b20
        return true;
    }
    if(s==5){
        if(entries.size()<=19)fail("001d3e30 entry 19 outside the model");
        const UpperBodyEntry& e13=entries[13];
        const UpperBodyEntry& e14=entries[14];
        const UpperBodyEntry& e19=entries[19];
        V p=e19.sampled_position;
        const V n=negate(e14.sampled_position);
        for(std::size_t k=0;k<3;++k)p[k]=fin(p[k]+n[k]);
        for(std::size_t k=0;k<3;++k)p[k]=fin(p[k]+e14.position[k]);
        V reach{};
        for(std::size_t k=0;k<4;++k)reach[k]=fin(e19.sampled_position[k]-e13.sampled_position[k]);
        const float length=fin(0.0F+vu::sqrt(length_squared(reach)));
        V d{};
        for(std::size_t k=0;k<4;++k)d[k]=fin(p[k]-e13.position[k]);
        const float radicand=length_squared(d);
        if(radicand==0.0F)fail("001d3f90 RSQRT of zero");
        const float r=vu::rsqrt(radicand);
        for(std::size_t k=0;k<3;++k)d[k]=fin(d[k]*r);
        // 001d3fd0..001d3fdc: |1 - component +25c| <= 1e-7 is compared and
        // never branched on.
        for(std::size_t k=0;k<4;++k)out[k]=fin(e13.position[k]+fin(d[k]*length));
        return true;
    }
    if(s==24){
        out=component_5c0;
        return true;
    }
    return false;
}

bool upper_body_right_hand_target(std::span<const UpperBodyEntry> entries,const RootVector& component_780,
    std::uint32_t state,RootVector& out){
    const std::uint32_t s=state&0xffU;
    if(s==24){
        out=component_780;
        return true;
    }
    const bool reach=s==3||s==4||s==5||s==23;
    const bool offset=s==6||s==7;
    const bool frame=s==10||s==11||s==12||s==16;
    if(!reach&&!offset&&!frame)return false;
    if(entries.size()<=20)fail("001d4eb0 entry 20 outside the model");
    const UpperBodyEntry& e15=entries[15];
    const UpperBodyEntry& e20=entries[20];
    if(offset){
        // 001d5358: entry 15 +a0 plus entry 20 +60 - entry 15 +60, four lanes.
        const V d=difference(e20.sampled_position,e15.sampled_position);
        for(std::size_t k=0;k<4;++k)out[k]=fin(e15.position[k]+d[k]);
        return true;
    }
    // 001d56e0 (xyz += 0 - the frame's +60), 001a90d0 (rotated by the
    // conjugate of its +50), then placed by its +90 / +a0 (the inline
    // sandwich and translation tail).
    auto into_frame=[&](const UpperBodyEntry& k){
        V p=e20.sampled_position;
        const V n=negate(k.sampled_position);
        for(std::size_t c=0;c<3;++c)p[c]=fin(p[c]+n[c]);
        p=rotate_offset(conjugate(k.sampled_world),p);
        return place(k.world,k.position,p);
    };
    if(frame){
        // 001d53d8: states 10 / 11 use entry 14, 12 / 16 entry 0.
        out=into_frame(entries[(s==10||s==11)?14:0]);
        return true;
    }
    const UpperBodyEntry& e14=entries[14];
    V p{};
    if(s==3){
        p=into_frame(e14);                          // 001d50e0
    }else{
        p=e20.sampled_position;                     // 001d4f08: states 4, 5, 23
        const V n=negate(e14.sampled_position);
        for(std::size_t c=0;c<3;++c)p[c]=fin(p[c]+n[c]);
        for(std::size_t c=0;c<3;++c)p[c]=fin(p[c]+e14.position[c]);
    }
    // The shared tail: |entry 20 +60 - entry 15 +60| along the RSQRT-unit
    // direction from entry 15 +a0. |1 - component +25c| <= 1e-7 is compared
    // and never branched on.
    const float length=fin(0.0F+fin(vu::sqrt(length_squared(difference(e20.sampled_position,e15.sampled_position)))));
    V d=difference(p,e15.position);
    normalize_by_rsqrt(d,"001d4eb0");
    for(std::size_t k=0;k<4;++k)out[k]=fin(e15.position[k]+fin(d[k]*length));
    return true;
}

namespace {
// The first-use static quaternions 0049ff60 / 0049ff70 (guards 004f47c0 /
// 004f47c8): Mtx44_SetRotation of the cos / sin (00140e80 / 00140e70) of
// (0, pi/2, pi/2) through the inline matrix-to-quaternion block. 001d2f90's
// 0049ff80 (guard 004f47d0) is the same block on (0, pi/2, -pi/2).
V hand_rest_quaternion(std::uint32_t c_bits){
    auto mul=[](float a,float b){return fin(a*b);};
    auto add=[](float a,float b){return fin(a+b);};
    auto sub=[](float a,float b){return fin(a-b);};
    const float a=0.0F,b=fb(0x3fc90fdbU),c=fb(c_bits);
    const float f24=fin(game_cosf(a)),f23=fin(game_sinf(a));
    const float f22=fin(game_cosf(b)),f21=fin(game_sinf(b));
    const float f20=fin(game_cosf(c)),f0=fin(game_sinf(c));
    const float i=mul(f22,f24);
    const float f2=mul(f0,f24),f1=mul(f0,f23);
    const float f13=mul(-f0,f22),f18=mul(-f22,f23);
    const float f4=mul(f20,f24);
    const float f12=sub(f4,mul(f1,f21));
    const float f17=sub(f1,mul(f4,f21));
    const float f3=mul(f20,f23);
    const float f14=add(f3,mul(f2,f21));
    const float f16=mul(f20,f22);
    const float f15=add(f2,mul(f3,f21));
    const std::array<float,16> m{f12,f15,f18,0.0F, f13,f16,f21,0.0F, f14,f17,i,0.0F, 0.0F,0.0F,0.0F,1.0F};
    return secondary_matrix_quaternion(m);
}
const V& hand_rest(){
    static const V q=hand_rest_quaternion(0x3fc90fdbU);
    return q;
}
const V& left_hand_rest(){
    static const V q=hand_rest_quaternion(0xbfc90fdbU);
    return q;
}
// The 4-lane dot through the accumulator (vmulax.w x, vmadday.w y,
// vmaddaz.w z, vmaddw.w w).
float dot4(const V& a,const V& b){
    const V p{fin(a[0]*b[0]),fin(a[1]*b[1]),fin(a[2]*b[2]),fin(a[3]*b[3])};
    return madd(madd(madd(fin(1.0F*p[0]),1.0F,p[1]),1.0F,p[2]),1.0F,p[3]);
}
V into_hand_parent(std::span<const UpperBodyEntry> entries,const V& q,std::size_t hand=20,const char* where="001d40e0"){
    const UpperBodyEntry* p=parent_of(entries,entries[hand],where);
    if(!p)fail(std::string(where)+" hand entry without a parent");
    return multiply(conjugate(p->ik_world),q);
}
// The aim path shared by 001d40e0 (001d4158..) and 001d2f90 (001d3008..):
// the rest quaternion turned by the normalized 0.35 lerp to +250 when
// |1 - +25c| > 1e-7, by the look-at of the target from the hand's +f0 about
// up (limit 0x3f7fffe7), made w >= 0, then body (x) and conj(parent +e0) (x).
V aimed_hand(std::span<const UpperBodyEntry> entries,const V& rest,const V& component_250,const V& target,
    const V& up,const V& body,std::size_t hand,const char* where){
    V out=rest;
    if(std::fabs(fin(1.0F-component_250[3]))>fb(0x33d6bf95U)){
        // 001d4434 / 001d32b0: lerp (0, 0, 0, 1) -> component +250 by 0.35
        // in four lanes, normalized by the RSQRT of its 4-lane square sum.
        const V a{0.0F,0.0F,0.0F,1.0F};
        const float t=fb(0x3eb33333U);
        V l{};
        for(std::size_t k=0;k<4;++k)l[k]=msub(madd(fin(a[k]+0.0F),component_250[k],t),a[k],t);
        const float sum=dot4(l,l);
        if(sum==0.0F)fail(std::string(where)+" RSQRT of zero");
        const float r=fin(vu::rsqrt(sum));
        for(std::size_t k=0;k<4;++k)l[k]=fin(l[k]*r);
        out=multiply(l,out);
    }
    // 001d4508 / 001d33ec: the look-at frame from the hand's +f0 to the
    // target about up.
    const V direction=difference(target,entries[hand].ik_position);
    const V look=secondary_matrix_quaternion(secondary_look_at(direction,up,fb(0x3f7fffe7U)));
    out=multiply(look,out);
    if(out[3]<0.0F)out=negate(out);
    out=multiply(body,out);
    return into_hand_parent(entries,out,hand,where);
}
}

bool upper_body_right_hand_rotation(std::span<const UpperBodyEntry> entries,const UpperBodyHandInput& in,
    std::uint32_t state,RootVector& out){
    const std::uint32_t s=state&0xffU;
    const bool aim=(s>=3&&s<=7)||s==10||s==11||s==23;
    const bool fixed=s==12||s==16;
    const bool sampled=s>=13&&s<=15;
    if(!aim&&!fixed&&!sampled&&s!=24)return false;
    if(entries.size()<=20)fail("001d40e0 entry 20 outside the model");
    const UpperBodyEntry& e20=entries[20];
    if(sampled){
        out=e20.sampled_local;                      // 001d4d50
        return true;
    }
    if(s==24){                                      // 001d4d70
        out=into_hand_parent(entries,in.component_6b0);
        hemisphere(out,e20.reference);
        return true;
    }
    if(fixed){                                      // 001d4960
        out=multiply(in.aim,hand_rest());
        if(out[3]<0.0F)out=negate(out);
        out=multiply(in.body,out);
        out=into_hand_parent(entries,out);
        return true;
    }
    out=aimed_hand(entries,hand_rest(),in.component_250,in.target,in.up,in.body,20,"001d40e0"); // 001d4158
    return true;
}

bool upper_body_left_hand_rotation(std::span<const UpperBodyEntry> entries,const UpperBodyLeftHandInput& in,
    std::uint32_t state,RootVector& out){
    const std::uint32_t s=state&0xffU;
    const bool fixed=s==3||(s>=10&&s<=16);
    if(s!=5&&!fixed&&s!=24)return false;
    if(entries.size()<=20)fail("001d2f90 entry 20 outside the model");
    if(s==5){                                       // 001d3008
        out=aimed_hand(entries,left_hand_rest(),in.component_250,in.target,in.up,in.body,19,"001d2f90");
        return true;
    }
    if(fixed){                                      // 001d3818
        out=multiply(entries[20].ik_world,in.component_480);
        out=into_hand_parent(entries,out,19,"001d2f90");
        return true;
    }
    out=into_hand_parent(entries,in.component_4f0,19,"001d2f90"); // 001d3920
    hemisphere(out,entries[19].reference);
    return true;
}

void upper_body_arm_solve(const UpperBodyArmInput& in,RootVector& upper,RootVector& forearm){
    V upper_local=in.upper_local;
    if(upper_local[3]<0.0F)upper_local=negate(upper_local);   // sp590
    V forearm_local=in.forearm_local;
    if(forearm_local[3]<0.0F)forearm_local=negate(forearm_local); // sp580
    const V upper_world=multiply(in.parent_world,upper_local); // sp570
    const V forearm_world=multiply(upper_world,forearm_local); // sp560
    const V minus_shoulder=negate(in.upper_position);         // sp550
    V reach=difference(in.target,in.upper_position);          // sp4e0
    const float reach_length=normalize_by_length(reach,"001d190c");
    const float forearm_length=fin(0.0F+fin(vu::sqrt(length_squared(difference(in.hand_position,in.forearm_position)))));
    V hand=difference(in.hand_position,in.upper_position);     // sp4d0
    normalize_by_rsqrt(hand,"001d19c4");
    V swing=rotation_between(hand,reach,"001d1a44");          // sp540
    const V elbow=swing_point(swing,in.forearm_position,minus_shoulder,in.upper_position); // sp500
    V elbow_direction=difference(elbow,in.upper_position);    // sp4c0
    const float upper_length=normalize_by_length(elbow_direction,"001d1b74");
    // 001d1b9c..001d1bec: cos = (a^2 + b^2 - c^2) / (2 a b), clamped to [-1, 1].
    const float c2=fin(forearm_length*forearm_length);
    const float b2=fin(reach_length*reach_length);
    float sum=fin(upper_length*upper_length);
    const float twice=fin(2.0F*upper_length);
    sum=fin(sum+b2);
    const float denominator=fin(twice*reach_length);
    sum=fin(sum-c2);
    if(denominator==0.0F)fail("001d1be4 divides by zero");
    float cosine=fin(sum/denominator);
    cosine=fpu_min(fpu_max(cosine,-1.0F),1.0F);
    V axis=cross(reach,elbow_direction);                       // sp4b0
    V upper_world_out{},rotation{},moved_elbow{},moved_hand{};
    if(axis[0]==0.0F&&axis[1]==0.0F&&axis[2]==0.0F){
        // 001d2340: vaddx.xyz sets all three zero flags.
        upper_world_out=upper_world;
        upper=upper_local;
        moved_elbow=in.forearm_position;
        moved_hand=in.hand_position;
        rotation=swing;
    }else{
        normalize_by_rsqrt(axis,"001d1c84");
        const float sine=fin(vu::sqrt(fin(1.0F-fin(cosine*cosine))));          // f7
        const float current=dot3(reach,elbow_direction);                         // f6
        const float current_sine=fin(vu::sqrt(max_zero(fin(1.0F-fin(current*current)))));
        float half=fin(0.5F*fin(fin(cosine*current)+fin(sine*current_sine)));
        half=fpu_min(fpu_max(half,-0.5F),0.5F);
        const float w=fin(vu::sqrt(fin(0.5F+half)));
        float s=fin(vu::sqrt(fin(0.5F-half)));
        if(!(cosine<=current))s=fin(-s);
        const V bend{fin(axis[0]*s),fin(axis[1]*s),fin(axis[2]*s),w};           // sp490
        swing=multiply(bend,swing);                                              // sp540
        upper=clamp_local(multiply(swing,upper_world),in.parent_world,in.upper_reference,in.upper_limit);
        upper_world_out=multiply(in.parent_world,upper);                         // sp520
        rotation=multiply(upper_world_out,conjugate(upper_world));               // sp540
        moved_elbow=swing_point(rotation,in.forearm_position,minus_shoulder,in.upper_position); // sp500
        moved_hand=swing_point(rotation,in.hand_position,minus_shoulder,in.upper_position);     // sp4f0
    }
    V to_target=difference(in.target,moved_elbow);             // sp480
    normalize_by_rsqrt(to_target,"001d23b8");
    V to_hand=difference(moved_hand,moved_elbow);              // sp470
    normalize_by_rsqrt(to_hand,"001d241c");
    const V turn=rotation_between(to_hand,to_target,"001d249c"); // sp530
    const V world=multiply(turn,multiply(rotation,forearm_world));
    forearm=clamp_local(world,upper_world_out,in.forearm_reference,in.forearm_limit);
}

void upper_body_ik_reach(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& target,std::size_t i,float weight){
    UpperBodyEntry& hand=entry_at(entries,i,"001d2c10");
    const auto up=[&](const UpperBodyEntry& e)->UpperBodyEntry&{
        const UpperBodyEntry* p=parent_of(entries,e,"001d2c10");
        if(!p)fail("001d2c10 chain has fewer than four parents");
        return entries[static_cast<std::size_t>(p-entries.data())];
    };
    UpperBodyEntry& lower=up(hand);
    UpperBodyEntry& upper=up(lower);
    UpperBodyEntry& above=up(upper);
    UpperBodyEntry& top=up(above);
    if(weight<=0.0F){
        upper.target=upper.sampled_local;
        lower.target=lower.sampled_local;
    }else{
        const UpperBodyArmInput in{target,upper.position,upper.local,upper.limit,upper.reference,
            lower.position,lower.local,lower.limit,lower.reference,above.world,hand.position};
        V u{},l{};
        upper_body_arm_solve(in,u,l);
        if(weight<1.0F){
            // 001d2d28..001d2e8c: hemisphere against +40, then +40 + (x - +40) w.
            const auto blend=[weight](const V& from,V x){
                hemisphere(x,from);
                V r{};
                for(std::size_t k=0;k<4;++k)r[k]=msub(madd(fin(from[k]+0.0F),x[k],weight),from[k],weight);
                return r;
            };
            upper.target=blend(upper.sampled_local,u);
            lower.target=blend(lower.sampled_local,l);
        }else{
            upper.target=u;
            lower.target=l;
        }
    }
    top.target=top.local;
    top.ik_world=top.world;
    top.ik_position=top.position;
    above.target=above.local;
    above.ik_world=above.world;
    above.ik_position=above.position;
    const auto index=[&](const UpperBodyEntry& e){return static_cast<std::size_t>(&e-entries.data());};
    upper_body_ik_world(entries,nodes,index(upper));
    upper_body_ik_world(entries,nodes,index(lower));
    upper_body_ik_position(entries,nodes,i);
}

namespace {
// vrsqrt Q,vf0.w / vmulq.xyz of 001d1218 and 001d12e8: the interpreter's
// RSQRT of zero gives the largest float, so a zero direction stays zero.
void normalize_by_rsqrt_or_zero(V& v){
    const float s=length_squared(v);
    const float q=s==0.0F?std::numeric_limits<float>::max():fin(vu::rsqrt(s));
    for(std::size_t k=0;k<3;++k)v[k]=fin(v[k]*q);
}
// vaddx.xyzw vf1,vf1,vf0 / cfc2 vi17 == 0xf: every lane zero (no sign flag).
bool all_zero(const V& v){
    for(const float x:v)if(fin(x+0.0F)!=0.0F)return false;
    return true;
}
}

void upper_body_leg_solve(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::uint8_t component_8,const RootVector& target,std::size_t node,bool flag){
    UpperBodyEntry& tip=entry_at(entries,node,"001d1140");
    const auto up=[&](const UpperBodyEntry& e)->UpperBodyEntry&{
        const UpperBodyEntry* p=parent_of(entries,e,"001d1140");
        if(!p)fail("001d1140 chain has fewer than four parents");
        return entries[static_cast<std::size_t>(p-entries.data())];
    };
    UpperBodyEntry& end=up(tip);     // s2
    UpperBodyEntry& lower=up(end);   // s1
    UpperBodyEntry& upper=up(lower); // s0
    UpperBodyEntry& above=up(upper); // v1
    if(!flag&&component_8!=2){
        // 001d16d8: the three local rotations, no world update.
        upper.target=upper.local;
        lower.target=lower.local;
        end.target=end.local;
        return;
    }
    V lower_local=lower.local; // sp+2a0
    V tip_position=tip.position; // sp+290
    V d1=difference(end.position,lower.position); // sp+280
    normalize_by_rsqrt_or_zero(d1);
    V mid{};
    for(std::size_t k=0;k<4;++k)mid[k]=fin(fin(end.position[k]+upper.position[k])*0.5F);
    V d2=difference(mid,lower.position); // sp+270
    normalize_by_rsqrt_or_zero(d2);
    if(!all_zero(d1)&&!all_zero(d2)){
        // 001d138c..001d1614: turn the lower bone so the end lies on the
        // direction to the midpoint of the end and the upper bone.
        const V arc=rotation_between(d1,d2,"001d13c0");
        lower_local=multiply(conjugate(upper.world),multiply(arc,multiply(upper.world,lower_local)));
        const V turned=rotate_offset(arc,difference(tip_position,lower.position));
        for(std::size_t k=0;k<4;++k)tip_position[k]=fin(turned[k]+lower.position[k]);
    }
    const UpperBodyArmInput in{target,upper.position,upper.local,upper.limit,upper.reference,
        lower.position,lower_local,lower.limit,lower.reference,above.ik_world,tip_position};
    V u{},l{};
    upper_body_arm_solve(in,u,l);
    upper.target=u;
    lower.target=l;
    end.target=end.local;
    const auto index=[&](const UpperBodyEntry& e){return static_cast<std::size_t>(&e-entries.data());};
    upper_body_ik_world(entries,nodes,index(upper));
    upper_body_ik_world(entries,nodes,index(lower));
    upper_body_ik_world(entries,nodes,index(end));
}

void upper_body_foot_rotation(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const RootVector& rotation,std::size_t node,bool flag){
    UpperBodyEntry& e=entry_at(entries,node,"001d0cf0");
    if(!flag){
        if(node>=nodes.size())fail("001d0cf0 node outside the model");
        e.target=nodes[node].pose;
        return;
    }
    const UpperBodyEntry* p=parent_of(entries,e,"001d0cf0");
    if(!p)fail("001d0cf0 entry without a parent");
    // 001d0df0(+d0, conj(parent +e0) (x) rotation, 1, +10).
    V t=multiply(e.inverse,multiply(conjugate(p->ik_world),rotation));
    if(t[3]<0.0F)t=negate(t);
    V clamped{};
    upper_body_limit_rotation(clamped,t,e.limit);
    e.target=multiply(e.reference,clamped);
}

void upper_body_leg_pass(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    std::uint8_t component_8,const RootVector& target,const RootVector& rotation,std::size_t node,bool flag){
    upper_body_leg_solve(entries,nodes,component_8,target,node,flag);
    upper_body_foot_rotation(entries,nodes,rotation,node,flag);
}

namespace {
using Matrix=std::array<float,16>;
// 0013ed60 / 001cf3a0: xyz = M0 v.x + M1 v.y + M2 v.z (vmulax / vmadday /
// vmaddz .xyz), w kept.
V rotate_rows(const Matrix& m,const V& v){
    V r=v;
    for(std::size_t k=0;k<3;++k)r[k]=madd(madd(fin(m[k]*v[0]),m[4+k],v[1]),m[8+k],v[2]);
    return r;
}
// 00171b30: M0 x + M1 y + M2 z + M3 w on all four lanes.
V transform_rows(const Matrix& m,const V& v){
    V r{};
    for(std::size_t k=0;k<4;++k)r[k]=madd(madd(madd(fin(m[k]*v[0]),m[4+k],v[1]),m[8+k],v[2]),m[12+k],v[3]);
    return r;
}
// 001cd110..001cd170: the rotation matrix of q (vf2 = q + q, vf4/5/6 = q
// times vf2.x/y/z, vf3 = (1, 1, 1)); row 3 = vf0.
Matrix quaternion_rows(const V& q){
    const V q2{fin(q[0]+q[0]),fin(q[1]+q[1]),fin(q[2]+q[2]),fin(q[3]+q[3])};
    V x{},y{},z{};
    for(std::size_t k=0;k<4;++k){y[k]=fin(q[k]*q2[1]);z[k]=fin(q[k]*q2[2]);x[k]=fin(q[k]*q2[0]);}
    return {msub(fin(1.0F-y[1]),1.0F,z[2]),fin(x[1]+z[3]),fin(x[2]-y[3]),0.0F,
            fin(y[0]-z[3]),msub(fin(1.0F-x[0]),1.0F,z[2]),fin(y[2]+x[3]),0.0F,
            fin(z[0]+y[3]),fin(z[1]-x[3]),msub(fin(1.0F-x[0]),1.0F,y[1]),0.0F,
            0.0F,0.0F,0.0F,1.0F};
}
// 001cf2d0(out, A, B): row k of A (xyz) turned by B's rows 0..2, w kept.
Matrix turn_rows(const Matrix& a,const Matrix& b){
    Matrix r{};
    for(std::size_t k=0;k<4;++k){
        const V row{a[4*k],a[4*k+1],a[4*k+2],a[4*k+3]};
        const V t=rotate_rows(b,row);
        for(std::size_t j=0;j<4;++j)r[4*k+j]=t[j];
    }
    return r;
}
// 001bc2b0: rows 0..2 of the node's +10 matrix, row 3 (0, 0, 0, 1).
Matrix rotation_part(const Matrix& m){
    Matrix r=m;
    r[12]=0.0F;r[13]=0.0F;r[14]=0.0F;r[15]=1.0F;
    return r;
}
// 001cd074..001cd330 (and its three copies): the node frame's row 2 turned
// onto the ground normal, the frame turned by it (001cf2d0), as a quaternion.
V aligned_rotation(const Matrix& node,const V& normal){
    const Matrix a=rotation_part(node);
    const V q=rotation_between({a[8],a[9],a[10],a[11]},normal,"001cd074");
    return secondary_matrix_quaternion(turn_rows(a,quaternion_rows(q)));
}
// 001ccb70..001ccc48: the contact normal in the component frame when the
// contact's weight is positive, else (0, 0, 1, 0).
V contact_normal(const UpperBodyLegContact& c,const Matrix& inverse){
    if(c.weight<=0.0F)return {0.0F,0.0F,1.0F,0.0F};
    return rotate_rows(inverse,c.normal);
}
// 001ccd68..001cce08: the node's world position, its +90 (plus 0.2 z scaled
// by 0: 0017f670 / 00134080) turned and moved by the parent's +10 matrix.
V child_position(const Matrix& parent,const V& translation){
    V t{};
    for(std::size_t k=0;k<4;++k)t[k]=fin(translation[k]+fin(std::bit_cast<float>(k==2?0x3e4ccccdU:0U)*0.0F));
    const V r=rotate_rows(parent,t);
    return {fin(parent[12]+r[0]),fin(parent[13]+r[1]),fin(parent[14]+r[2]),parent[15]};
}
}

UpperBodyLegTargets upper_body_leg_targets(const UpperBodyLegInput& in,const std::array<float,16>& component,
    const std::array<float,16>& inverse){
    UpperBodyLegTargets out;
    float ground_40=in.contact_40.point[2],ground_70=in.contact_70.point[2];          // f21 / f20
    out.normal_cb0=contact_normal(in.contact_40,inverse);
    out.normal_ca0=contact_normal(in.contact_70,inverse);
    const V foot_16=transform_rows(component,child_position(in.node_11,in.translation_16)); // sp+bd0
    const V foot_12=transform_rows(component,child_position(in.node_7,in.translation_12));  // sp+bc0
    out.flag_c0=!(in.contact_70.weight<=0.0F)&&!(ground_70<foot_16[2]);
    out.flag_s1=!(in.contact_40.weight<=0.0F)&&!(ground_40<foot_12[2]);
    if(!out.flag_c0)ground_70=foot_16[2];
    if(!out.flag_s1)ground_40=foot_12[2];
    out.target_b90=transform_rows(inverse,{foot_16[0],foot_16[1],ground_70,1.0F});
    out.target_b80=transform_rows(inverse,{foot_12[0],foot_12[1],ground_40,1.0F});
    out.rotation_a70=aligned_rotation(in.node_11,out.normal_ca0);
    out.rotation_a60=aligned_rotation(in.node_7,out.normal_cb0);
    out.rotation_a50=aligned_rotation(in.node_16,out.normal_ca0);
    out.rotation_a40=aligned_rotation(in.node_12,out.normal_cb0);
    return out;
}

}
