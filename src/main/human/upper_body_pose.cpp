#include "upper_body_pose.hpp"
#include "upper_body_math.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using V=RootVector;
using Bytes=std::array<std::uint8_t,upper_body_entry_count>;
using Words=std::array<std::uint32_t,upper_body_entry_count>;

using namespace upper_body_math;

// The tables after 001d7de0 (shared arrays, by address).
constexpr std::array<std::uint32_t,10> mode_addresses{0x0048a6e0U,0x0048a700U,0x0048a720U,0x0048a740U,
    0x0048a760U,0x0048a780U,0x0048a7a0U,0x0048a7c0U,0x0048a7e0U,0x0048a800U};
constexpr std::array<Bytes,10> mode_arrays{{
    {2,2,2,2,2,2,2,2,0,2,0,2,1,0,2,0,1,0,0,0,0},
    {2,2,2,2,2,2,2,2,0,3,0,2,1,0,3,0,1,0,0,0,0},
    {5,2,5,2,2,5,2,2,5,5,5,2,1,5,5,0,1,5,0,5,0},
    {2,2,5,2,2,5,2,2,5,5,5,2,1,5,5,4,1,5,4,5,4},
    {5,2,5,2,2,5,2,2,5,5,5,2,1,4,5,4,1,4,4,4,4},
    {5,2,5,2,2,5,2,2,5,5,5,2,1,4,5,4,1,4,4,4,4},
    {2,2,5,2,2,5,2,2,5,5,5,2,1,4,5,4,1,4,4,4,4},
    {2,2,5,2,2,5,2,2,3,5,3,2,1,4,5,4,1,4,4,4,4},
    {2,2,2,2,2,2,2,2,0,5,5,2,1,0,5,4,1,0,4,0,4},
    {2,2,2,2,2,2,2,2,2,0,2,2,1,4,0,4,1,4,4,4,4}}};
constexpr std::array<std::uint32_t,10> selector_addresses{0x0048a820U,0x0048a840U,0x0048a860U,0x0048a880U,
    0x0048a8a0U,0x0048a8c0U,0x0048a8e0U,0x0048a900U,0x0048a920U,0x0048a940U};
constexpr std::array<Bytes,10> selector_arrays{{
    {1,4,1,4,4,1,4,4,1,4,1,4,4,1,4,1,4,1,1,1,1},
    {1,4,1,4,4,1,4,4,2,4,2,4,4,2,4,2,4,2,2,2,2},
    {3,4,3,4,4,3,4,4,3,4,3,4,4,3,4,2,4,3,2,3,2},
    {1,4,3,4,4,3,4,4,3,4,3,4,4,3,4,4,4,3,4,3,4},
    {3,4,3,4,4,3,4,4,3,4,3,4,4,4,4,4,4,4,4,4,4},
    {3,4,3,4,4,3,4,4,3,4,3,4,4,4,4,4,4,4,4,4,4},
    {1,4,3,4,4,3,4,4,3,4,3,4,4,4,4,4,4,4,4,4,4},
    {1,4,3,4,4,3,4,4,3,4,3,4,4,4,4,4,4,4,4,4,4},
    {1,4,1,4,4,1,4,4,1,4,3,4,4,1,4,4,4,1,4,1,4},
    {1,4,1,4,4,1,4,4,1,1,1,4,4,4,1,4,4,4,4,4,4}}};
constexpr std::uint32_t one=0x3f800000U,minus_one=0xbf800000U,w02=0x3e4ccccdU,w04=0x3ecccccdU,
    w07=0x3f333333U;
constexpr std::array<std::uint32_t,11> weight_addresses{0x0048a3a0U,0x0048a400U,0x0048a460U,0x0048a4c0U,
    0x0048a520U,0x0048a580U,0x0048a5e0U,0x0048a640U,0x004f3180U,0x004f31e0U,0x004f3240U};
