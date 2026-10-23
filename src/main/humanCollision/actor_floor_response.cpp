#include "float_bits.hpp"
#include "actor_floor_response.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Floor response requires finite normal-or-zero arithmetic");
    return v;
}
float add(float a,float b){return checked(checked(a)+checked(b));}
float sub(float a,float b){return checked(checked(a)-checked(b));}
float mul(float a,float b){return checked(checked(a)*checked(b));}
}
ActorFloorResponseDraft apply_actor_floor_response(
    const ActorFloorResponseState& retained,const ActorFloorResponseInput& input){
    ActorFloorResponseDraft result{retained};
    auto& state=result.state;
    state.flag_dc=checked(input.secondary_weight)==0&&checked(input.tertiary_weight)==0;
    state.flag_dd=false;
    state.contact_blend=0;
    if(!input.contact)return result;
    state.contact_e8=true;
    const float height=checked(input.contact->height);
    const float allowance=input.previously_supported?
        (((input.flags34&2)!=0&&(input.flags34&4)==0)?.35F:1.25F):-.1F;
    const float base=sub(input.reference_height,.2F);
    if(sub(base,allowance)<=height&&checked(input.contact->normal_z)>.5F){
        result.support_accepted=true;
        if(base<=height){
            // These products precede the primary/override gate in the source.
            const float damping=mul(30,sub(input.velocity_z,input.relative_velocity_z));
            const float limit=std::abs(mul(5,input.force_limit_scale));
            const float spring=mul(add(.2F,sub(height,input.reference_height)),180);
            const float correction=sub(spring,damping);
            if(checked(input.primary_weight)>0&&!input.override_9c0){
                const float bounded=std::min(std::max(correction,-limit),limit);
                const RootVector vertical{0,0,bounded,0};
                for(unsigned i=0;i<4;++i)
                    state.force[i]=add(state.force[i],mul(vertical[i],input.mass));
                result.force_added=true;
            }
        }
        state.flag_dc=false;
        if(checked(input.primary_weight)>0||!input.flag13)state.flag_dd=true;
    }
    state.height_distance=sub(input.reference_height,height);
    const float amount=checked(std::abs(add(.2F,sub(height,input.reference_height)))/2.5F);
    state.contact_blend=std::min(std::max(amount,0.0F),1.0F);
    return result;
}
}
