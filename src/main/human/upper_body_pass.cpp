#include "upper_body_pass.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <random>
#include <stdexcept>

namespace omega {
namespace {
using V=RootVector;
constexpr std::size_t n21=upper_body_entry_count;
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Upper-body pass: "+what);}

// One finite fill of the words the ported code cannot know. Run 0 places
// positions with w = 1 (what 001d5730 / 001d5fa0 store); later runs vary
// every lane.
struct Poison {
    std::mt19937 rng;
    std::size_t run;
    float unit(){return static_cast<float>(static_cast<double>(rng()>>8)*(1.0/16777216.0));}
    float range(float lo,float hi){return lo+(hi-lo)*unit();}
    V rotation(){
        for(;;){
            double q[4];
            double n=0.0;
            for(double& v:q){v=static_cast<double>(range(-1.0F,1.0F));n+=v*v;}
            if(n<0.01)continue;
            n=std::sqrt(n);
            return {static_cast<float>(q[0]/n),static_cast<float>(q[1]/n),static_cast<float>(q[2]/n),static_cast<float>(q[3]/n)};
        }
    }
    V position(){return {range(-60.0F,60.0F),range(-60.0F,60.0F),range(-60.0F,60.0F),run==0?1.0F:range(-2.0F,2.0F)};}
};
bool same(const V& a,const V& b){
    for(std::size_t k=0;k<4;++k)if(std::bit_cast<std::uint32_t>(a[k])!=std::bit_cast<std::uint32_t>(b[k]))return false;
    return true;
}
bool same_rows(const std::array<float,12>& a,const std::array<float,12>& b){
    for(std::size_t k=0;k<12;++k)if(std::bit_cast<std::uint32_t>(a[k])!=std::bit_cast<std::uint32_t>(b[k]))return false;
    return true;
}
bool same_slot(const UpperBodySampleSlot& a,const UpperBodySampleSlot& b){
    return a.cursor_10==b.cursor_10&&a.key_18==b.key_18&&a.next_key_1c==b.next_key_1c;
}
bool same_look(const UpperBodyLookRecord& a,const UpperBodyLookRecord& b){
    return same(a.world_20,b.world_20)&&same(a.local_30,b.local_30)&&same(a.world_40,b.world_40)&&same(a.local_50,b.local_50)
        &&std::bit_cast<std::uint32_t>(a.weight)==std::bit_cast<std::uint32_t>(b.weight);
}

struct Run {
    V root{};
    bool rooted=false;
    std::array<std::array<float,12>,n21> rows{};
    std::array<V,n21> composed{};
    float weight_308=0.0F;
    UpperBodySampleSlot a,b;
    UpperBodyArmsResult arms;
    UpperBodyLookRecord look;
};

Run run_once(const UpperBodyPassInput& in,const UpperBodyLegTargets& legs,float blend,std::size_t run){
    Poison p{std::mt19937(static_cast<std::mt19937::result_type>(0x1cb160U+run*0x9e3779b9U)),run};
    const auto& constants=upper_body_entry_constants();
    const auto parents=upper_body_parents(in.parent_bytes);
    std::array<UpperBodyEntry,n21> entries{};
    for(std::size_t i=0;i<n21;++i){
        UpperBodyEntry& e=entries[i];
        e.limit=constants[i].limit;e.reference=constants[i].reference;e.inverse=constants[i].inverse; // 001d7b20
        e.sampled=constants[i].sampled;e.composed_aim=constants[i].composed_aim;e.parent=parents[i];
        e.sampled_local=p.rotation();e.sampled_world=p.rotation();e.sampled_position=p.position();
        e.local=p.rotation();e.world=p.rotation();e.position=p.position();
        e.target=p.rotation();e.ik_world=p.rotation();e.ik_position=p.position();
    }
    std::array<UpperBodyNode,n21> nodes=in.nodes;
    for(std::size_t i=0;i<n21;++i)if(!in.composed_known[i])nodes[i].composed=p.rotation();
    Run r;
    r.a=in.a;r.b=in.b;
    upper_body_sample(r.a,r.b,in.weight_318,blend,in.aim.aim_cf0,entries,nodes);       // 001d6120
    const auto& rows=upper_body_state_rows();
    const UpperBodyStateRow& ra=rows[in.state_a];
    const UpperBodyStateRow& rb=rows[in.state_b];
    auto weights_a=ra.weights;
    auto weights_b=rb.weights;
    // Rows sharing one weight array clamp it twice; max(0, w) is idempotent.
    upper_body_mode_loop(ra.modes,weights_a,rb.modes,weights_b,blend,entries);              // 001cdd18
    if(const auto root=upper_body_root_offset(in.state_a,in.state_b,blend,nodes[0].offset,in.aim.root_offset_9f0)){
        nodes[0].offset=*root;r.root=*root;r.rooted=true;                                    // 001cdea0..001ce02c
    }
    upper_body_forward(entries,nodes,blend,in.aim.aim_cf0);                                  // 001d5730
    r.look=in.look_410;
    if(!in.look_known){
        r.look.world_20=p.rotation();r.look.local_30=p.rotation();r.look.world_40=p.rotation();r.look.local_50=p.rotation();
        r.look.weight=p.unit();
    }
    upper_body_look(r.look,entries,in.component,in.look_origin,in.dt);                     // 001ce04c..001ce078: +410
    UpperBodyArmsInput arms;
    arms.state_a=in.state_a;arms.state_b=in.state_b;arms.blend_31c=blend;
    arms.component_250=in.component_250;
    arms.component_480=in.component_480;arms.component_490=in.component_490;               // 001b1c28 (001d81a0)
    arms.component_4f0=p.rotation();arms.component_5c0=p.position();
    arms.component_6b0=p.rotation();arms.component_780=p.position();                         // state 24: unknown
    arms.target_d70=in.aim.target_d70;arms.up_df0=in.aim.up_df0;
    arms.look_d90=in.aim.look_d90;arms.body_cd0=in.aim.body_cd0;
    UpperBodyArmsState state{in.weight_308,p.rotation(),p.rotation()};                       // sp+990 / sp+930: unknown
    r.arms=upper_body_arms(entries,nodes,arms,state);                                       // 001ce080..001ce6b8
    r.weight_308=state.weight_308;
    upper_body_ik_reset(entries,0);                                                          // 001ce6bc
    // 001ce6c4..001ce710: s0 = max(entry 0 +b0, +b1) != 2.
    const bool s0=std::max(entries[0].mode_a,entries[0].mode_b)!=2;
    upper_body_leg_pass(entries,nodes,in.component_8,legs.target_b90,legs.rotation_a50,16,legs.flag_c0||s0); // 001ce734
    upper_body_leg_pass(entries,nodes,in.component_8,legs.target_b80,legs.rotation_a40,12,legs.flag_s1||s0); // 001ce75c
    upper_body_head(entries,in.component_250,r.look.local_30,r.look.local_50,14,r.look.weight); // 001ce764: +440 / +460 / +470
    upper_body_compose(entries,nodes,ra.selectors,rb.selectors,in.state_a!=in.state_b,blend); // 001d02b0
    for(std::size_t i=0;i<n21;++i){
        r.composed[i]=nodes[i].composed;
        r.rows[i]=upper_body_node_rows(nodes[i].composed);                                  // 001ce7c0..001ce830
    }
    return r;
}
}

UpperBodyPassResult run_upper_body_pass(const UpperBodyPassInput& input){
    const auto& rows=upper_body_state_rows();
    if(input.state_a>=rows.size()||input.state_b>=rows.size())fail("state outside the table");
    UpperBodyPassResult result;
    result.blend_31c=upper_body_smoothstep(input.weight_318);                                 // 001ccb18
    result.weight_308=input.weight_308;
    result.a=input.a;result.b=input.b;
    std::array<Run,upper_body_pass_runs> runs;
    try{
        // 001ccb60..001cdbc4 reads no pass state: one evaluation serves every run.
        const UpperBodyLegTargets legs=upper_body_leg_targets(input.legs,input.component,secondary_aim_inverse(input.component));
        for(std::size_t k=0;k<runs.size();++k)runs[k]=run_once(input,legs,result.blend_31c,k);
    }catch(const std::exception& e){
        result.failure=e.what();
        return result;
    }
    const Run& first=runs[0];
    for(std::size_t k=1;k<runs.size();++k){
        // The sampler cursors, +308 and the root offset read no unknown word;
        // a difference would mean an unknown word reached them.
        const char* differs=nullptr;
        if(!same_slot(runs[k].a,first.a)||!same_slot(runs[k].b,first.b))differs="001d6120 cursors";
        else if(std::bit_cast<std::uint32_t>(runs[k].weight_308)!=std::bit_cast<std::uint32_t>(first.weight_308))differs="+308";
        else if(runs[k].rooted!=first.rooted||(first.rooted&&!same(runs[k].root,first.root)))differs="node 0 +1b0";
        if(differs){result.failure=std::string(differs)+" depends on unknown words";return result;}
    }
    result.reported=true;
    result.a=first.a;result.b=first.b;
    result.weight_308=first.weight_308;
    result.arms=first.arms;
    if(first.rooted)result.root_offset=first.root;
    bool look_known=true;
    for(std::size_t k=1;k<runs.size();++k)look_known=look_known&&same_look(runs[k].look,first.look);
    if(look_known)result.look_410=first.look;
    for(std::size_t i=0;i<n21;++i){
        bool rows_known=true,composed_known=true;
        for(std::size_t k=1;k<runs.size();++k){
            rows_known=rows_known&&same_rows(runs[k].rows[i],first.rows[i]);
            composed_known=composed_known&&same(runs[k].composed[i],first.composed[i]);
        }
        if(rows_known)result.rows[i]=first.rows[i];
        if(composed_known)result.composed[i]=first.composed[i];
    }
    return result;
}

}
