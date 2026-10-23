#pragma once

#include "spawned_actor_ctor.hpp"

#include <cstdint>

namespace omega {

// 0016f5b0..0016f68c. The inherited 00230240 constructor, existing
// component predicate, 003cc950 team getter, and new component's virtual
// SetTeam remain address-keyed host services. Vtable entries are read live
// from the EE memory image, including after callbacks that may change them.
std::uint32_t run_spawned_actor_ai_base(SpawnedActorMemory& memory,
                                        SpawnedActorHost& host,
                                        std::uint32_t component,
                                        std::uint32_t actor);

} // namespace omega
