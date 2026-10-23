#pragma once
// The play side of a spawned NPC's deferred delete: 003cb4f0 -> 0040bf50 runs
// the actor's virtual +8, the destructor 001b57c0 (spawned_npc_dtor.hpp), with
// a1 = -1. The destructor runs on an image of the actor built from the native
// state of a despawned actor (deactivated by 001b5190, removed by 0040b720);
// its child calls that have a native counterpart go to the services, the
// releases of memory the native port does not model are recorded, and every
// other child call throws naming its address (SpawnedActorHost). See
// research/GENERATOR_RUNTIME.md "Checkpoint 119f".
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace omega {

struct SpawnedNpcTeardownInput {
    std::uint32_t id_c{};                       // actor +c
    std::array<std::uint8_t,3> root{};          // actor +2c / +2d / +2e
    std::uint8_t ai_inactive_159{};             // AI component +159
    float stun_33bc{-1.0F};                     // actor +33bc (ActorStunState::elapsed_33bc)
};

struct SpawnedNpcTeardownServices {
    // 00185d40(actor, kind, 0, 1.0f) for kinds 0, 1, 2 (the effect slots off).
    std::function<void(std::uint32_t kind)> effect_off;
    // 00144ba0(inventory, hand) for hands 0, 1 (the looping fire sound stop).
    std::function<void(std::uint8_t hand)> stop_loop_sound;
    // 003caa50(004ffb50, 004ed9b0) -> 00163c50(manager, actor): the actor's
    // queued route requests.
    std::function<void()> route_release;
    // 0020eba0(msg, 004f12d0, actor) sent with 003c9420(msg, 004ffb50, 1):
    // the stun's sound stop (only while +33bc is not negative).
    std::function<void()> stop_stun_sound;
};

struct SpawnedNpcTeardownResult {
    // Every child call of the destructor tree in order ("addr(args)"), the
    // service ones and the recorded releases alike.
    std::vector<std::string> calls;
};

// The image addresses (fixed; the actor's +c is the input's).
inline constexpr std::uint32_t spawned_npc_teardown_actor=0x01000000;
inline constexpr std::uint32_t spawned_npc_teardown_inventory=0x01100000;
inline constexpr std::uint32_t spawned_npc_teardown_component=0x01200000;

// Runs 001b57c0(actor, -1) on the image. Throws when the actor would release
// its physical component (+2d && +2c && !+2e: 001b5ae4, not hosted; a
// despawned actor was deactivated, which clears +3310 / +3314 at 001b531c),
// and for every unhosted child call.
SpawnedNpcTeardownResult run_spawned_npc_teardown(const SpawnedNpcTeardownInput& input,
                                                  const SpawnedNpcTeardownServices& services);

}
