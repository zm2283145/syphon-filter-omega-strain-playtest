#include "float_bits.hpp"
#include "actor_movement_adjustment.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Actor movement adjustment requires finite normal-or-zero arithmetic");
    return value;
}
float add(float a,float b){return checked(checked(a)+checked(b));}
float sub(float a,float b){return checked(checked(a)-checked(b));}
float mul(float a,float b){return checked(checked(a)*checked(b));}
void valid(Vec3 value){checked(value.x);checked(value.y);checked(value.z);}
void valid(const CollisionMatrix& matrix){
    for(float value:matrix)checked(value);
    if(matrix[3]!=0||matrix[7]!=0||matrix[11]!=0||matrix[15]!=1)
        throw std::runtime_error("Actor movement bounds require an affine transform");
}
// An indeterminate VU lane (NaN here): its value is not established, so it
// may be carried but never read (checked() rejects it).
constexpr float indeterminate=std::numeric_limits<float>::quiet_NaN();
// A VU FMAC result: an overflow clamps to +-FLT_MAX (ee_vu0.py rnd).
float vu_result(float value){
    if(std::isinf(value))return std::copysign(std::numeric_limits<float>::max(),value);
    return checked(value);
}
// 00182f30 / 00171b30, per lane: vmulax (the product is the result, so an
// overflow clamps), then vmadday, vmaddaz, vmaddw (each product is an operand
// of the add: one that overflows is not modelled by the VU interpreter, so the
// lane becomes indeterminate). Reached by the constructor's sentinel bounds
// (00183010: +-FLT_MAX) before an actor's first context rebuild.
RootVector transform(const CollisionMatrix& matrix,RootVector point){
    RootVector result{};
    for(unsigned row=0;row<4;++row){
        result[row]=vu_result(checked(matrix[row])*checked(point[0]));
        for(unsigned column=1;column<4;++column){
            const float product=checked(matrix[column*4+row])*checked(point[column]);
            if(std::isinf(product))result[row]=indeterminate;
            else if(!std::isnan(result[row]))result[row]=vu_result(result[row]+checked(product));
        }
    }
    return result;
}
// 00182f00 per lane; an indeterminate lane stays indeterminate.
float lane_min(float a,float b){return std::isnan(a)||std::isnan(b)?indeterminate:std::min(a,b);}
float lane_max(float a,float b){return std::isnan(a)||std::isnan(b)?indeterminate:std::max(a,b);}
}
bool actor_movement_related(std::span<const ActorMovementRelationNode> nodes,std::size_t source,std::size_t target){
    if(nodes.empty()||nodes.size()>65536||source>=nodes.size()||target>=nodes.size())
        throw std::runtime_error("Invalid actor movement relation node range");
    std::vector<unsigned> incoming(nodes.size());
    std::size_t links=0;
    for(std::size_t index=0;index<nodes.size();++index){
        const auto& node=nodes[index];
        if(node.parent&&*node.parent>=nodes.size())throw std::runtime_error("Invalid movement relation parent");
        if(node.children.size()>1000000-links)throw std::runtime_error("Movement relation link bound exceeded");
        links+=node.children.size();
        for(auto child:node.children){
            if(child>=nodes.size()||nodes[child].parent!=index||++incoming[child]!=1)
                throw std::runtime_error("Inconsistent movement relation child ownership");
        }
    }
    for(std::size_t index=0;index<nodes.size();++index)
        if(incoming[index]!=(nodes[index].parent?1U:0U))throw std::runtime_error("Missing movement relation child link");
    std::vector<std::uint8_t> colors(nodes.size());
    for(std::size_t index=0;index<nodes.size();++index){
        std::vector<std::size_t> chain;
        std::optional<std::size_t> cursor=index;
        while(cursor&&colors[*cursor]==0){colors[*cursor]=1;chain.push_back(*cursor);cursor=nodes[*cursor].parent;}
        if(cursor&&colors[*cursor]==1)throw std::runtime_error("Cyclic actor movement relation");
        for(auto visited:chain)colors[visited]=2;
    }
    auto root=source;while(nodes[root].parent)root=*nodes[root].parent;
    std::vector<std::size_t> pending{root};
    while(!pending.empty()){
        const auto node=pending.back();pending.pop_back();
        if(node==target)return true;
        for(auto child=nodes[node].children.rbegin();child!=nodes[node].children.rend();++child)pending.push_back(*child);
    }
    return false;
}
Bounds actor_movement_world_bounds(const Bounds& local,const CollisionMatrix& matrix){
    // Inverted local bounds are accepted: the constructor's sentinel (minimum
    // +FLT_MAX, maximum -FLT_MAX) is a real +2e80 value.
    valid(local.minimum);valid(local.maximum);valid(matrix);
    constexpr float largest=std::numeric_limits<float>::max();
    Bounds result{{largest,largest,largest},{-largest,-largest,-largest}};
    // Original eight-corner order: minimum corner, Z toggle, Y toggle, X toggle.
    for(unsigned corner=0;corner<8;++corner){
        const auto point=transform(matrix,{(corner&4)?local.maximum.x:local.minimum.x,
            (corner&2)?local.maximum.y:local.minimum.y,(corner&1)?local.maximum.z:local.minimum.z,1});
        result.minimum={lane_min(result.minimum.x,point[0]),lane_min(result.minimum.y,point[1]),lane_min(result.minimum.z,point[2])};
        result.maximum={lane_max(result.maximum.x,point[0]),lane_max(result.maximum.y,point[1]),lane_max(result.maximum.z,point[2])};
    }
    return result;
}
ActorMovementPairResult adjust_actor_movement_pair(const RootVector& endpoint,const ActorMovementPairInput& input){
    ActorMovementPairResult result{endpoint,false};
    if(!input.self_controller_54&&!input.other_controller_54)return result;
    if(!input.other_flag_2d||!input.other_flag_2c||input.other_flag_2e)return result;
    if(input.other_mode_108==2||input.other_mode_108==3||!input.physical_enabled_3d0||
        input.same_actor||input.excluded_by_relation)return result;
    RootVector delta{};
    for(unsigned i=0;i<4;++i)delta[i]=sub(endpoint[i],input.other_position[i]);
    delta[2]=0;
    const float squared=add(add(mul(delta[0],delta[0]),mul(delta[1],delta[1])),mul(delta[2],delta[2]));
    // Original immediate 0x40fae147 differs from decimal literal 7.84F.
    if(!(squared<std::bit_cast<float>(std::uint32_t{0x40fae147})))return result;
    const auto other_bounds=actor_movement_world_bounds(input.other_local_bounds,input.other_world_transform);
    // Only Z is read from the already transformed self bounds at this stage.
    const float minimum=checked(input.self_world_bounds.minimum.z),maximum=checked(input.self_world_bounds.maximum.z);
    if(minimum>maximum)throw std::runtime_error("Inverted actor movement height interval");
    if(minimum>checked(other_bounds.maximum.z)||checked(other_bounds.minimum.z)>maximum||squared==0)return result;
    // 001e8bd0: VU RSQRT Q = vf0.w/sqrt(|delta|^2), then vmulq.xyz.
    const float reciprocal=checked(vu::rsqrt(squared));
    const float factor=checked(input.factor);
    if(factor<0||factor>1)throw std::runtime_error("Actor movement factor outside supported zero-to-one interval");
    for(unsigned i=0;i<2;++i){
        const float unit=mul(delta[i],reciprocal);
        const float correction=mul(factor,sub(mul(2.8F,unit),delta[i]));
        result.endpoint[i]=add(input.other_position[i],add(delta[i],correction));
    }
    result.adjusted=true;
    return result;
}
bool actor_movement_pair_in_range(const RootVector& endpoint,const RootVector& other_position){
    RootVector delta{};
    for(unsigned i=0;i<4;++i)delta[i]=sub(endpoint[i],other_position[i]);
    delta[2]=0;
    const float squared=add(add(mul(delta[0],delta[0]),mul(delta[1],delta[1])),mul(delta[2],delta[2]));
    return squared<std::bit_cast<float>(std::uint32_t{0x40fae147});
}
ActorMovementFactors actor_movement_factors(float elapsed){
    checked(elapsed);if(elapsed<0)throw std::runtime_error("Negative actor movement elapsed time");
    return {std::min(1.F,mul(15.F,elapsed)),std::min(1.F,mul(22.F,elapsed))};
}
RootVector actor_movement_adjustment_delta(const RootVector& original,const RootVector& adjusted){
    RootVector result{};for(unsigned i=0;i<4;++i)result[i]=sub(adjusted[i],original[i]);return result;
}
RootVector apply_actor_movement_adjustment(const RootVector& intended,const RootVector& adjustment,float weight){
    RootVector result{};for(unsigned i=0;i<4;++i)result[i]=add(intended[i],mul(adjustment[i],weight));return result;
}
}
