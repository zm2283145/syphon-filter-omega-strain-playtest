#pragma once
#include "animation_root.hpp"
#include <cstdint>

namespace omega {
// Recovered 003a8c80 solver and 0025d4b0 launch-speed policy. World positions
// have W=1. Caller supplies initialized runtime tuning and item definition.
// sqrt/rsqrt follow the interpreter's VU/FPU model (vu_float.hpp); atan2/tan
// are the executable's libm routines (game_libm.hpp), so the solver is
// bit-exact with 003a8c80 in the interpreter (tests: game_libm_tests).
struct ProjectileTrajectory {
    RootVector initial_velocity{},terminal_velocity{};
};
struct ProjectileLaunchTuning {
    float gravity{}; // Runtime004f5438.
    float type1_minimum_angle{}; // Runtime004f7fe0.
    float type7_gravity_offset{}; // Executable0048af98.
};
ProjectileTrajectory solve_projectile_trajectory(const RootVector& source,
    const RootVector& target,float gravity,float arc,float minimum_angle);
RootVector prepare_projectile_launch(const RootVector& source,const RootVector& target,
    float arc,std::uint8_t definition_type,std::int32_t maximum_speed,
    const ProjectileLaunchTuning& tuning);
}
