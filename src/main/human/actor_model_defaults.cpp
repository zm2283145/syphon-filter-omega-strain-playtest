#include "actor_model_defaults.hpp"
#include "actor_model_context.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
std::array<ActorChannelCommand,23> actor_default_channel_commands(){
    std::array<ActorChannelCommand,23> commands{};
    for(std::size_t i=0;i<commands.size();++i){
        float value=0.0F;
        switch(i){
        case 0:case 1:case 5:case 6:case 9:case 14:case 15:case 16:case 17:case 22:value=0.5F;break;
        case 21:value=1.0F;break;
        default:break;
        }
        commands[i]=ActorChannelCommand{ActorChannelOperation::set,i,value,0,0,false};
    }
    return commands;
}

namespace {
// 001b0740: node word, then the weight word (lui/ori immediates).
constexpr std::array<HumanCentroidWeight,10> centroid_table{{
    {2,std::bit_cast<float>(0x3fa00000U)},    // 001b0764..0788
    {5,std::bit_cast<float>(0x3fa00000U)},    // 001b0798..07bc
    {4,std::bit_cast<float>(0x3e800000U)},    // 001b07cc..07f0
    {6,std::bit_cast<float>(0x3e800000U)},    // 001b0800..0824
    {7,std::bit_cast<float>(0x3d4ccccdU)},    // 001b0834..085c
    {11,std::bit_cast<float>(0x3d4ccccdU)},   // 001b086c..0894
    {17,std::bit_cast<float>(0x3e19999aU)},   // 001b08a4..08cc
    {18,std::bit_cast<float>(0x3e19999aU)},   // 001b08dc..0904
    {19,std::bit_cast<float>(0x3ccccccdU)},   // 001b0914..093c
    {20,std::bit_cast<float>(0x3ccccccdU)},   // 001b094c..0974
}};
}
std::span<const HumanCentroidWeight> human_centroid_table(){return centroid_table;}

std::vector<float> human_centroid_weights(std::size_t node_count){
    std::vector<float> weights(node_count,0.0F);                 // 001af1e0..1f8: 001af3e0 on each +170
    for(const auto& entry:centroid_table){                       // 001af248..2a0
        if(entry.node>=node_count)
            throw std::runtime_error("Human centroid weights: table node outside the node collection (001af330 has no range check)");
        weights[entry.node]=entry.weight;
    }
    return weights;
}

RootVector human_model_centroid(std::span<const std::array<float,16>> nodes,std::span<const float> weights){
    if(nodes.size()!=weights.size())throw std::runtime_error("Human centroid: one weight per node is required");
    ActorModelContextState state;
    ActorModelContextHost host;
    host.count=[&]{return nodes.size();};
    host.snapshot=[&](std::size_t i){
        ActorModelContextNode node;node.transform=nodes[i];node.center_weight=weights[i];
        return node;};   // bounds disabled: only the 003a9090 part is read here
    update_actor_model_context(state,true,host);
    return state.centroid;
}
}
