#pragma once

#include "spawned_actor_ctor.hpp"

#include <cstdint>

namespace omega {

struct SpawnedActorAiArgs {
    std::uint32_t actor{};
    std::uint32_t role{2};
    std::uint32_t pair{};
    std::uint32_t attachment{}; // a3, unused by the solo role-2 branch
};

// Solo role-2 001ae450..001ae7c8 composed with generator component
// 00158800..001588c0 or null-pair component 001588d0..0015897c.
// The base constructor 0016f5b0, AI runtime initializer 00158bc0 and component
// attachment 003cce30 (with its +8c binding 002300d0) are calls through the
// host, which the default SpawnedActorHost dispatches to their native bodies.
void run_spawned_actor_ai(SpawnedActorMemory& memory, SpawnedActorHost& host,
                          const SpawnedActorAiArgs& args);

} // namespace omega
