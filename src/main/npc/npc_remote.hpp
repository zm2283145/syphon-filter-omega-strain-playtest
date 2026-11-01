#pragma once
// 132ln5h13 (research/NET_INLEVEL.md 4.23): the remote NPC controller 0x004de2a0 (constructor 002d6030, 0x220 bytes)
// that the control message 0x00582730 kind 2 (receiver 001b3d98) installs on an NPC whose owner is another console.
// Its base is the AI component (00158740: the path follower at the actor's +3548, +141 = 0x34, the aggressiveness
// draw); it does not think (virtual +88 answers 2, so 00159590 never selects it) and steers the actor from the owner's
// cNetHuman block (actor +3584: +3c the speed, +40 the heading, +44..+4c the position) and its aim fields (+1e0 the
// block, +1e4 its +1c: +8 the target key, +c the subtarget, +10..+18 the point, +20 aiming, +21 firing).
// NpcAiController (npc_world.hpp) carries it as its remote mode (set_remote / make_fresh_ai); this header has its
// state and the pure parts of the update 002d5160.
#include "npc_ai.hpp"
#include "npc_perception.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {
// The cNetHuman's aim fields (+1e0 and +1e4 set); +21 is the +1e4 block's ready byte.
struct NpcRemoteAim {
    bool aiming_20{},firing_21{};
    bool point_valid_0{};
    std::uint32_t target_8{};
    std::int8_t subtarget_c{-1};
    RootVector point_10{};
};
// The cNetHuman's motion fields (actor +3584 set).
struct NpcRemoteMotion {
    float speed_3c{},heading_40{};
    RootVector position_44{};
};
// What the remote controller reads outside the actor (PORT-ONLY: LanNetCore's NPC block answers; the play links them).
struct NpcRemoteLink {
    std::function<std::optional<NpcRemoteAim>()> link;                  // nullopt: +1e0 or +1e4 clear
    std::function<bool()> fire_gate;                                   // 002c4f20 on (+1e4): max/current halfwords
    std::function<std::optional<NpcRemoteMotion>()> motion;             // nullopt: +3584 clear
    std::function<void(const RootVector& position)> write_position;     // the cNetHuman's +44..+4c
    std::function<std::uint32_t()> word_150;                            // the controller's +150 (0021e548)
    std::function<bool(std::uint32_t word)> object_live;                // 0040c540(&word) non-null with +4 == 0xbebaafde
    std::function<std::uint64_t(std::uint32_t key)> object_by_key;      // 0040c540 (0: none); unknown keys throw
    // 00157c44..00157c9c / 002d4554..: the Generator Message 0x004a4a68 (+0x24 the actor, +0x28 = 1) to the live
    // object of +150 with 003c9420(msg, object, 0).
    std::function<void(std::uint32_t word_150)> generator_message;
    // 134lt6: 002d4460's branch for a controller without +150 whose virtual +44 answers (NetNPC_13.c): cNPCInfoObject
    // 00436c00, GetOwner 00434440(&actor +c) (its answer unused), 0042d910(actor) when +141 bit 7, then 00436ba0(-1).
    std::function<void(bool terrain_141)> local_release;
    // 134lt6: 002d5160's lost contact, after the actor's +60(1): cNetObjectMgr_TerrainActivation 0042d790(actor).
    std::function<void()> terrain_activation;
};
// The remote controller's own fields.
struct NpcRemoteState {
    std::int32_t node_1e8{-1};          // +1e8: the node object (the install's node; -1 none)
    std::int32_t node_208{-1};          // +208: its index
    std::int32_t node_210{-1};          // +210: the install's current node when it had no hold / previous one
    float speed_200{};                  // +200: the speed the locomotion reads (virtual +d8)
    float timer_204{};                  // +204: the no-contact time (002d5a40)
    std::uint16_t burst_1ec{};          // +1ec: 002d48f0's remaining shots
    std::uint8_t mode_1ee{};            // +1ee: the ramp mode (1 up, 2 down)
    std::uint8_t flags_214{};           // +214: bit 0 follow, bit 1 15f9b0 at activation, bit 2 node teleport
    std::int32_t node_20c{-1};          // +20c: 0015f9b0's destination node at activation (bit 1; -1: the point +1f0)
    RootVector point_1f0{};             // +1f0: 0015f9b0's point at activation when +20c < 0
};
struct NpcRemoteBurstStep {
    std::uint16_t count{};
    float pause{},cooldown{};
    bool shoot{};
};
// The install's copies from an AI controller (002d6030, old +88 == 1): +141 bit 7 and +98, the node (+208 / +1e8)
// from the old path follower's +4c, else +54, else +50 (also +210), else the actor's +3548, and +141 bit 2 =
// virtual +164 & 1 of the old (npc_corpse_removable_164).
struct NpcRemoteInstall {
    std::uint8_t flags_141{0x34};
    float radius_squared_98{};
    NpcRemoteState state;
};
NpcRemoteInstall npc_remote_install(const NpcAiComponent* old,bool old_removable_164,std::int32_t start_node_3548,
    std::uint8_t new_flags_141);
