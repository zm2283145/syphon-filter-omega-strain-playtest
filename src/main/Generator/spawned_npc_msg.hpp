#pragma once

#include "spawned_actor_ctor.hpp"

#include <cstdint>
#include <functional>

namespace omega {

// The component's virtual +0x54 (the "is player" predicate): the host gets
// the slot's target address and the component (a0) and returns the full
// 64-bit v0, which 0021d09c tests with beqz. It may write memory.
using SpawnedNpcPredicate =
    std::function<std::uint64_t(SpawnedActorMemory& memory, std::uint32_t target, std::uint32_t component)>;

// GetNPC(SpawnedNPCMsg) 0021d060..0021d0ec: a0 points at the script argument
// block whose word 0 is the message. With the message's +24 actor and that
// actor's +58 component non-null, the component's virtual +0x54 runs; a
// nonzero v0 (a player) gives 0. Otherwise +24 and +58 are read again (+24
// without a null check) and the component, or 0, goes through the identity
// 0014a670 (sw / lw: the low word). Returns that word.
std::uint32_t spawned_npc_msg_get_npc(SpawnedActorMemory& memory, std::uint32_t arguments,
                                      const SpawnedNpcPredicate& predicate);

}  // namespace omega
