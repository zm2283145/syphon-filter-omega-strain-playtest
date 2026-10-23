#include "actor_fader.hpp"
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void finite(float value){
    if(!std::isfinite(value))throw std::runtime_error("Actor fader: nonfinite value");
}
}

void start_actor_fade(ActorFader& fader,std::uint8_t mode,float duration){
    finite(duration);
    if(duration<=0.0F)return;
    if(mode==1){
        fader.value_00=duration;
        fader.direction_08=-1.0F;
    }else if(mode==2){
        fader.value_00=0.0F;
        fader.direction_08=1.0F;
    }
    fader.duration_04=duration;
    fader.paused_0c=false;
}

void update_actor_fader(ActorFader& fader,float frame_dt){
    finite(frame_dt);
    finite(fader.value_00);
    finite(fader.duration_04);
    finite(fader.direction_08);
    if(frame_dt<0.0F)frame_dt=0.0F;
    if(frame_dt>0.05F)frame_dt=0.05F;
    if(fader.paused_0c||fader.duration_04<=0.0F)return;
    fader.value_00+=fader.direction_08*frame_dt;
    if(fader.value_00<0.0F||fader.value_00>fader.duration_04){
        fader.value_00=0.0F;
        fader.duration_04=0.0F;
    }
}

std::uint8_t actor_fader_alpha(const ActorFader& fader){
    finite(fader.value_00);
    finite(fader.duration_04);
    finite(fader.direction_08);
    // 0025e5b0: 255 * (+0 / +4) (div.s, then mul.s) while +c is clear and
    // +4 > 0; else 255 when +8 == 1.0 and 0 otherwise; +c set forces 0. The
    // word stored to 005383c4 is cvt.w.s (truncating, saturating) & 0xff.
    float alpha=0.0F;
    if(!fader.paused_0c&&fader.duration_04>0.0F){
        const float ratio=fader.value_00/fader.duration_04;
        alpha=255.0F*ratio;
    }else if(fader.direction_08==1.0F)alpha=255.0F;
    if(fader.paused_0c)alpha=0.0F;
    std::int32_t word=0;
    if(alpha>=2147483648.0F)word=2147483647;
    else if(alpha<=-2147483648.0F)word=static_cast<std::int32_t>(-2147483647-1);
    else word=static_cast<std::int32_t>(alpha);
    return static_cast<std::uint8_t>(word&0xff);
}

void schedule_actor_respawn_poll(ActorRespawnPoll& poll){
    poll.remaining_ms=15.0F;
    poll.scheduled=true;
}

bool advance_actor_respawn_poll(ActorRespawnPoll& poll,float elapsed_ms,const ActorFader& fader){
    finite(elapsed_ms);
    finite(poll.remaining_ms);
    if(elapsed_ms<0.0F)throw std::runtime_error("Actor respawn poll: negative script-clock delta");
    if(!poll.scheduled)return false;
    poll.remaining_ms-=elapsed_ms;
    if(poll.remaining_ms>0.0F)return false;
    if(fader.duration_04==0.0F){
        poll.scheduled=false;
        poll.remaining_ms=0.0F;
        return true;
    }
    poll.remaining_ms=15.0F;
    return false;
}

void schedule_actor_respawn_timer(ActorRespawnTimer& timer,float delay_ms,std::uint8_t stage){
    finite(delay_ms);
    if(delay_ms<0.0F)throw std::runtime_error("Actor respawn timer: negative delay");
    timer.remaining_ms=delay_ms;
    timer.stage=stage;
    timer.scheduled=true;
}

bool advance_actor_respawn_timer(ActorRespawnTimer& timer,float elapsed_ms){
    finite(elapsed_ms);
    finite(timer.remaining_ms);
    if(elapsed_ms<0.0F)throw std::runtime_error("Actor respawn timer: negative script-clock delta");
    if(!timer.scheduled)return false;
    timer.remaining_ms-=elapsed_ms;
    if(timer.remaining_ms>0.0F)return false;
    timer.scheduled=false;
    timer.remaining_ms=0.0F;
    return true;
}

ActorRespawnTimerEffects fire_actor_respawn_timer(ActorRespawnTimer& timer,bool game_state_15,bool zone_ready,
    ActorFader& fader,bool& invulnerable_33a4,std::uint8_t& byte_3269){
    ActorRespawnTimerEffects effects;
    if(!game_state_15)return effects;
    if(timer.stage==0){
        invulnerable_33a4=true;
        if(!zone_ready){schedule_actor_respawn_timer(timer,1000.0F,0);return effects;}
        start_actor_fade(fader,1,1.0F);
        effects.fade_in=true;
        effects.screen_effect=true;
        effects.extinguish=true;
        byte_3269=0;
        schedule_actor_respawn_timer(timer,3000.0F,1);
        return effects;
    }
    if(timer.stage==1){
        if(fader.duration_04==0.0F){
            invulnerable_33a4=false;
            effects.invulnerability_ended=true;
        }else schedule_actor_respawn_timer(timer,1000.0F,1);
        return effects;
    }
    throw std::runtime_error("Actor respawn timer: stage "+std::to_string(timer.stage)+" does not exist");
}
}
