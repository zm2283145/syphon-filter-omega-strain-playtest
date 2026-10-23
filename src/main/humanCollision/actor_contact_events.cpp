#include "float_bits.hpp"
#include "actor_contact_events.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float finite(float x){
    if(::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("Actor contact: unsupported nonfinite/subnormal arithmetic");
    return x;
}
void validate(const RootVector& v){for(float x:v)finite(x);}
float add(float a,float b){return finite(a+b);}
float sub(float a,float b){return finite(a-b);}
float mul(float a,float b){return finite(a*b);}
}
std::optional<ActorContactEvent> produce_actor_contact_event(
    const ActorContactEventInput& input,const ActorContactEventHost& host){
    if(!input.successful_hit)return std::nullopt;
    if(!input.destination||!host.contact_object||!host.component||!host.eligible||
       !host.component_velocity||!host.refresh_world_root||!host.normal_z||
       !host.comparison_object||!host.actor_velocity||!host.normal||!host.send)
        throw std::runtime_error("Actor contact: complete live host and destination required");
    const auto object=host.contact_object();
    if(!object)throw std::runtime_error("Actor contact: successful hit has no source object");
    const auto component=host.component(object);
    if(!component||!host.eligible(object))return std::nullopt;
    const auto velocity=host.component_velocity(component);validate(velocity);
    // Bounded finite interpretation of VU MAC==0x0f after adding zero in XYZW.
    // In particular, W-only motion must not be discarded as XYZ-zero.
    if(velocity[0]==0&&velocity[1]==0&&velocity[2]==0&&velocity[3]==0)return std::nullopt;
    host.refresh_world_root(input.root_key);
    const auto downward=-std::bit_cast<float>(std::uint32_t{0x3c23d70a});
    const auto normal_limit=std::bit_cast<float>(std::uint32_t{0xbf70902e});
    auto deliver=[&](std::uint8_t subtype)->std::optional<ActorContactEvent>{
        const ActorContactEvent event{input.destination,object,subtype,5,1};
        host.send(event);return event;
    };
    if(velocity[2]<downward&&finite(host.normal_z())<normal_limit){
        bool present=false;
        for(std::size_t i=0;i<3;++i)if(host.comparison_object(i)==object)present=true;
        if(!present)return deliver(2);
    }
    const auto actor=host.actor_velocity();validate(actor);
    RootVector difference{};
    for(std::size_t i=0;i<4;++i)difference[i]=sub(velocity[i],actor[i]);
    const auto normal=host.normal();validate(normal);
    RootVector products{};
    for(std::size_t i=0;i<4;++i)products[i]=mul(difference[i],normal[i]);
    const auto projection=add(add(products[0],products[1]),products[2]);
    if(projection>30)return deliver(1);
    return std::nullopt;
}
}
