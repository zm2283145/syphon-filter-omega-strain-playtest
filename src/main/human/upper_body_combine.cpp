#include "upper_body_combine.hpp"
#include "upper_body_math.hpp"
#include "secondary_aim.hpp"

namespace omega {
using namespace upper_body_math;

namespace {
float fpu_max(float s,float t){return t>s?t:s;}
float fpu_min(float s,float t){return t<s?t:s;}
// vaddax / vmaddax / vmsubx.xyzw: the unnormalized lerp of the glue.
V lerp4(const V& a,const V& b,float t){
    V r{};
    for(std::size_t k=0;k<4;++k)r[k]=msub(madd(fin(a[k]+0.0F),b[k],t),a[k],t);
    return r;
}
// 001ce080..001ce218 / 001ce3a0..001ce538: the reach target of the two states
// and the weight +308.
template<class Target>
bool arm_target(const UpperBodyArmsInput& in,Target target,RootVector& out,float& weight){
    V a{},b{};
    const bool has_a=target(in.state_a,a);
    const bool has_b=in.state_a!=in.state_b?target(in.state_b,b):false;
    if(has_a&&has_b){out=lerp3(a,b,in.blend_31c);weight=1.0F;}
    else if(has_a){out=a;weight=fin(1.0F-in.blend_31c);}
    else if(has_b){out=b;weight=in.blend_31c;}
    else{out=V{};weight=0.0F;}
    return has_a||has_b;
}
// 001ce22c..001ce390 / 001ce54c..001ce6a8: the rotation of the two states into
// the slot handed to 001d28d0 (left alone when neither yields one).
template<class Rotation>
bool arm_rotation(const UpperBodyArmsInput& in,Rotation rotation,RootVector& slot){
    V a{},b{};
    const bool has_a=rotation(in.state_a,a);
    const bool has_b=in.state_a!=in.state_b?rotation(in.state_b,b):false;
    if(has_a&&has_b){hemisphere(a,b);slot=lerp4(a,b,in.blend_31c);}
    else if(has_a)slot=a;
    else if(has_b)slot=b;
    return has_a||has_b;
}
}

float upper_body_smoothstep(float x){
    const float x2=fin(x*x),x3=fin(x*x2);
    const float s=fin(fin(3.0F*x2)-fin(2.0F*x3));
    return fpu_min(fpu_max(s,0.0F),1.0F); // max.s f3, f1, f0 / min.s f3, f3, f2
}

float upper_body_look_ramp(float weight,bool active,float dt){
    const float step=fpu_min(1.0F,fin(3.0F*dt)); // 001dd6f4 min.s f20, f1, f0
    if(active)return fpu_min(1.0F,fin(weight+step)); // 001de270
    return fpu_max(0.0F,fin(weight-step));           // 001de29c
}

namespace {
// vmul.xyzw / vmulax.w .. vmaddw.w (x + y + z + w), then c.olt.s with 0.
bool dot_negative(const V& a,const V& b){
    float s=fin(1.0F*fin(a[0]*b[0]));
    s=madd(s,1.0F,fin(a[1]*b[1]));
    s=madd(s,1.0F,fin(a[2]*b[2]));
    s=madd(s,1.0F,fin(a[3]*b[3]));
    return s<0.0F;
}
}

UpperBodyLookRecord upper_body_head_look_record(){
    UpperBodyLookRecord r;
    r.nodes={0xe,9,5,0xe};r.enabled=true;   // 001d84cc..001d84dc
    return r;
}

RootVector upper_body_look_target(const RootVector& point,const std::array<float,16>& up_matrix,
    const std::array<float,16>& component,const RootVector& origin){
    const auto& m=up_matrix;
    // 001dd254..001dd2c0: (0, 0, 1) times the transposed rows (vmulax / vmadday / vmaddz.xyz).
    V up{};
    for(std::size_t k=0;k<3;++k)up[k]=madd(madd(fin(m[4*k]*0.0F),m[4*k+1],0.0F),m[4*k+2],1.0F);
    // 001dd324..001dd348: the origin through the component (xyzw).
    V p{};
    for(std::size_t k=0;k<4;++k)
        p[k]=madd(madd(madd(fin(component[k]*origin[0]),component[4+k],origin[1]),component[8+k],origin[2]),component[12+k],origin[3]);
    V d{};
    for(std::size_t k=0;k<4;++k)d[k]=fin(point[k]-p[k]);       // 001dd36c
    return secondary_matrix_quaternion(secondary_look_at(d,up,fb(0x3f7fffe7U)));  // 001dd384..001dd658
}

void upper_body_look(UpperBodyLookRecord& r,std::span<const UpperBodyEntry> entries,
    const std::array<float,16>& component,const RootVector& origin,float dt){
    if(!r.enabled)return;                                         // 001dd6d4
    if(r.active){
        const auto entry=[&](std::int8_t i)->const UpperBodyEntry&{
            if(i<0||static_cast<std::size_t>(i)>=entries.size())fail("001dd6a0 entry out of range");
            return entries[static_cast<std::size_t>(i)];
        };
        const float t=fpu_min(1.0F,fin(3.0F*dt));                 // 001dda68 / 001ddf40
        V base=entry(r.nodes[1]).reference;                         // 001dd704..001dd720
        if(r.nodes[2]!=-1)base=multiply(entry(r.nodes[2]).world,base); // 001dd740..001dd7a8
        base=multiply(secondary_matrix_quaternion(component),base);    // 001dd7b0..001dd9c0
        const V inverse=conjugate(base);                            // 001dd9cc
        const V look=upper_body_look_target(r.point,component,component,origin); // 001dd9ec
        V q=multiply(inverse,look);                                 // 001dda08
        if(r.weight!=0.0F){                                         // 001dda44
            V prev=multiply(inverse,r.world_20);                    // 001dda80
            if(q[3]<0.0F)q=negate(q);                               // 001ddabc
            if(dot_negative(prev,r.local_30))prev=negate(prev);     // 001ddaf4..001ddb3c
            r.local_30=upper_body_lerp(prev,q,t);                   // 001ddb40..001ddbb0
        }else r.local_30=q;                                         // 001ddbb8
        (void)upper_body_limit_rotation(r.local_30,r.local_30,entry(r.nodes[1]).limit); // 001ddbc8..001ddd40
        r.world_20=multiply(base,r.local_30);                       // 001ddd48..001ddda0
        if(r.nodes[3]!=-1){                                         // 001dddb0
            const V second=multiply(r.world_20,entry(r.nodes[3]).reference); // 001dddbc..001dde40
            const V inverse2=conjugate(second);                     // 001dde50
            V q2=multiply(inverse2,look);                           // 001dde78
            if(dot_negative(q2,r.local_30))q2=negate(q2);           // 001ddeb0: against +30
            if(r.weight!=0.0F){                                     // 001ddf1c
                V prev=multiply(inverse2,r.world_40);               // 001ddf58
                if(dot_negative(prev,r.local_50))prev=negate(prev); // 001ddfa0..001ddfec
                r.local_50=upper_body_lerp(prev,q2,t);              // 001ddff0..001de060
            }else r.local_50=q2;                                    // 001de068
            (void)upper_body_limit_rotation(r.local_50,r.local_50,entry(r.nodes[3]).limit); // 001de078..001de1f0
            r.world_40=multiply(second,r.local_50);                 // 001de1f8..001de24c
        }
    }
    r.weight=upper_body_look_ramp(r.weight,r.active,dt);           // 001de250 / 001de280
}

void upper_body_head(std::span<UpperBodyEntry> entries,const RootVector& component_250,
    const RootVector& c440,const RootVector& c460,std::size_t i,float weight){
    if(i>=entries.size())fail("001d0780 entry out of range");
    UpperBodyEntry& e=entries[i];
    if(e.parent<0||static_cast<std::size_t>(e.parent)>=entries.size())fail("001d0780 entry has no parent");
    UpperBodyEntry& p=entries[static_cast<std::size_t>(e.parent)];
    const bool turn=!(std::fabs(fin(1.0F-component_250[3]))<=fb(0x33d6bf95U));
    V l{};
    if(turn)l=upper_body_lerp(V{0,0,0,1},conjugate(component_250),0.5F); // 001cf5b0, 00190750 inline
    if(weight!=0.0F){
        const float t=fin(fb(0x3f333333U)*weight);
        p.target=upper_body_lerp(p.local,multiply(p.reference,c440),t);
        if(turn)p.target=multiply(p.target,l);
        e.target=upper_body_lerp(e.local,multiply(e.reference,c460),t);
        if(turn)e.target=multiply(e.target,l);
    }else if(turn){
        p.target=multiply(p.local,l);
        e.target=multiply(e.local,l);
    }else{
        p.target=p.local;p.ik_world=p.world;p.ik_position=p.position;
        e.target=e.local;e.ik_world=e.world;e.ik_position=e.position;
    }
}

UpperBodyArmsResult upper_body_arms(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    const UpperBodyArmsInput& in,UpperBodyArmsState& state){
    const auto& rows=upper_body_state_rows();
    if(in.state_a>=rows.size()||in.state_b>=rows.size())fail("001ce080 state out of range");
    UpperBodyArmsResult r{};
    r.right=rows[in.state_a].flags[3]!=0||rows[in.state_b].flags[3]!=0; // +4f
    if(r.right){
        V target{};
        arm_target(in,[&](std::uint8_t s,V& out){return upper_body_right_hand_target(entries,in.component_780,s,out);},
            target,state.weight_308);
        upper_body_ik_reach(entries,nodes,target,20,state.weight_308);
        const UpperBodyHandInput hand{in.component_250,in.component_6b0,in.target_d70,in.up_df0,in.look_d90,in.body_cd0};
        r.right_rotation=arm_rotation(in,[&](std::uint8_t s,V& out){return upper_body_right_hand_rotation(entries,hand,s,out);},
            state.rotation_990);
        upper_body_ik_set_rotation(entries,nodes,state.rotation_990,20);
    }
    r.left=rows[in.state_a].flags[2]!=0||rows[in.state_b].flags[2]!=0; // +4e
    if(r.left){
        V target{};
        arm_target(in,[&](std::uint8_t s,V& out){return upper_body_left_hand_target(entries,in.component_490,in.component_5c0,s,out);},
            target,state.weight_308);
        upper_body_ik_reach(entries,nodes,target,19,state.weight_308);
        const UpperBodyLeftHandInput hand{in.component_250,in.component_480,in.component_4f0,in.target_d70,in.up_df0,in.body_cd0};
        r.left_rotation=arm_rotation(in,[&](std::uint8_t s,V& out){return upper_body_left_hand_rotation(entries,hand,s,out);},
            state.rotation_930);
        upper_body_ik_set_rotation(entries,nodes,state.rotation_930,19);
    }
    return r;
}

std::array<float,12> upper_body_node_rows(const RootVector& q){
    const V q2{fin(q[0]+q[0]),fin(q[1]+q[1]),fin(q[2]+q[2]),fin(q[3]+q[3])};
    V a{},b{},c{};
    for(std::size_t k=0;k<4;++k){a[k]=fin(q[k]*q2[0]);b[k]=fin(q[k]*q2[1]);c[k]=fin(q[k]*q2[2]);}
    return {msub(fin(1.0F-b[1]),1.0F,c[2]),fin(a[1]+c[3]),fin(a[2]-b[3]),0.0F,
        fin(b[0]-c[3]),msub(fin(1.0F-a[0]),1.0F,c[2]),fin(b[2]+a[3]),0.0F,
        fin(c[0]+b[3]),fin(c[1]-a[3]),msub(fin(1.0F-a[0]),1.0F,b[1]),0.0F};
}

std::optional<RootVector> upper_body_root_offset(std::uint8_t state_a,std::uint8_t state_b,float blend_31c,
    const RootVector& node0_offset,const RootVector& offset_9f0){
    const bool a=state_a==0xc||state_a==0x10,b=state_b==0xc||state_b==0x10; // 001cde94..001cdeb4
    if(!a&&!b)return std::nullopt;
    const V& p=a?offset_9f0:node0_offset;                                    // s2
    const V& q=b?offset_9f0:node0_offset;                                    // s0
    V r{0.0F,0.0F,0.0F,1.0F};                                                // +1bc = 1.0 (001ce018)
    for(std::size_t k=0;k<3;++k)r[k]=msub(madd(fin(p[k]+0.0F),q[k],blend_31c),p[k],blend_31c);
    return r;
}

void upper_body_grip_step(RootVector& rotation_480,RootVector& offset_490,
    const std::optional<UpperBodyGunGrip>& gun,float dt){
    if(!gun)return;                                                          // 001b1c3c
    const V& a=gun->position_170;const V& b=gun->position_190;
    const V position{fin(a[0]+b[0]),fin(a[1]+b[1]),fin(a[2]+b[2]),1.0F};     // 003851f4, 001b1c7c
    // 0038515c..0038517c: vaddx.xyzw vf1, vf1, vf0; MAC == 0xf: every lane
    // zero and none signed.
    const V& q=gun->rotation_180;
    bool zero=true;
    for(std::size_t k=0;k<4;++k){
        const float r=q[k]+0.0F;
        if(r!=0.0F||sign_flag(r))zero=false;
    }
    const V rotation=zero?V{0.0F,0.0F,0.0F,1.0F}:q;                         // 0038519c sqc2 vf0
    const float t=fpu_min(fpu_max(fin(10.5F*dt),0.0F),1.0F);                 // 001b1ca8, 00192740
    rotation_480=upper_body_lerp(rotation_480,rotation,t);                   // 001b1cbc 00190750
    offset_490=lerp4(offset_490,position,t);                                 // 001b1cdc 001a9160
}

}