constexpr std::array<Words,11> weight_arrays{{
    {w02,0,0x3ee66667U,0,0,0x3f333334U,0,0,one,0x3f59999aU,one,0,0,minus_one,one,minus_one,0,minus_one,minus_one,0,0},
    {w02,0,0x3f19999aU,0,0,one,0,0,one,one,one,0,0,minus_one,one,minus_one,0,minus_one,minus_one,0,0},
    {one,0,one,0,0,one,0,0,one,one,one,0,0,0,one,0,0,0,0,0,0},
    {0,0,w02,0,0,w04,0,0,w04,w07,one,0,0,w04,one,minus_one,0,w04,minus_one,w04,0},
    {0,0,w02,0,0,w04,0,0,one,w07,one,0,0,minus_one,one,minus_one,0,minus_one,minus_one,0,0},
    {0,0,w02,0,0,w04,0,0,w04,w07,w07,0,0,w04,w07,0,0,w04,0,w04,0},
    {0x3f000000U,0,0x3f570a3eU,0,0,one,0,0,w04,one,one,0,0,w04,one,0,0,w04,0,w04,0},
    {0,0,0,0,0,0,0,0,0,w07,one,0,0,0,one,minus_one,0,0,minus_one,0,0},
    {},{},{}}};
struct StateIndex {std::uint8_t modes,selectors,weights;std::array<std::uint8_t,4> flags;};
constexpr std::array<StateIndex,upper_body_state_count> state_indices{{
    {0,0,9,{0,0,0,0}},{0,0,9,{0,0,0,0}},{3,3,3,{1,1,0,1}},{4,4,0,{1,1,1,1}},{3,3,3,{1,1,0,1}},
    {6,6,4,{1,1,1,1}},{3,3,3,{1,1,0,1}},{3,3,3,{0,1,0,1}},{4,4,0,{1,1,1,1}},{4,4,1,{1,1,1,1}},
    {4,4,0,{1,1,1,1}},{4,4,1,{1,1,1,1}},{5,5,2,{1,1,1,1}},{7,7,8,{0,0,1,1}},{7,7,8,{0,0,1,1}},
    {7,7,8,{0,0,1,1}},{5,5,2,{1,0,1,1}},{2,2,5,{0,0,0,0}},{2,2,5,{0,0,0,0}},{2,2,5,{0,0,0,0}},
    {2,2,5,{0,0,0,0}},{1,1,8,{0,0,0,0}},{2,2,6,{0,1,0,0}},{8,8,7,{1,1,0,1}},{9,9,10,{0,0,1,1}}}};

// 001d7b20's entry constants: +10 xyz, +20, +70, +b8 (+30 is the conjugate).
struct EntryWords {std::array<std::uint32_t,3> limit;std::array<std::uint32_t,4> reference;std::uint8_t sampled,aim;};
constexpr std::uint32_t l1=0x401ae5d1U,l2=0x402cb286U,l3=0x4130f1b8U,l4=0x416ed9ebU,l5=0x4108c733U,
    l6=0x40b32a46U,l7=0x40da8278U,l8=0x3ffffffdU,l9=0x3fda1f8dU;
