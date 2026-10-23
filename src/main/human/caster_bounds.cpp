#include "caster_bounds.hpp"
#include "actor_model_context.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
// 001afd50: node, radius (0x3f400000, 0x3ed55555, 0x3f800000, 0x3eaaaaab, 0x3f2aaaab).
constexpr std::array<HumanBoundSphere,12> human_table{{
    {0,std::bit_cast<float>(0x3f400000U)},{4,std::bit_cast<float>(0x3ed55555U)},{6,std::bit_cast<float>(0x3ed55555U)},
    {12,std::bit_cast<float>(0x3f400000U)},{16,std::bit_cast<float>(0x3f400000U)},{14,std::bit_cast<float>(0x3f800000U)},
    {13,std::bit_cast<float>(0x3eaaaaabU)},{15,std::bit_cast<float>(0x3eaaaaabU)},{17,std::bit_cast<float>(0x3eaaaaabU)},
    {18,std::bit_cast<float>(0x3eaaaaabU)},{19,std::bit_cast<float>(0x3f2aaaabU)},{20,std::bit_cast<float>(0x3f2aaaabU)}}};
constexpr float attachment_radius=std::bit_cast<float>(0x3e99999aU);   // 0038a034
float finite(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor bounds need finite node matrices and points");
    return v;
}
ShadowMatrix node_matrix(const std::array<float,16>& m){
    ShadowMatrix r;
    for(std::size_t c=0;c<4;++c)for(std::size_t k=0;k<4;++k)r.column[c][k]=finite(m[c*4+k]);
    return r;
}
void include(ActorBounds& b,const ShadowVector& v){   // 00182f00: vmini.xyz / vmax.xyz
    for(std::size_t c=0;c<3;++c){b.minimum[c]=std::min(b.minimum[c],v[c]);b.maximum[c]=std::max(b.maximum[c],v[c]);}
}
}
std::span<const HumanBoundSphere> human_bound_spheres(){return human_table;}

ActorBounds actor_context_bounds(std::span<const std::array<float,16>> nodes,std::span<const HumanBoundSphere> table){
    // 001af460: 001af720 on every node (enable 0, mode 1, radius 0, centre 004e1d90 = 0), then the table.
    std::vector<ActorModelContextNode> spheres(nodes.size());
    for(std::size_t i=0;i<nodes.size();++i){
        spheres[i].transform=nodes[i];spheres[i].bounds_at_translation=true;spheres[i].bounds_center={0,0,0,0};
    }
    for(const HumanBoundSphere& s:table){
        if(s.node>=nodes.size())throw std::runtime_error("Human bound-sphere node outside the skeleton node collection (001af330)");
        ActorModelContextNode& n=spheres[s.node];
        n.bounds_enabled=true;n.bounds_at_translation=true;n.bounds_center={0,0,0,1};n.radius=s.radius;   // 001af600
    }
    ActorModelContextHost host;
    host.count=[&]{return spheres.size();};
    host.snapshot=[&](std::size_t i){return spheres[i];};
    ActorModelContextState state;
    update_actor_model_context(state,true,host);   // 003a9220 (the centroid 003a9090 is not used here)
    return {{state.bounds_min[0],state.bounds_min[1],state.bounds_min[2],state.bounds_min[3]},
            {state.bounds_max[0],state.bounds_max[1],state.bounds_max[2],state.bounds_max[3]}};
}

ActorBounds human_context_bounds(std::span<const std::array<float,16>> nodes){
    return actor_context_bounds(nodes,human_table);
}

ActorBounds human_actor_bounds(std::span<const std::array<float,16>> nodes,const std::optional<ActorAttachmentSphere>& attachment){
    return actor_bounds(nodes,human_table,attachment);
}

ActorBounds actor_bounds(std::span<const std::array<float,16>> nodes,std::span<const HumanBoundSphere> table,
                         const std::optional<ActorAttachmentSphere>& attachment){
    ActorBounds b=actor_context_bounds(nodes,table);   // 00182760 copy of context +0x30/+0x40
    if(!attachment)return b;                     // 0038a100: render +0x2410 null
    const ActorAttachmentSphere& a=*attachment;
    if(a.node>=nodes.size())throw std::runtime_error("Attachment node outside the skeleton node collection (0019f430)");
    const ShadowMatrix m=node_matrix(nodes[a.node]);   // 0038a7c0: node +0x10
    const ShadowVector point{finite(a.point[0]),finite(a.point[1]),finite(a.point[2]),1.0F};
    // 0038a000..0038a030: mode = x, y and z all zero (vaddx.xyz with 0, MAC Z flags 0xe).
    const bool at_translation=point[0]==0.0F&&point[1]==0.0F&&point[2]==0.0F;
    const ShadowVector centre=at_translation?m.column[3]:shadow_transform(m,point);   // 0038a688 / 0038a658
    ShadowVector lower{},upper{};
    for(std::size_t c=0;c<4;++c){
        const float r=c<3?attachment_radius:0.0F;
        lower[c]=finite(centre[c]-r);upper[c]=finite(centre[c]+r);
    }
    include(b,lower);include(b,upper);   // 0038a6ec..0038a780
    return b;
}
}
