#pragma once

#include <cstdint>
#include <initializer_list>
#include <unordered_map>

namespace omega {

class SpawnedActorMemory {
public:
    std::uint32_t read(std::uint32_t address, unsigned size = 4) const;
    void write(std::uint32_t address, unsigned size, std::uint32_t value);
    const std::unordered_map<std::uint32_t, std::uint8_t>& bytes() const { return bytes_; }
private:
    std::unordered_map<std::uint32_t, std::uint8_t> bytes_;
};

// Every child JAL of 001b7b00 is addressed explicitly. Missing services throw.
// A host may write memory and return v0; model, inventory, AI and identity
// construction remain service boundaries until independently goldened. The
// argument list flattens defined inputs: 001b7940 includes f12/f13 raw bits,
// 001b7930 and 001b7920 include f12 raw bits, and 001b5fe0 includes its
// ninth (stack) name pointer and then the caller's sp (its entry sp) after the
// eight argument-register words. 00143df0 takes its six argument registers
// (a0..a3, t0, t1) and then the caller's sp. The default dispatch runs
// 0016f5b0, 003cce30, 00158bc0, 002300d0, 001ae450, 001b5fe0, the inventory
// services 00143df0 and 00143c50 with their leaves 00147840, 001861f0,
// 001314f0 / 00332f90 (agent_perks.hpp), 0038c7d0 and 0038c5e0, the
// destructor tree's nodes 001b57c0, 00158550, 00144c70, 003ccf30, 001a02c0
// and leaf 00184cd0 (spawned_npc_dtor.hpp), and the leaves 001294f8
// (strlen), 00129360 (strcpy), 001b6a60 (item-list init) and 0012f370
// (returns 0) natively.
class SpawnedActorHost {
public:
    virtual ~SpawnedActorHost() = default;
    virtual std::uint32_t call(SpawnedActorMemory& memory, std::uint32_t address,
                               std::initializer_list<std::uint32_t> args);
};

struct SpawnedActorArgs {
    std::uint32_t actor{}, matrix{}, node{}, manifest{}, health{}, items{};
    // pair and serial are written to the caller stack on entry, then read
    // live at the original instruction sites after possible host mutations.
    std::uint32_t role{2}, name{}, pair{}, serial{};
    std::uint32_t stack{0x1f00000};
};

// The solo role-2 constructor 001b7b00..001b8128, inclusive of its direct
// initialization, identity tag, serial fallback and handoff to 001b5fe0.
std::uint32_t run_spawned_actor_ctor(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                      const SpawnedActorArgs& args);

// ResetNpc's in-place rebuild 001b8130 (checkpoint 132sx9): the same body as
// 001b7b00 (frame 0x120) with the identity, the generator-spawned flag, the
// serial and an optional appearance in the caller's four stack words (sd at
// +0, +8, +10, +18), written on entry and read live. Role 2 only.
struct ResetActorArgs {
    std::uint32_t actor{}, matrix{}, node{}, manifest{}, health{}, items{};
    std::uint32_t role{2}, name{};
    std::uint32_t id_pointer{}, generator_spawned{}, serial{}, appearance{};
    std::uint32_t stack{0x1f00000};
};
std::uint32_t run_reset_actor_ctor(SpawnedActorMemory& memory, SpawnedActorHost& host, const ResetActorArgs& args);
// The same with the four stack words already in memory (00148600's sd at 001487ac..001487bc).
std::uint32_t run_reset_actor_ctor_in_place(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                            const ResetActorArgs& args);

// ResetNpc 00148600(component; script wrapper 001485e0 passes the NPC's AI
// component and returns 0), with stack the caller's sp: when the actor's +7c
// (generator-spawned +3598 for 004da690) answers 0 it saves the manifest name
// +3318 (strcpy to sp+110), max health +3398, the ID +c, the items +3588/+358c,
// the node +3548 and a copy of the template name +3264 (heap, 0036b180 line
// 0x1f02), takes the 64-byte placement from +33f0's +e8 (003cd1c0), deactivates
// the component (+4c, 00157ad0), reads the serial +31f0, destroys the actor in
// place (+8 with -1: 001b57c0, no free) and rebuilds it at the same address with
// 001b8130 (role 2, flag 0, the old serial, no appearance), then frees the
// template copy (0036b130 line 0x1f13). Every call goes through host.
void run_reset_npc(SpawnedActorMemory& memory, SpawnedActorHost& host, std::uint32_t component, std::uint32_t stack);

// 001b8080..001b80c4 (role 2): the serial +31f0 is the draw, except that a draw
// of -1 takes the counter 004ee968 (read twice, no write in between), which then
// increments (32-bit). Shared by run_spawned_actor_ctor and play's spawns.
std::uint32_t spawned_actor_serial(std::uint32_t draw, std::uint32_t& counter_004ee968);

} // namespace omega
