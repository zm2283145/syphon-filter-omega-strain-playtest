#pragma once
// The spawned NPC's destructor 001b57c0 (vtable 004da690 +8) and the parts of
// its teardown it calls directly or through the component: 003ccf30 (the +58
// component release), 00158550 (the AI component destructor, vtable 004d9380
// +8), 00144c70 (the inventory destructor), 001a02c0 (the actor's model
// context lists and target link) and the leaf 00184cd0 (actor virtual +7c,
// byte +3598). Every child call goes through SpawnedActorHost::call with the
// argument registers it is given (float registers as raw bits after them);
// virtual calls go to the address read from the object's vtable in memory.
// See research/GENERATOR_RUNTIME.md "Checkpoint 119f".
#include "spawned_actor_ctor.hpp"

#include <cstdint>

namespace omega {

// 001b57c0(actor, a1) with the caller's sp `stack` (frame stack - 0x240):
// returns the actor (0 for a null actor). a1's low 16 bits (signed) > 0 also
// free the actor (00100740).
std::uint32_t run_spawned_npc_dtor(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                   std::uint32_t actor, std::uint32_t a1, std::uint32_t stack = 0x1f00000);
// 003ccf30(actor): the +58 component's virtual +8(-1) and its free
// (0036b130 "0x4bd228", line 0x291) under the profiling lock, then +58 = 0.
void run_component_release(SpawnedActorMemory& memory, SpawnedActorHost& host, std::uint32_t actor);
// 00158550(component, a1): returns the component (0 for null).
std::uint32_t run_ai_component_dtor(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                    std::uint32_t component, std::uint32_t a1);
// 00144c70(inventory, a1): returns the inventory (0 for null).
std::uint32_t run_inventory_dtor(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                 std::uint32_t inventory, std::uint32_t a1);
// 001a02c0(actor): 001a08d0(+2ec0), 001a07f0(+2fc0), then the target link:
// linked when +3478 is set and its +c equals +347c. The linked branch
// (001a0348..001a0704) is not hosted and throws.
void run_actor_unlink(SpawnedActorMemory& memory, SpawnedActorHost& host, std::uint32_t actor);

} // namespace omega
