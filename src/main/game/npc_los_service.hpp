#pragma once
// The deferred requests of the world's LOS manager (service 004ea0b0, vtable
// 004d9100 over 004d90a0; NPC_COMBAT.md section 5.5, milestone N5c). A
// request (key 005435a0, vtable 004d9180) sent to the manager's virtual +0c
// (00138a10 -> 0013a480) with mode byte +5c != 0 is not answered at once: the
// first inactive entry (+20 = 0) of the manager's pool (+20: 30 entries of
// 0xe0 bytes, 00135e70(manager, 0x1e)) takes the origin +30, displacement
// +40, the receiver +24, the result storage +2c (the entry's own +30 when
// null), the serial (*+28 = ++manager +48, 0 wrapping to 1, when +28 is not
// null; else 0), the objects +50 / +54 and the mask +58, and is appended to
// list +30 (mode 1) or list +3c (mode 2); another mode deactivates the entry
// again. No inactive entry: the request is dropped (returns 0).
//
// The manager's frame update (00138a10 for class 005436b0 -> 0013acd0), which
// the world's frame-update broadcast reaches after the route manager (the
// route manager 004ed9b0 is the world's first child, from the world's
// constructor 00137ab0; the LOS manager is appended by the game start
// 001350e0 at 001356f4): every entry of list +30, then the head of list +3c
// only, each executed by 0013ae20 (the provider *004ea0b8 set up with the
// entry, its exclusions cleared and the entry's objects added, then 003c26d0
// with the required mask 0 and the excluded mask = the low byte of +58, the
// stack controls (0, 1) selecting the executor 003c37a0): the result byte
// (hit) and, on a hit, the point go to the storage; with a receiver, the result
// message (key 005435a8, vtable 004d91c0: +30 the result byte, +40 the point,
// +c0 the serial) is sent to it with transport 1 (delivered at once); then the
// entry is deactivated and removed from its list.
//
// The geometric query itself is the host's (NpcLosTrace): the native world
// LOS host (world_los.hpp trace_world_los) answers it in play.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

namespace omega {

// The result message 005435a8 as its receivers read it. The point is
// meaningful only on a hit; on a miss 0013ae20 copies its uninitialized stack
// buffer, which no N5c receiver reads (nullopt here).
struct NpcLosResult {
    std::uint8_t hit_30{};
    std::optional<RootVector> point_40;
    std::uint32_t serial_c0{};
};
using NpcLosReceiver=std::function<void(const NpcLosResult&)>;

struct NpcLosRequest {
    RootVector origin_30{},displacement_40{};
    NpcLosReceiver receiver_24;                 // null: no result message
    std::uint32_t* serial_28{};                 // where the serial is written (null: none, serial 0)
    std::uint64_t object_50{},object_54{};      // excluded objects (0: none)
    std::uint32_t mask_58{};
    std::uint8_t mode_5c{};                     // 1 / 2 (0 is the immediate path, world_los.hpp)
};
struct NpcLosEntry {
    RootVector origin_00{},displacement_10{};
    std::uint8_t active_20{};
    NpcLosReceiver receiver_24;
    std::uint32_t serial_d0{};
    std::uint64_t object_d4{},object_d8{};
    std::uint32_t mask_dc{};
};
inline constexpr std::size_t npc_los_pool_size=30;
struct NpcLosService {
    std::array<NpcLosEntry,npc_los_pool_size> pool{};
    std::deque<std::size_t> list_30,list_3c;    // pool indices in list order
    std::uint32_t serial_48{};
};

// What 003c26d0 is asked (the provider set up with the entry): the hit point,
// or nullopt for no hit.
struct NpcLosQuery {
    RootVector origin{},displacement{};
    std::uint8_t excluded_mask{};
    std::array<std::uint64_t,2> objects{};
    std::uint8_t object_count{};
};
using NpcLosTrace=std::function<std::optional<RootVector>(const NpcLosQuery&)>;

// 0013a480 for a request with +5c != 0 (mode 0 throws: the immediate path is
// answer_los_request). Returns 1 when the request was queued.
bool npc_los_request(NpcLosService& service,const NpcLosRequest& request);
// 0013acd0 (the frame update).
void update_npc_los_service(NpcLosService& service,const NpcLosTrace& trace);
}