constexpr std::array<EntryWords,upper_body_entry_count> entry_words{{
    {{one,one,one},{0,0,0,one},1,0},
    {{l1,l2,l3},{0x3f12d5e8U,0,0,0x3f51b3f3U},0,1},
    {{0x416ed9ebU,l5,l6},{0xbe05a8a9U,0,0,0x3f7dcf55U},1,0},
    {{l1,l2,l3},{0x3f12d5e8U,0,0,0x3f51b3f3U},0,1},
    {{l1,l2,l3},{0xbf248dbaU,0,0,0x3f441b7eU},0,1},
    {{l4,l4,l6},{0xbe05a8a9U,0,0,0x3f7dcf55U},1,1},
    {{l1,l2,l3},{0xbf248dbaU,0,0,0x3f441b7eU},0,1},
    {{l7,l4,l4},{0,0,0,one},0,1},
    {{l4,l3,l3},{0,0,0,one},1,1},
    {{l7,l7,l7},{0,0,0,one},1,1},
    {{l4,l3,l3},{0,0,0,one},1,1},
    {{l7,l4,l4},{0,0,0,one},0,1},
    {{l8,l4,l4},{0,0,0,one},0,1},
    {{l9,l9,l9},{0xbc77f66cU,0xbdafc880U,0xbe31239cU,0x3f7b26c4U},1,1},
    {{l7,l7,l7},{0,0,0,one},1,1},
    {{l9,l9,l9},{0xbc77f66cU,0x3dafc880U,0x3e31239cU,0x3f7b26c4U},1,1},
    {{l8,l4,l4},{0,0,0,one},0,1},
    {{l8,l8,l8},{0x80000000U,0x3ecfa828U,0xbecfa828U,0x3f51b3f3U},1,1},
    {{l8,l8,l8},{0,0xbecfa828U,0x3ecfa828U,0x3f51b3f3U},1,1},
    {{l8,l8,l6},{0x3ec3ef16U,0,0,0x3f6c835fU},1,1},
    {{l8,l8,l6},{0x3ec3ef16U,0,0,0x3f6c835fU},1,1}}};

V vec(const std::array<std::uint32_t,4>& w){return {fb(w[0]),fb(w[1]),fb(w[2]),fb(w[3])};}
}

const std::array<UpperBodyStateRow,upper_body_state_count>& upper_body_state_rows(){
    static const std::array<UpperBodyStateRow,upper_body_state_count> rows=[]{
        std::array<UpperBodyStateRow,upper_body_state_count> out{};
        for(std::size_t s=0;s<upper_body_state_count;++s){
            const StateIndex& x=state_indices[s];
            UpperBodyStateRow& r=out[s];
            r.modes=mode_arrays[x.modes];
            r.selectors=selector_arrays[x.selectors];
            for(std::size_t i=0;i<upper_body_entry_count;++i)r.weights[i]=fb(weight_arrays[x.weights][i]);
            r.flags=x.flags;
            r.modes_address=mode_addresses[x.modes];
            r.selectors_address=selector_addresses[x.selectors];
            r.weights_address=weight_addresses[x.weights];
        }
        return out;
    }();
    return rows;
}

const std::array<UpperBodyEntryConstants,upper_body_entry_count>& upper_body_entry_constants(){
    static const std::array<UpperBodyEntryConstants,upper_body_entry_count> entries=[]{
        std::array<UpperBodyEntryConstants,upper_body_entry_count> out{};
        for(std::size_t i=0;i<upper_body_entry_count;++i){
            const EntryWords& w=entry_words[i];
            out[i].limit={fb(w.limit[0]),fb(w.limit[1]),fb(w.limit[2]),0.0F};
            out[i].reference=vec(w.reference);
            out[i].inverse=conjugate(out[i].reference);
            out[i].sampled=w.sampled;
            out[i].composed_aim=w.aim;
        }
        return out;
    }();
    return entries;
}

std::array<int,upper_body_entry_count> upper_body_parents(
    std::span<const std::uint8_t,upper_body_entry_count> parent_bytes){
    std::array<int,upper_body_entry_count> out{};
    for(std::size_t i=0;i<upper_body_entry_count;++i)out[i]=parent_bytes[i]==0xffU?-1:static_cast<int>(parent_bytes[i]);
    return out;
}

