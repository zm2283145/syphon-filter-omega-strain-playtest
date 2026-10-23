#pragma once

#include "spawned_actor_ctor.hpp"

#include <cstdint>

namespace omega {

// 001b5fe0 register and stack inputs. `stack` is the caller's sp at the JAL
// (the callee's entry sp); the frame is stack-0x790 and the ninth argument
// `name` is the caller stack word at `stack`+0, which the body writes on
// entry (marshalling) and then rereads at the original instruction sites.
struct SpawnedNpcBodyArgs {
    std::uint32_t actor{}, matrix{}, manifest{}, health{}, armor{}, items{};
    std::uint32_t pair{}, role{2}, name{};
    std::uint32_t stack{0x1effed0};
};

// The spawned-NPC body 001b5fe0..001b6a54 (all roles, solo and multiplayer
// branches). Every child call goes through the host; the default
// SpawnedActorHost runs 001ae450, 00143df0, 00143c50, the perk tests 001314f0 / 00332f90 (agent_perks.hpp),
// 001294f8, 00129360 and 001b6a60 natively (00143df0 receives the body frame
// sp as its seventh word) and throws for every other address of the body.
// Returns the original v0: 0 when the skeleton file is missing (36def0
// true), otherwise 1.
std::uint32_t run_spawned_npc_body(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                   const SpawnedNpcBodyArgs& args);

// 001294f8: strlen (MMI zero-byte tests are exact; reads only).
std::uint32_t run_ee_strlen(const SpawnedActorMemory& memory, std::uint32_t text);
// 00129360: strcpy returning dst. The 16/8-byte aligned chunk loops read a
// whole chunk before writing it and stop at any chunk holding a NUL, so they
// are equivalent to this forward byte copy (including NUL), overlaps included.
std::uint32_t run_ee_strcpy(SpawnedActorMemory& memory, std::uint32_t dst, std::uint32_t src);
// 001b6a60 (with its leaf 001b6a90): zero +0/+4/+8, byte +0xc = 1, return p.
std::uint32_t run_item_list_init(SpawnedActorMemory& memory, std::uint32_t list);

} // namespace omega
