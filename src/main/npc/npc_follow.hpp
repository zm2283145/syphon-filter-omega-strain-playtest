#pragma once
// Checkpoint 132zf12: AI mode 9 Follow, the handler 00153870 of the mode
// jump table 0049b6a0 (0xd90 bytes), entered by the script's Follow(NPC,
// GOBJ, float, float) 00148880 -> virtual +fc = 001488f0 (npc_follow,
// npc_script_control.hpp), and the path follower's follow point 0015fbd0.
// Interpreter goldens: tests/npc_follow_goldens.hpp (tools/ee/gen_npc_follow.py).
//
// Every message first resolves the target +f4 (0040c540). None: ResumeAIMode
// (virtual +e0 = 0014b570) and "not handled". Otherwise the goal is the
// target's virtual +f4 matrix times the offset (+100, +104, +108, +10c as
// floats; vmulax / vmadday / vmaddaz / vmaddw, all four lanes) plus the
// translation of a second +f4 call (vadd.xyzw), and the target region is:
// a player target (+58 virtual +54): 004ed9c0[003cc950] (the controller's
// +68); an AI target (+58 virtual +58): its path follower's +5c, and when its
// hold node +4c is >= 0 and is neither this follower's +4c nor +54, this
// follower's +4c = it and, with the actor's +14 set, the network message
// 002d7290(actor, 0x2d, 4, node, 0, 0, 0) (key 0051f0a8, transport 0);
// otherwise 0015d790(the target's +e8 row 3, this follower's +5c).
//
// AI message (004ee550) event 2, the tick (returns 1): d = goal - the actor's
// +f4 row 3 (w = 1.0) and its length (zero xyz: length 0, d kept); the
// speeds: an actor-class target (+4c = *0049d010): its and this actor's
// +3584 ? (+3584)+3c : +30e0; any other target: |its +58 virtual +74| (VU
// length, zero xyz 0) and |this actor's +58 virtual +74| (VU sqrt, no zero
// test). Clear = 1 unless the target is present (+2d, +2c, !+2e), then the
// target manager's line test 0022cb10(004f77c0, actor, target, 0, 0) == 0.
// With +ec != 3 and (target speed > 0 or NaN, or path +60 bit 3 clear):
// 0015fbd0(+1b0, goal + 10 * the target's +f4 row 1). Then: d <= 1.0: +ec = 0,
// +f0 = the target region; else d <= 6 + |offset xyz| or +ec == 3: with a
// target speed > 0 and clear, +ec = 1 when d . (the target's +f4 row 1) < -0.5
// (w lane: (x + y) + z of the vmul.xyzw products), else 2; else clear: +ec =
// 2; else +ec = 3, +f0 = 0015d790(this actor's +e8 row 3, +f0), the route
// buffer +74 allocated when missing (0036b180(0, 0x30), 00161930: +10 = +14 =
// -1, no node array, +24 = +78, count 0) and 0015efb0(manager, actor, +4c,
// -1, +f0, -1, the target region, goal, +74). The speed: s = the target
// speed; +ec 2 / 3: s = 2 s + Jog (virtual +148(2) = 0.5); +ec 1: s =
// max(0, 0.5 (s - Walk)). With a target speed <= 1e-5 (3727c5ac): +ec 0 / 1
// stop; else d > 6.0 (as doubles, 0011c750): Run (0.9) when +60 (virtual
// +d8) < Jog, else nothing; d <= 6.0: Walk. With a target speed above 1e-5:
// own speed <= 1e-5: Walk; else r = own / s; r < 0.9 (3f666666): +60 +
// 0.025 (3ccccccd) then capped at 1.0; r > 1.1 (3f8ccccd): +60 - 0.025, and
// below 0 Walk * 0.5. Each speed write is at priority 50 (+64 <= 50: +60,
// +64 = 50, +141 bit 6 clear). Last: stop -> movement state 0 at priority 20
// when +13c != 0 (0014d510); otherwise state 1 at priority 20 when +13c = 0.
// Event 3: 0015fa70(+1b0, goal, the target region) (its result). Event 0
// (enter): +ec = 0, +e8 = -1, 00163c50(manager, actor), +f0 = path +5c; 1.
// Event 1 (exit): +64 = 50 when below 51, then virtual +fc(0, 0, 0) (Follow
// with no target: nothing); 1. Path message (004ed9b8) events 5 / 6 with
// +ec = 3: 0015fa70(+1b0, goal, the target region) (its result); events 1 /
// 3: +e8 = 30, not handled. Anything else: not handled.
#include "npc_ai.hpp"
#include "npc_route.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace omega {
// What mode 9 reads beyond NpcPerceptionHost::object (the target's +c,
// +2c/+2d/+2e, +4c actor class, +58 controller kind, 003cc950, an AI's
// path follower +5c, +e8 row 3), NpcPerceptionHost::player_region
// (004ed9c0) and NpcPerceptionHost::visibility (0022cb10), and the route
// manager calls. An unset member throws naming its address when read.
struct NpcFollowHost {
    std::uint64_t self{};                                                       // the NPC's actor (component +30)
    std::uint8_t self_14{};                                                     // actor +14 (the 002d7290 gate)
    std::function<std::array<RootVector,4>(std::uint64_t object)> world_f4;     // virtual +f4 (the target's, the actor's)
    std::function<RootVector(std::uint64_t object)> position_e8;                // virtual +e8 row 3 (this actor)
    // An actor-class object's +3584 ? (+3584)+3c : +30e0.
    std::function<float(std::uint64_t object)> speed;
    // +58 virtual +74 (cNPC: 0022f510); nullopt: no +58 (the original
    // calls through null: throws).
    std::function<std::optional<RootVector>(std::uint64_t object)> velocity_74;
    // An AI target's path follower (+58 +1b0) +4c.
    std::function<std::int32_t(std::uint64_t object)> hold_4c;
    // 0015d790(point, previous) with the caller's f20 (npc_region_at): at
    // 00153a5c the f20 of the message's sender (as mode 7's 001547d4,
    // NpcGoToBodyHost::region_at)
    // (NpcAiHost::sender_f20: the base update's dt for an enter; nullopt
    // elsewhere), at 00153ee0 the actor's own speed; `site` names the call.
    std::function<std::int32_t(const RootVector& point,std::int32_t previous,std::optional<float> f20,const char* site)> region_at;
    // 00163c50(003caa50(004ffb50, 004ed9b0), actor).
    std::function<void()> release_routes;
    // 0015efb0(manager, actor, start, -1, region, -1, destination region,
    // point, buffer); its result is not read.
    std::function<void(std::int32_t start,std::int32_t region,std::int32_t destination_region,const RootVector& point,
        const std::shared_ptr<NpcRouteBuffer>& buffer)> request_route;
    // 0015fa70(+1b0, point, region); nullopt: the original returns an unset
    // stack byte (npc_route_to_point), which throws.
    std::function<std::optional<std::uint8_t>(const RootVector& point,std::int32_t region)> route_to_point;
};
// The network message 002d7290 builds (key 0051f0a8, vtable 004de4d0) and
// sends to the actor with transport 0: NpcAiSend::Kind::network with action
// = +ac (0x2d), action_kind = +a0 (4), node = +b0 (the node), +b4 / +b8 0.
inline constexpr std::uint32_t npc_network_message=0x0051f0a8;
// 00153870 (see above). Needs NpcAiHost::follow and NpcAiHost::sense.
bool npc_follow_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// 0015fbd0(pf, point, region) (the region is not read): with a route buffer
// +74 (none: nothing), its count +28 = 0 when positive, +0 = point, +10 = -1;
// +50 = -1, +48 = the count, +58 = -1, the route step 00160a00 (bit 3 = no
// segment), then +20 = +00 = point and +10 = point - the actor's +f4 row 3
// (vsub.xyzw).
void npc_follow_point(NpcPathFollower& pf,const NpcNodeGraph& graph,const RootVector& point,const NpcPathHost& host);
// 0036b180(0, 0x30) and 00161930(buffer, +78): +10 = +14 = -1, +20 = 0 (no
// node array), +24 = the follower's +78, +28 = 0; +0 and +18 stay heap
// content (+0 unset here; +18 keeps the NpcRouteBuffer default, read by
// 00160f60 only with +14 >= 0, which 00160e60 writes together with +18).
std::shared_ptr<NpcRouteBuffer> npc_new_route_buffer(const NpcPathFollower& pf);
}
