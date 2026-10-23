#include "actor_flashbang_hide.hpp"
#include "secondary_playback.hpp"

namespace omega {
ActorFlashbangHideResult handle_actor_flashbang_hide(std::uint32_t& flags_324c,std::int32_t mode_320c,
    SecondaryComponent* secondary){
    ActorFlashbangHideResult r;
    if(!secondary)return r;                                   // 0018d1b4
    if(mode_320c!=0&&mode_320c!=3)return r;                   // 0018d1bc..0018d1c8
    SecondaryOneShot event;                                   // sp+0x30: +24, +28, +2c, +30, +34 = 0
    r.requested=true;
    r.accepted=request_secondary_one_shot(*secondary,secondary_motion_name("flashbanghide"),1,event);   // 0018d274
    flags_324c|=0x100U;                                       // 0018d294..0018d29c
    return r;
}

bool handle_actor_flashbang_hide_end(std::uint32_t& flags_324c,SecondaryComponent* secondary){
    if(!secondary)return false;                               // 0018d154
    if(!((flags_324c>>8)&1U))return false;                    // 0018d15c..0018d168
    secondary->states.request_312=secondary->states.request_313;   // 001cae10
    flags_324c&=~0x100U;                                      // 0018d178..0018d184
    return true;
}
}