void upper_body_mode_loop(std::span<const std::uint8_t,upper_body_entry_count> modes_a,
    std::span<float,upper_body_entry_count> weights_a,
    std::span<const std::uint8_t,upper_body_entry_count> modes_b,
    std::span<float,upper_body_entry_count> weights_b,
    float blend,std::span<UpperBodyEntry,upper_body_entry_count> entries){
    const float rest=fin(1.0F-blend);
    const auto aims=[](std::uint8_t m){return m==5||m==6;};
    for(std::size_t i=0;i<upper_body_entry_count;++i){
        weights_a[i]=max_zero(weights_a[i]);
        weights_b[i]=max_zero(weights_b[i]);
        float w=0.0F;
        if(!(blend<=0.0F)&&aims(modes_a[i])&&aims(modes_b[i]))
            w=madd(fin(rest*weights_a[i]),blend,weights_b[i]);
        else if(aims(modes_a[i]))w=weights_a[i];
        else if(aims(modes_b[i]))w=weights_b[i];
        entries[i].weight=w;
        entries[i].mode_a=modes_a[i];
        entries[i].mode_b=modes_b[i];
    }
}

bool upper_body_limit_rotation(RootVector& out,const RootVector& q,const RootVector& limit){
    bool zero=true;
    for(std::size_t k=0;k<3;++k){
        const float x=q[k]+0.0F;
        if(!std::isfinite(x))fail("nonfinite arithmetic");
        zero=zero&&x==0.0F;
    }
    if(zero){out=q;return false;}
    float s=fin(q[0]*q[0]);
    s=madd(s,1.0F,fin(q[1]*q[1]));
    s=madd(s,1.0F,fin(q[2]*q[2]));
    const float length=fin(0.0F+fin(vu::sqrt(s)));
    if(length==0.0F)fail("001d29f0 divides by a zero length");
    const float inverse=fin(vu::div(1.0F,length));
    const V n{fin(q[0]*inverse),fin(q[1]*inverse),fin(q[2]*inverse),0.0F};
    float e=fin(fin(n[0]*n[0])*limit[0]);
    e=madd(e,1.0F,fin(fin(n[1]*n[1])*limit[1]));
    e=madd(e,1.0F,fin(fin(n[2]*n[2])*limit[2]));
    if(e==0.0F)fail("001d29f0 divides by a zero limit sum");
    const float bound=fin(0.0F+fin(vu::div(1.0F,e)));
    if(fin(length*length)<=bound&&!(q[3]<0.0F)){out=q;return false;}
    const float w=fin(vu::sqrt(max_zero(fin(1.0F-bound))));
    const float scale=fin(vu::sqrt(bound));
    out={fin(scale*n[0]),fin(scale*n[1]),fin(scale*n[2]),w};
    return true;
}

RootVector upper_body_lerp(const RootVector& a,const RootVector& b,float t){
    V r{};
    for(std::size_t k=0;k<4;++k)r[k]=msub(madd(fin(a[k]+0.0F),b[k],t),a[k],t);
    const V sq{fin(r[0]*r[0]),fin(r[1]*r[1]),fin(r[2]*r[2]),fin(r[3]*r[3])};
    float s=fin(1.0F*sq[0]);
    s=madd(s,1.0F,sq[1]);
    s=madd(s,1.0F,sq[2]);
    s=madd(s,1.0F,sq[3]);
    // 0019078c: VRSQRT of zero gives the largest float (a zero +180 grip
    // reached by 001b1cbc, gen_upper_body_grip.py).
    const float q=s==0.0F?std::numeric_limits<float>::max():fin(vu::rsqrt(s));
    return {fin(r[0]*q),fin(r[1]*q),fin(r[2]*q),fin(r[3]*q)};
}

RootVector upper_body_quaternion_multiply(const RootVector& a1,const RootVector& a2){return multiply(a2,a1);}

