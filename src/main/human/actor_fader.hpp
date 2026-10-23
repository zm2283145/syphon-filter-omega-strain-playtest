#pragma once
#include <cstdint>

namespace omega {
struct ActorFader {
    float value_00{};
    float duration_04{};
    float direction_08{};
    bool paused_0c{};
};
struct ActorRespawnPoll {
    float remaining_ms{};
    bool scheduled{};
};

// 0025e6e0: mode 1 fades in, mode 2 fades out; any other mode only sets the
// duration. A positive duration also clears the pause byte +c; non-positive
// durations leave the retained fader state unchanged.
void start_actor_fade(ActorFader& fader,std::uint8_t mode,float duration);
// 0025e660 receives world dt, clamped here to [0, 0.05] as in 00133340.
void update_actor_fader(ActorFader& fader,float frame_dt);
// 0025e5b0: the original alpha byte, truncated from the binary32 ratio.
std::uint8_t actor_fader_alpha(const ActorFader& fader);
// 00176d30/00177c60: arm a 15 ms script-clock poll and repeat it while the
// fader is active. Returns true once the poll observes a completed fade.
void schedule_actor_respawn_poll(ActorRespawnPoll& poll);
bool advance_actor_respawn_poll(ActorRespawnPoll& poll,float elapsed_ms,const ActorFader& fader);

// The revive's timed event (vtable 004da970, handler 001ba130): actor +c,
// stage +10, script-clock milliseconds.
struct ActorRespawnTimer {
    float remaining_ms{};
    std::uint8_t stage{};
    bool scheduled{};
};
void schedule_actor_respawn_timer(ActorRespawnTimer& timer,float delay_ms,std::uint8_t stage);
// Returns true once the event is due; it is then unscheduled and the caller
// runs fire_actor_respawn_timer with timer.stage.
bool advance_actor_respawn_timer(ActorRespawnTimer& timer,float elapsed_ms);
struct ActorRespawnTimerEffects {
    bool fade_in{};               // 0025e6e0(fader, 1, 1.0)
    bool screen_effect{};         // 00260990(*004ffc30, 1, 0, 0, 0, 0), not hosted
    bool extinguish{};            // 00186360(actor, 0, 0, 0, 0, -1.0)
    bool invulnerability_ended{}; // +33a4 = 0
};
// 001ba130 for the due stage. game_state_15: byte 004ffba0 == 15 (otherwise
// nothing, not re-armed). zone_ready: 00224e00 for the local player's zone.
// Stage 0 ready: fade-in, screen effect, +33a4 = 1, fire off, +3269 = 0,
// {3000, stage 1}; not ready: +33a4 = 1, {1000, stage 0}. Stage 1: fader +4
// == 0 -> +33a4 = 0, else {1000, stage 1}.
ActorRespawnTimerEffects fire_actor_respawn_timer(ActorRespawnTimer& timer,bool game_state_15,bool zone_ready,
    ActorFader& fader,bool& invulnerable_33a4,std::uint8_t& byte_3269);
}
