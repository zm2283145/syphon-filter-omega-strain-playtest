#include "agent_update_timer.hpp"

namespace omega {

bool agent_update_timer_step(float& timer_1f0, float dt, bool multiplayer) {
    if (!multiplayer) return false;
    timer_1f0 += dt;
    if (timer_1f0 > 3.0F) {
        timer_1f0 = 0.0F;
        return true;
    }
    return false;
}

}
