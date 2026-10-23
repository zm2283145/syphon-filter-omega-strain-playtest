#pragma once
#include "spawned_actor_ctor.hpp"

namespace omega {
// Original 00158bc0..00159010: the AI runtime initializer called by the
// spawned-component constructors 00158800/001588d0. It lazily allocates the
// entity slot (+18, 0xc bytes, then 003d9d30 with actor +3264), the behaviour
// (+1ac, 0x60 bytes, 0016e0a0) and the sensor (+1b0, 0x80 bytes, 00161840),
// stores the runtime defaults, draws 00127dd0 for +6c and calls the live
// virtuals +15c (args component, 0x14, f12=0.0, f13=200.0 as raw bits) and
// +148 (component, 1). The +148 host return is that virtual's f0 raw bits.
// Locks, allocator, constructors, RNG and virtuals stay explicit services;
// a callback failure leaves earlier writes committed. v0 is not modelled
// (the original leaves the +148 virtual's v0; both callers ignore it).
void run_ai_runtime_init(SpawnedActorMemory&, SpawnedActorHost&,
                         std::uint32_t component, std::uint32_t actor);
}
