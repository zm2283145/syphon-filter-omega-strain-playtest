#pragma once

#include "spawned_actor_ctor.hpp"

#include <cstdint>

namespace omega {

// 00143df0 register inputs (a0..a3, t0, t1) and the caller's sp at the JAL
// (the callee's entry sp). The frame is stack-0xf0: t1 is spilled to the
// byte sp+0xaf, the second record lookup to sp+0xa8, and the multiplayer
// message object lives at sp+0xb0..0xec; all are reread at the original
// instruction sites. a3 is never read.
struct InventoryAddArgs {
    std::uint32_t inventory{}, item{}, give_ammo{}, unused_a3{}, preload{}, no_net{};
    std::uint32_t stack{};
};

// 00143df0..0014407c: add `item` to the weapon inventory (weapon.cc). Record
// lookups go through 00147840 on the database word 004ffd30; the slot comes
// from the class table 00489d60 (+30 byte), a second copy of a class-0 item
// moves to slot 1 while it is empty; with give_ammo the clip is the record's
// +70 and the spare rounds +70 x 001861f0(inventory +88, record +74); the
// attach point comes from 00489d78 (slot held) / 00489d70, 1 for slot 1 while
// slot 0 is held; a record +13c model is attached (0038c7d0 on actor +60);
// slots 6/7 clear actor byte +24a2+point; a hold-class-5 record is stored in
// the object at +0. With 005721c8 set and t1 clear the add message (vtable
// 004de610 after the 003c9e30 base constructor with 0052acc8) is sent by
// 003c9420 and destroyed by 003c9d60. 00145710 (preload) and the message
// transport are explicit host services. v0 is left by the last child and no
// hosted caller reads it; the native returns 0.
std::uint32_t run_inventory_add(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                const InventoryAddArgs& args);
// 00143c50..00143cb0: equip (00142c50(inv, k)) the first of slots 0..5 whose
// item word (+8+16k) is not -1. Returns 00142c50's v0; with every slot empty
// the caller's v0 is left (no hosted caller reads it) and the native returns 0.
std::uint32_t run_inventory_equip_first(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                        std::uint32_t inventory);
// 00147840: the 0x140-byte record of `item` in the weapon database: +14 base
// plus item x 0x140, item 0 when it is negative or not below (+4)->+4.
std::uint32_t run_weapon_record(const SpawnedActorMemory& memory, std::uint32_t database,
                                std::uint32_t item);
// 001861f0: clips for an actor. For a player (actor +58 virtual +54) whose
// agent data (001314f0(004ffb50, slot): slot = team byte - 1 in multiplayer,
// 0 solo, -1 when the second predicate fails) has unlock 5 (00332f90),
// trunc(clips x f(0048a2d0)), plus one when that equals clips; otherwise clips.
std::uint32_t run_clips_for_actor(SpawnedActorMemory& memory, SpawnedActorHost& host,
                                  std::uint32_t actor, std::uint32_t clips);
// 001314f0 and 00332f90 are run_agent_record_accessor / run_agent_unlock_test (agent_perks.hpp).
// 0038c7d0(model, point, part): unless part is non-null with byte +40c >= 8,
// store part at model +2410 + 4 x point and refresh (0038c5e0). Returns the
// store address model + 4 x point, which 0038c5e0 leaves in v0; when it
// skips, the caller's v0 is left (no hosted caller reads it; native 0).
std::uint32_t run_model_attach(SpawnedActorMemory& memory, SpawnedActorHost& host, std::uint32_t model,
                               std::uint32_t point, std::uint32_t part);
// 0038c5e0(model): the hand blend words +23d0..+23ec from the parts at +2410
// and +2414 (bytes +40d/+40e) and the per-skeleton (+2c0c) byte tables
// 00571510 / 00571528 (48 bytes per skeleton type). A pure leaf; it leaves
// the caller's v0.
void run_model_attach_refresh(SpawnedActorMemory& memory, std::uint32_t model);

} // namespace omega
