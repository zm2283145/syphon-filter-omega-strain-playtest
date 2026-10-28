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
}