void upper_body_forward(std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes,
    float blend,const RootVector& aim){
    if(entries.size()!=nodes.size()||entries.size()>upper_body_entry_count)fail("001d5730 node count");
    for(std::size_t i=0;i<entries.size();++i){
        UpperBodyEntry& e=entries[i];
        const UpperBodyNode& node=nodes[i];
        const std::uint8_t m=e.mode_a>e.mode_b?e.mode_a:e.mode_b;
        if(m==0)continue;
        if(e.parent>=static_cast<int>(entries.size()))fail("001d5730 parent outside the model");
        const bool parented=e.parent>=0;
        const V rotation=parented?entries[static_cast<std::size_t>(e.parent)].world:V{0.0F,0.0F,0.0F,1.0F};
        const V translation=parented?entries[static_cast<std::size_t>(e.parent)].position:V{};
        if(m==2)e.local=node.pose;
        else if(m==3||m==4)e.local=e.sampled_local;
        else if(m==5||m==6){
            const V inverse_parent=parented?conjugate(rotation):V{0.0F,0.0F,0.0F,1.0F};
            V sampled=parented?multiply(inverse_parent,e.sampled_world):e.sampled_world;
            V target{};
            if(e.composed_aim)target=node.composed;
            else{
                target=multiply(inverse_parent,aim);
                if(target[3]<0.0F)target=negate(target);
            }
            V pose=multiply(e.inverse,node.pose);
            sampled=multiply(e.inverse,sampled);
            target=multiply(e.inverse,target);
            if(pose[3]<0.0F)pose=negate(pose);
            hemisphere(sampled,target);
            V r=upper_body_lerp(pose,sampled,e.weight);
            upper_body_limit_rotation(r,r,e.limit);
            e.local=multiply(e.reference,r);
        }
        if(m==3||m==5){
            const std::uint8_t low=e.mode_a<e.mode_b?e.mode_a:e.mode_b;
            if(low!=m&&(low==2||low==3||low==4)){
                const V other=low==2?node.pose:e.sampled_local;
                const float f=m==e.mode_a?fin(1.0F-blend):blend;
                V x=e.local;
                hemisphere(x,other);
                V r{};
                for(std::size_t k=0;k<4;++k)r[k]=msub(madd(fin(other[k]+0.0F),x[k],f),other[k],f);
                e.local=r;
            }
        }
        if(m!=1)e.world=multiply(rotation,e.local);
        e.position=place(rotation,translation,node.offset);
    }
}

void upper_body_compose(std::span<const UpperBodyEntry> entries,std::span<UpperBodyNode> nodes,
    std::span<const std::uint8_t,upper_body_entry_count> selectors_a,
    std::span<const std::uint8_t,upper_body_entry_count> selectors_b,bool distinct,float blend){
    if(entries.size()!=nodes.size()||entries.size()>upper_body_entry_count)fail("001d02b0 node count");
    for(std::size_t i=0;i<entries.size();++i){
        const UpperBodyEntry& e=entries[i];
        UpperBodyNode& node=nodes[i];
        const auto source=[&](std::uint8_t selector)->V{
            if(selector==4)return e.target;
            if(selector==3)return e.local;
            if(selector==2)return e.sampled_local;
            return node.pose;
        };
        V out{};
        if(distinct&&selectors_a[i]!=selectors_b[i]){
            V x=source(selectors_a[i]),y=source(selectors_b[i]);
            hemisphere(x,e.reference);
            hemisphere(y,e.reference);
            out=upper_body_lerp(x,y,blend);
        }else{
            out=source(selectors_a[i]);
            hemisphere(out,e.reference);
        }
        node.composed=out;
    }
}

