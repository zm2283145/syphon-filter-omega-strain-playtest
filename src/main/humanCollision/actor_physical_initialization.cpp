#include "float_bits.hpp"
#include "actor_physical_initialization.hpp"
#include "actor_physical_state.hpp"
#include "morph_resources.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float number(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Physical initialization requires finite normal-or-zero arithmetic");
    return value;
}
}
void initialize_actor_physical_state(ActorPhysicalInitializationState& state,const ActorPhysicalInitializationHost& host){
    if(!host.root||!host.world_pose||!host.external_vector||!host.world_placement)
        throw std::runtime_error("Physical initialization requires its real root and vector getters");
    const auto root=host.root();
    const auto pose=host.world_pose(root);
    const auto matrix=root_placement_matrix(pose);
    const auto input=host.external_vector();for(const auto value:input)number(value);
    RootVector target{};
    for(unsigned row=0;row<4;++row){
        auto value=number(matrix[row]*input[0]);
        for(unsigned column=1;column<4;++column)
            value=number(value+number(matrix[column*4+row]*input[column]));
        target[row]=value;
    }
    const auto placement=host.world_placement(root);
    for(const auto value:placement.position)number(value);
    auto next=state;
    next.integrator=reset_actor_physical_integrator(state.integrator,target);
    const RootVector up{0,0,1,0};
    next.contacts[0].position=placement.position;next.contacts[0].direction=up;next.contacts[0].scalar=0;
    next.contacts[1].position=placement.position;next.contacts[1].direction=up;next.contacts[1].scalar=0;
    next.flag_dc=0;next.flag_dd=1;next.primary_support_enabled=0;
    next.base_height=placement.position[2];next.contact_height=std::bit_cast<float>(0xff7fffffU);
    auto dot=number(up[0]*placement.position[0]);
    dot=number(dot+number(up[1]*placement.position[1]));
    dot=number(dot+number(up[2]*placement.position[2]));
    next.plane={0,0,1,number(-dot)};
    next.scalar_2c4=0;next.scalar_394=0;state=next;
}
}
