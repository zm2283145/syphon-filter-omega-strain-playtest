#include "animation_player.hpp"
#include "secondary_aim.hpp"
#include "skeleton_pose.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float check(float v) {
    if (!std::isfinite(v)) throw std::runtime_error("Animation player nonfinite value");
    return v;
}
float add(float a,float b) { return check(a+b); }
float mul(float a,float b) { return check(a*b); }
// The EE float rules of the root collection 003ae750 (VU0 macro mode and the
// FPU compares): an operand with a zero exponent (a denormal) reads as a zero
// of the same sign, a result below the normal range flushes to a zero of the
// same sign (tools/ee/ee.py with daz, VU_ROUNDING.md). The VU clamps an
// overflow; that is not hosted (check throws). A multiply-add's product is
// rounded but not flushed; only the sum is.
float vu_in(float v) { return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v; }
float vu_out(float v) { return vu_in(check(v)); }
float vu_mul(float a,float b) { return vu_out(vu_in(a)*vu_in(b)); }
float vu_add(float a,float b) { return vu_out(vu_in(a)+vu_in(b)); }
float vu_madd(float acc,float a,float b) { const float p=vu_in(a)*vu_in(b); return vu_out(vu_in(acc)+p); }
float vu_msub(float acc,float a,float b) { const float p=vu_in(a)*vu_in(b); return vu_out(vu_in(acc)-p); }
// vmulax.w ACC,vf0,v / vmadday.w / vmaddaz.w / vmaddw.w: ((x+y)+z)+w, each times vf0.w = 1.
float vu_lane_sum(const RootVector& v) {
    float sum=vu_mul(1.0F,v[0]);
    for(unsigned k=1;k<4;++k) sum=vu_madd(sum,1.0F,v[k]);
    return sum;
}
// vaddx.xyzw v,v,vf0 then cfc2 vi17 (the MAC flag) == 0xf, every lane Z and
// none S, U or O: vf0 (identity). A denormal lane reads as zero there.
// Otherwise vmul.xyzw v,v, the w-lane sum, VU RSQRT Q = vf0.w/sqrt(sum) and
// vmulq.xyzw. A sum that flushes to zero (every lane below about 1.08e-19)
// reaches RSQRT of +0: Q is the VU's +MAX, the word 0x7fffffff, read as
// (2 - 2^-23) * 2^128 (VU User's Manual; the 132cp9 hardware vectors give
// +-7fffffff for a zero divisor). vmulq then scales each lane by it: the
// exact product rounded to binary32 (ee.py vu_zero_divisor, the
// vu_float_range rule), at most about 7.4e19, so never clamped. With vu_scale
// false the nonzero scale is the earlier double reciprocal (see
// animation_member_vu::rotation).
RootVector vu_normalized(RootVector q,bool vu_scale=true) {
    unsigned mac=0;
    for(unsigned k=0;k<4;++k) {
        const float lane=vu_add(q[k],0.0F);
        if(lane==0) mac|=8U>>k;
        if(std::signbit(lane)) mac|=0x80U>>k;
    }
    if(mac==0xf) return {0,0,0,1};
    RootVector squares{};
    for(unsigned k=0;k<4;++k) squares[k]=vu_mul(q[k],q[k]);
    const float sum=vu_lane_sum(squares);
    if(sum==0) {
        const double vu_max=std::ldexp(double((1U<<24)-1),105);
        for(auto& v:q) v=vu_out(static_cast<float>(double(vu_in(v))*vu_max));
        return q;
    }
    const float scale=vu_scale?vu_in(vu::rsqrt(sum)):vu_in(check(static_cast<float>(1.0/std::sqrt(double(sum)))));
    for(auto& v:q) v=vu_mul(v,scale);
    return q;
}
}
namespace animation_member_vu {
// 003b15b0 (the member merge called from grouping 003b0d00): c.eq.s of the
// member weight +4 against 0 (a denormal reads as zero) copies the
// contribution (word, weight, raw byte +8, quaternion +10). Otherwise each
// side whose raw byte is set is scaled by its weight (vmulx.xyzw 003b161c /
// 003b1674), the two are added (vadd.xyzw 003b16c0), the weights are added
// (add.s 003b16f8) and the raw byte is cleared. A leaf contribution's raw
// byte is always set (003b1740).
void merge(AnimationGroupMember& dst,const GraphLeafContribution& src) {
    if(vu_in(check(dst.weight))==0.0F) { dst={src.node_index,src.weight,true,src.rotation}; return; }
    const auto incoming=animation_root_vu::scaled(src.rotation,src.weight);
    const auto current=dst.raw_rotation?animation_root_vu::scaled(dst.rotation,dst.weight):dst.rotation;
    for(unsigned k=0;k<4;++k) dst.rotation[k]=vu_add(current[k],incoming[k]);
    dst.weight=vu_add(dst.weight,src.weight); dst.raw_rotation=false;
}
// 003adde0 (called per member from 003ada60 before the clip evaluator
// 003b17d0): a raw member's quaternion is passed as is; otherwise the same
// steps as the root normalization, RSQRT at 003ade70.
// PORT-ONLY (open, VU_ROUNDING.md): 003ade70 is vrsqrt, which rounds twice
// (a binary32 root, then a binary32 division); the scale here keeps the
// earlier double reciprocal (one rounding), up to 1 ulp off. The VU scale
// changes the recorded TORONTO1 routes, so it waits for their re-recording
// (checkpoint 132zf14 report, patch 3).
GraphQuaternion rotation(const AnimationGroupMember& member) {
    return member.raw_rotation?member.rotation:vu_normalized(member.rotation,false);
}
// 003b17d0 c.eq.s of the weight f12 against 0 (003b181c) returns before
// sampling; a denormal weight reads as zero.
bool sampled(float weight) { return vu_in(check(weight))!=0.0F; }
}
AnimationPlayerState make_animation_player(const SkeletonDefinition& skeleton,
    std::span<const RootAnchorKey> anchors,std::size_t channels,std::size_t selectors) {
    if(anchors.empty()||anchors.size()>256||channels>4096||selectors>4096)
        throw std::runtime_error("Animation player registry extent");
    AnimationPlayerState state;
    state.pose=make_initial_nonroot_pose(skeleton);
    state.root_local={1,0,0,0,0,1,0,0,0,0,1,0,skeleton.bones[0].local_offset.x,
        skeleton.bones[0].local_offset.y,skeleton.bones[0].local_offset.z,1};
    state.graph.channels.resize(channels);state.graph.selectors.resize(selectors);
    for(const auto& key:anchors) {
        if(std::any_of(state.anchors.begin(),state.anchors.end(),[&](const auto& a){return a.key==key;}))
            throw std::runtime_error("Animation player duplicate anchor");
        state.anchors.push_back({key,make_initial_root_animation(),std::nullopt,std::nullopt,0});
    }
    std::vector<std::array<float,16>> locals{state.root_local};
    for(const auto& bone:state.pose.bones) locals.push_back(bone.local);
    state.world=skeleton_world_pose(skeleton,locals);
    return state;
}
namespace animation_root_vu {
// 003ae7ec..003ae880 / 003aea08..003aea74: vmul.xyzw by the previous
// orientation, the w-lane sum, c.olt.s against 0 (the dot is a flushed VU
// result), the negation vmulax.xyzw ACC,vf0,vf0 (ACC = 0) then
// vmsubw.xyzw v,v,vf0 (0 - v*1, so +-0 and denormal lanes become +0), vadd.
void add_orientation(RootVector& sum,RootVector value,const RootVector& reference) {
    RootVector product{};
    for(unsigned k=0;k<4;++k) product[k]=vu_mul(value[k],reference[k]);
    if(vu_lane_sum(product)<0) for(auto& v:value) v=vu_msub(0.0F,v,1.0F);
    add(sum,value);
}
// vadd.xyzw into the running sum (003ae860 / 003ae878 / 003aea70 / 003aeac0).
void add(RootVector& sum,const RootVector& value) {
    for(unsigned k=0;k<4;++k) sum[k]=vu_add(sum[k],value[k]);
}
// 003ae9b8..003ae9c0: lwc1 of the anchor weight (root +d0), c.ole.s against 0,
// bc1t past the anchor. The EE FPU reads a denormal operand as zero.
bool weight_included(float weight) {
    return !(vu_in(check(weight))<=0.0F);
}
// vmulx.xyzw by the weight moved in with qmtc2 (003ae9e8 / 003aea9c).
RootVector scaled(RootVector value,float weight) {
    for(auto& v:value) v=vu_mul(v,weight);
    return value;
}
// 003ae8b0..003ae930 / 003aeaf8..003aeb78: the identity test at
// 003ae8b8..003ae8d8 and the RSQRT at 003ae91c / 003aeb64 (vu_normalized).
RootVector normalized(RootVector q) { return vu_normalized(q); }
}
void update_animation_player(AnimationPlayerState& state,const AnimationGraph& graph,
    const SkeletonDefinition& skeleton,const AnimationClipBank& clips,
    std::span<const AnimationRootRequest> roots,float dt,const RootMotionConstraint& constraint,
    const AnimationAfterGraph& after_graph) {
    if(check(dt)<0||roots.size()>256||state.groups.size()>4096)
        throw std::runtime_error("Animation player frame extent or time");
    auto next=state;
    graph.advance_selectors(next.graph,dt);
    for(auto& anchor:next.anchors)anchor.weight=0;
    std::vector<std::uint64_t> hosts;
    for(const auto& root:roots) {
        if(std::find(hosts.begin(),hosts.end(),root.host)!=hosts.end())
            throw std::runtime_error("Animation player duplicate host");
        if(check(root.initial_phase)<0||root.initial_phase>1)
            throw std::runtime_error("Animation player initial phase");
        hosts.push_back(root.host);
    }
    std::erase_if(next.groups,[&](const auto& g){
        return std::find(hosts.begin(),hosts.end(),g.host)==hosts.end()||
            (!g.active&&(g.number==-1||!g.phase.looping));
    });
    for(auto& group:next.groups) {
        if(group.number==-1) {
            if(group.members.size()!=1) throw std::runtime_error("Animation player singleton group corrupt");
            group.members[0].weight=0;group.members[0].raw_rotation=false;group.members[0].rotation={};
        } else group.members.clear();
        group.active=false;group.totals={};
    }
    std::size_t contributions=0;
    for(const auto& root:roots) {
        if(!root.submit_node) continue;
        const auto leaves=graph.evaluate(root.node,root.weight,root.rotation,next.graph);
        contributions+=leaves.size();
        if(contributions>10000) throw std::runtime_error("Animation player contribution limit");
        for(const auto& leaf:leaves) {
            const auto& policy=graph.nodes()[leaf.node_index].leaf;
            auto found=std::find_if(next.groups.begin(),next.groups.end(),[&](const auto& g){
                return g.host==root.host&&(policy.group==-1?
                    (g.members.size()==1&&g.members[0].node==leaf.node_index):g.number==policy.group);
            });
            if(found==next.groups.end()) {
                if(next.groups.size()==4096) throw std::runtime_error("Animation player group limit");
                AnimationPlayerGroup group;
                group.host=root.host;group.number=policy.group;group.phase.independent_bounds=true;
                group.phase.looping=true;group.phase.previous=group.phase.current=root.initial_phase;
                next.groups.push_back(group);found=std::prev(next.groups.end());
            }
            auto& group=*found;group.active=true;group.paused=root.paused;
            group.totals.total_weight=add(group.totals.total_weight,leaf.weight);
            group.totals.weighted_duration=add(group.totals.weighted_duration,mul(policy.duration,leaf.weight));
            auto member=std::find_if(group.members.begin(),group.members.end(),[&](const auto& m){return m.node==leaf.node_index;});
            if(member==group.members.end()) {
                group.members.push_back({leaf.node_index,leaf.weight,true,leaf.rotation});
                group.phase.looping=group.phase.looping&&policy.looping;
            } else animation_member_vu::merge(*member,leaf);
        }
    }
    if(after_graph) {
        const auto order=after_graph(next.groups,next);
        if(next.groups.size()>4096)throw std::runtime_error("Animation player callback group limit");
        if(order.size()>256)throw std::runtime_error("Animation player surviving host limit");
        for(std::size_t i=0;i<order.size();++i)
            if(std::find(order.begin(),order.begin()+i,order[i])!=order.begin()+i)
                throw std::runtime_error("Animation player duplicate surviving host");
        std::erase_if(next.groups,[&](const auto& group){return std::find(order.begin(),order.end(),group.host)==order.end();});
        std::stable_sort(next.groups.begin(),next.groups.end(),[&](const auto& a,const auto& b){
            return std::find(order.begin(),order.end(),a.host)<std::find(order.begin(),order.end(),b.host);
        });
    }
    std::vector<WeightedNonRootSample> bone_samples;
    std::vector<WeightedRootClipSample> root_samples;
    if(check(next.playback_rate)<0)throw std::runtime_error("Animation player negative playback rate");
    const auto group_dt=mul(dt,next.playback_rate);
    for(auto& group:next.groups) {
        step_animation_group(group.phase,group.active,group.totals.total_weight,
            group.totals.weighted_duration,group.paused?0:group_dt);
        if(!group.active) continue;
        for(const auto& member:group.members) {
            // 003ada60 takes the member quaternion through 003adde0 before
            // 003b17d0 tests the weight.
            const auto rotation=animation_member_vu::rotation(member);
            if(!animation_member_vu::sampled(member.weight)) continue;
            const auto& policy=graph.nodes().at(member.node).leaf;
            const auto clip=clips.find(policy.clip_key);
            if(clip==clips.end()) throw std::runtime_error("Animation player unresolved clip: "+policy.clip_key);
            const ClipPlaybackControl control{policy.reverse,policy.mirror,policy.looping,
                policy.range_start,policy.range_end,policy.phase_offset};
            bone_samples.push_back({member.weight,sample_nonroot_bones(clip->second,skeleton,group.phase.current,control)});
            root_samples.push_back({member.weight,sample_root_clip(clip->second,skeleton,group.phase.current,
                group.phase.advance,control,rotation),policy.motion_bucket,policy.align_sign});
        }
    }
    update_nonroot_pose(next.pose,bone_samples);
    std::vector<RootAnchorDefinition> definitions;
    for(const auto& anchor:next.anchors) definitions.push_back({anchor.key,anchor.state.sign_reference});
    const auto accumulated=accumulate_root_clip_contributions(definitions,root_samples);
    for(std::size_t i=0;i<next.anchors.size();++i)
        next.anchors[i].weight=accumulated.anchors[i].frame.weight;
    if(accumulated.bone0_translation_applied) {
        next.root_local[12]=next.root_local[13]=next.root_local[14]=0;next.root_local[15]=1;
    }
    if(accumulated.bone0_rotation_applied) {
        const std::array<float,12> identity{1,0,0,0,0,1,0,0,0,0,1,0};
        std::copy(identity.begin(),identity.end(),next.root_local.begin());
    }
    // 003ba2b0 on node 0: +1a0 (the empty-cache path reads node 0's saved inverse bind).
    if(accumulated.bone0_rotation_applied||next.bone0_rotation!=RootVector{})
        next.bone0_rotation=bone_rotation_cache(accumulated.bone0_rotation_accumulator,next.bone0_rotation,{});
    else next.bone0_rotation=bone_rotation_cache({},{},skeleton_bind_pose(skeleton).at(0).inverse_bind);
    const auto prior=next.orientation,prior_base=next.base_orientation;
    next.orientation={};next.position={};next.base_orientation={};next.base_position={};
    for(std::size_t i=0;i<next.anchors.size();++i) {
        auto& anchor=next.anchors[i];const auto& frame=accumulated.anchors[i].frame;
        const auto parent=anchor.parent?&*anchor.parent:nullptr;
        const auto output=finalize_root_animation(anchor.state,frame,anchor.constraint.value_or(constraint),parent);
        if(output.active) {
            animation_root_vu::add_orientation(next.orientation,*output.weighted_orientation,prior);
            animation_root_vu::add(next.position,*output.weighted_position);
        }
        if(animation_root_vu::weight_included(frame.weight)) {
            const auto placement=root_world_placement(anchor.state,parent);
            animation_root_vu::add_orientation(next.base_orientation,
                animation_root_vu::scaled(placement.orientation,frame.weight),prior_base);
            animation_root_vu::add(next.base_position,animation_root_vu::scaled(placement.position,frame.weight));
        }
    }
    next.orientation=animation_root_vu::normalized(next.orientation);
    next.base_orientation=animation_root_vu::normalized(next.base_orientation);
    next.position[3]=next.base_position[3]=1;
    std::vector<std::array<float,16>> locals{next.root_local};
    for(const auto& bone:next.pose.bones) locals.push_back(bone.local);
    next.world=skeleton_world_pose(skeleton,locals);
    graph.finish_selector_frame(next.graph);
    state=std::move(next);
}
}