namespace {
// The 0x50-byte record 00190a00 initializes; 001d6120 reads only +10 and +30
// of it (+20 / +40 hold the root's xy and yaw quaternion).
struct SampleRecord {
    V translation{};                  // +10
    V rotation{0.0F,0.0F,0.0F,1.0F};  // +30
};
V key_vector(const UpperBodySampleSlot& s,std::int64_t word,std::size_t lanes){
    if(word<0||static_cast<std::uint64_t>(word)+lanes>s.keys.size())fail("001d6120 reads a key outside the clip");
    V v{};
    for(std::size_t k=0;k<lanes;++k)v[k]=fb(s.keys[static_cast<std::size_t>(word)+k]);
    return v;
}
// 001d6120's per-slot block: one bone of the record at the cursor.
SampleRecord sample(UpperBodySampleSlot& s){
    SampleRecord r;
    const auto cursor=static_cast<std::uint32_t>(s.cursor_10);
    if(!(cursor<s.bone_count))return r;
    const std::uint32_t bit=1U<<(cursor&31U);
    const float t=s.fraction_14;
    if(s.translation_mask_1c&bit){
        const V a=key_vector(s,s.key_18,3),b=key_vector(s,s.next_key_1c,3);
        if(cursor==0&&s.root_translation_14!=0)
            r.translation=lerp3({0.0F,0.0F,a[2],0.0F},{0.0F,0.0F,b[2],0.0F},t);
        else r.translation=lerp3({a[0],a[1],a[2],0.0F},{b[0],b[1],b[2],0.0F},t);
        s.key_18+=4;
        s.next_key_1c+=4;
    }
    if(s.rotation_mask_20&bit){
        if(cursor==0&&s.root_yaw_18!=0){
            key_vector(s,s.key_18,1);
            key_vector(s,s.next_key_1c,1);
            s.key_18+=4;
            s.next_key_1c+=4;
        }
        r.rotation=upper_body_lerp(key_vector(s,s.key_18,4),key_vector(s,s.next_key_1c,4),t);
        s.key_18+=4;
        s.next_key_1c+=4;
    }
    ++s.cursor_10;
    return r;
}
}

void upper_body_sample(UpperBodySampleSlot& a,UpperBodySampleSlot& b,float weight_318,float blend_31c,
    const RootVector& aim,std::span<UpperBodyEntry> entries,std::span<const UpperBodyNode> nodes){
    if(!a.present&&!b.present)return;
    if(entries.size()!=nodes.size()||entries.size()>upper_body_entry_count)fail("001d6120 node count");
    const bool both=!(weight_318<=0.0F)&&a.present&&b.present;
    UpperBodySampleSlot& single=a.present?a:b;
    for(std::size_t i=0;i<entries.size();++i){
        UpperBodyEntry& e=entries[i];
        const V offset{nodes[i].bone_offset[0],nodes[i].bone_offset[1],nodes[i].bone_offset[2],0.0F};
        V rotation{},position{};
        if(both){
            const SampleRecord ra=sample(a),rb=sample(b);
            if(e.sampled){
                V qa=ra.rotation,qb=rb.rotation;
                hemisphere(qa,e.reference);
                hemisphere(qb,e.reference);
                rotation=upper_body_lerp(qa,qb,blend_31c);
                for(std::size_t k=0;k<4;++k)
                    position[k]=msub(madd(fin(ra.translation[k]+0.0F),rb.translation[k],blend_31c),
                        ra.translation[k],blend_31c);
            }
        }else{
            const SampleRecord r=sample(single);
            rotation=r.rotation;
            position=r.translation;
        }
        if(!e.sampled)continue;
        for(std::size_t k=0;k<4;++k)position[k]=fin(position[k]+offset[k]);
        position[3]=1.0F;
        e.sampled_local=rotation;
        e.sampled_world=rotation;
        if(i!=0){
            if(e.parent<0||e.parent>=static_cast<int>(entries.size()))fail("001d6120 node without a parent entry");
            const UpperBodyEntry& p=entries[static_cast<std::size_t>(e.parent)];
            e.sampled_world=multiply(p.sampled_world,e.sampled_world);
            e.sampled_position=position;
            e.sampled_position=rotate_offset(p.sampled_world,e.sampled_position);
            e.sampled_position=add_translation(e.sampled_position,p.sampled_position);
        }else{
            e.sampled_world=multiply(aim,e.sampled_world);
            e.sampled_position=rotate_offset(aim,position);
        }
    }
}

}
