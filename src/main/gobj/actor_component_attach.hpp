#pragma once
#include "spawned_actor_ctor.hpp"

namespace omega {
// Original003cce30: release the saved old component, publish the replacement,
// then call its actual virtual+8c. Destruction, free and lock services remain
// explicit; a callback failure preserves earlier effects.
void run_actor_component_attach(SpawnedActorMemory&,SpawnedActorHost&,
                                std::uint32_t actor,std::uint32_t replacement);
}