// 002d5948..002d5a3c: the speed ramp of +200 toward f1 (step 0.015; mode 1 up to 1.0, mode 2 down to 0).
float npc_remote_ramp(float speed_200,std::uint8_t mode_1ee,float f1);
// 002d58a8..002d5944: the mode from 001600a0(pf, +208) (0 keeps `mode`, 3 down, 2 up, 1: from the current node,
// up within +40 of the block's position, else down; without a current node `mode`).
std::uint8_t npc_remote_route_mode(std::uint8_t order,std::uint8_t mode,const NpcPathFollower& pf,const NpcNodeGraph& graph,
    const RootVector& position_44);
// 002d5228..: |b - a| over xyz (vsub, w = 1; 0 when all three are zero, else sqrt((x*x + y*y) + z*z)).
float npc_remote_distance(const RootVector& a,const RootVector& b);
// 002d57a8..: the far teleport's test: distance > 4.0, actor +320c != 0x10, and actor +38 == 0x80 or a present
// physical component whose +2c8 is not below 0.5.
bool npc_remote_far_teleport(float distance,std::uint32_t mode_320c,std::uint32_t word_38,bool physical_present,float physical_2c8);
// 002d48f0: advance the remote controller's +1ec / +84 / +88 burst state.
NpcRemoteBurstStep npc_remote_burst_step(std::uint16_t count,float pause,float cooldown,float fire_delay,
    std::uint32_t actor_id,bool can_attack);
// 002d4b30: use the network point when +0 is set, moving it by actor row 1 * 20 when it is within 3 units.
RootVector npc_remote_no_fire_point(const NpcRemoteAim& aim,const std::array<RootVector,4>& actor_matrix,
    const RootVector& target_eye);
// 002c4f20: one process-wide initial reset, then increment current while it is below max.
bool npc_remote_fire_gate(std::uint16_t maximum,std::uint16_t& current,bool& reset);

// 134lt11 (research/NET_INLEVEL.md, "The NPC ownership transfer"): cNetworkTransferNpc (0x2e0 bytes, DWARF
// types\cNetworkTransferNpc.h), the payload at +30 of cNPC::GetCopyMsg 00158350's 0x310 cNetCopyMsg (vtable 004de110).
// PORT-ONLY boundary: the port's AI component is not a byte block, so these two functions read and write its named
// fields at the original's offsets. Everything else keeps the original's byte layout:
//   +000 cNpcTransfer (0xf0)            controller +60..+14f (a raw quadword copy; +f8 / +fc and +142..+14f are
//                                       padding, +144 reportToHitThreshold is read by no ported code: packed 0)
//   +0f0 cAiTransfer (0x14)             controller +44..+57 (state machine +44 / +48 / +4c / +50, team +54)
//   +110 cAiTargetTransfer (0x30)       the cAITarget +1ac: +00 last seen, +10 / +14 the view cone, +18 the node
//                                       group, +1c the priority, +20 / +21 the flag bytes (+22..+2f padding)
//   +140 cPathNavigatorTransfer (0x70)  the path follower +1b0: +00 / +10 / +20 vectors, +30 pathSaveDestination
//                                       (written by no ported code: packed 0), +40..+5c, +60 the flag byte
//   +1b0 cPathTransfer (0x20)           the follower's +74 cPath: +00 the destination, +10 its node, +14 / +18 the
//                                       first and last node (00161610 takes them from the node list when it has one)
//   +1d0 targetGobjId                   the cAITarget's +40 handle's ID (-1 when null or stale)
//   +1e0 cTargetAwareness[4]            the first four 0x40-byte awareness records (+30 list, front first); a missing
//                                       one gets only +8 = -1 (the static 0049c588)
inline constexpr std::size_t npc_transfer_size=0x2e0;
using NpcTransferBytes=std::array<std::uint8_t,npc_transfer_size>;
// What the transfer reads outside the component (the play links it; an empty member throws when it is needed).
struct NpcTransferLink {
    // The weak handle (object, id) resolves (0016ecc0's WGet: the object is non-null and its +c is id).
    std::function<bool(std::uint64_t object,std::int32_t id)> handle_live;
    // nGobjManager_LookupGOBJ 0040c540 of an ID: the object handle, or 0 when there is none.
    std::function<std::uint64_t(std::int32_t id)> lookup;
    // The object's controller +58 is set (0016e890: a target without one is cTargetManager_AddGobj'd to 004ef4c0).
    std::function<bool(std::uint64_t object)> has_controller;
    std::function<void(std::uint64_t object)> add_target;
};
// 00148b50 (npc_8.c): the sender's Get. A follower without a cPath first gets one (0x30, cPath_ctor 00161930 with
// its +78), as the original does; a buffer whose +0 no route has written is packed as zero (PORT-ONLY: the original
// copies the heap block's bytes). The bytes not listed above keep `out`'s content (the message constructor's).
void npc_transfer_get_00148b50(NpcTransferBytes& out,NpcAiComponent& c,const NpcPerception& p,const NpcTransferLink& link);
// 001489d0 (npc_8.c): the receiver's Put: cNpcTransfer, 0016efe0, 0016e890 (+40 from the ID, then each live
// awareness record pushed to the front of +30, so their order reverses), 001613e0, the cPath (0x30) when missing,
// then 001614c0: the 0x20 copy, the node array (+24 * 4) when missing and, with +14 >= 0, cPathSystem_RestorePath
// 0015f3d0(the route manager, +14, +18, path) (npc_route_rebuild; a null manager throws).
void npc_transfer_put_001489d0(const NpcTransferBytes& in,NpcAiComponent& c,NpcPerception& p,const NpcTransferLink& link,
    NpcRouteManager* routes);
}
