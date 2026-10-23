#include "actor_pad_actions.hpp"
#include <stdexcept>

namespace omega {
namespace {
ActorEdgeSlotState& slot(ActorPadActionState& s,std::size_t i){
    if(!s.slots)throw std::runtime_error("Actor pad action: physical edge slots are not hosted");
    return (*s.slots)[i];
}
// 001adb30(001adb70(P+0x3e0, i), 0)
void latch(ActorPadActionState& s,std::size_t i){latch_actor_edge_request(slot(s,i),false);}
// The +58 component's virtual +54 (IsPlayer); absent component reads false.
bool player_component(const ActorPadActionState& s){return s.component_is_player.value_or(false);}
}

bool handle_actor_pad_action(ActorPadActionState& s,std::int32_t action){
    switch(action){
    case 1:                                                       // 001a9a18
        if(s.physical_present&&s.physical_dd)s.flags_3248|=1U;
        return true;
    case 2:s.flags_3248&=~1U;return true;                       // 001a9a88
    case 9:s.flags_3248|=2U;return true;                        // 001a9ad0
    case 10:s.flags_3248&=~2U;return true;                      // 001a9ae8
    case 0x0f:                                                    // 001ab050
        if(!s.physical_present||s.mode_320c!=0)return true;
        if(s.byte_14&&player_component(s)){
            if(!s.attachment_a0)throw std::runtime_error("Actor pad action: +3524 object is unresolved");
            if(*s.attachment_a0)return true;
        }
        latch(s,2);latch(s,3);latch(s,8);
        if(s.byte_31b1)latch(s,4);
        return true;
    case 0x10:                                                    // 001ab140
        if((s.flags_3248>>4)&1U)return true;
        if(!s.physical_present||s.mode_320c!=0)return true;
        if(!s.byte_14&&player_component(s))return true;
        latch(s,1);
        return true;
    case 0x1a:case 0x1b:                                          // 001ab7d0 / 001ab870
        if((s.flags_3210>>16)&1U)return true;
        if(!s.physical_present)return true;
        if(!s.byte_14&&player_component(s))return true;
        latch(s,7);
        if(action==0x1a)latch(s,8);
        return true;
    case 0x1c:if(s.physical_present&&s.physical_9a5)s.byte_31b5=true;return true; // 001ab8f8
    case 0x1d:s.byte_31b5=false;return true;                    // 001ab950
    case 0x2c:case 0x2d:                                          // 001ac360
        if(s.life_state==2||s.life_state==3)return true;
        if((s.flags_324c>>4)&1U)return true;
        if(s.byte_14&&((s.flags_324c>>2)&1U))return true;
        if(s.byte_14&&s.physical_present&&s.physical_dc)return true;
        if(s.linked_model_3478)throw std::runtime_error("Actor pad action: 0x2c/0x2d with a linked model (+3478) is not native");
        return true;                                              // 001ac418 / 001ac8c0: nothing linked
    default:return false;
    }
}
}
