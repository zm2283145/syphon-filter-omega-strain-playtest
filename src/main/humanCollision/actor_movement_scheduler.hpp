#pragma once
#include "actor_movement_adjustment.hpp"
#include <cstdint>
#include <optional>

namespace omega {
// Live services read by 001e7f40/001e8760. Each method is one original read or
// virtual call; hosts may run real side effects and are called in original
// order, never cached across calls. Actor ids are live object identities.
class ActorMovementWorldHost {
public:
    virtual ~ActorMovementWorldHost()=default;
    // Actor +4c equals runtime class key 0049d010 and +3204 bit 0 is set.
    virtual bool class_flag(std::uint64_t actor)=0;
    virtual std::uint8_t mode_108(std::uint64_t actor)=0; // virtual +108
    // Controller at actor +58: nullopt when absent, else virtual +54/+58/+5c.
    virtual std::optional<bool> controller_54(std::uint64_t actor)=0;
    virtual std::optional<bool> controller_58(std::uint64_t actor)=0;
    virtual std::optional<bool> controller_5c(std::uint64_t actor)=0;
    // Physical object owned by the current actor.
    virtual std::int32_t physical_mode(std::uint64_t actor)=0;    // physical +20
    virtual bool physical_enabled(std::uint64_t actor)=0;         // physical +3d0
    virtual Bounds local_bounds(std::uint64_t actor)=0;           // virtual +88
    // Component +50: virtual +58 refresh only when dirty, then matrix +10.
    virtual CollisionMatrix refreshed_world_transform(std::uint64_t actor)=0;
    virtual bool byte_2c(std::uint64_t actor)=0;
    virtual bool byte_2d(std::uint64_t actor)=0;
    virtual bool byte_2e(std::uint64_t actor)=0;
    virtual std::uint8_t byte_14(std::uint64_t actor)=0;
    virtual std::uint8_t byte_49(std::uint64_t actor)=0;
    virtual RootVector position_e0(std::uint64_t actor)=0;        // virtual +e0
    // 1e8ca0 over the bound relation nodes (physical +c -> +100, target +2fc0).
    virtual bool related(std::uint64_t self,std::uint64_t target)=0;
    // Global list 004ea190. end() is captured once before traversal; next() is
    // read only after the node has been processed (including any message).
    virtual std::uint64_t list_begin()=0;
    virtual std::uint64_t list_end()=0;
    virtual std::uint64_t list_next(std::uint64_t node)=0;
    // Generation-checked handle at node +8: nullopt when stale or null.
    virtual std::optional<std::uint64_t> list_element(std::uint64_t node)=0;
    // Element +58 then +30; nullopt if either pointer is null.
    virtual std::optional<std::uint64_t> element_owner(std::uint64_t element)=0;
    virtual std::optional<std::uint64_t> player_slot(int slot)=0; // 003fb8d0
    // 003ae690(actor +2f70, key) +d0. Keys: 004a03d0 then 004a03d8.
    virtual float secondary_root_weight(std::uint64_t actor)=0;
    virtual float tertiary_root_weight(std::uint64_t actor)=0;
    // 003caa50(004ffb50, 004ee5c0) receiver, 0016b380(self +c, target +c, 7),
    // 003c9420 send with transport argument 0 (queued, not delivered inline).
    virtual void send_interaction(std::uint64_t self,std::uint64_t target,std::int32_t action)=0;
};

// 001e8760 with live reads in original order. Updates endpoint X/Y on success.
bool adjust_actor_movement_pair_live(ActorMovementWorldHost&,std::uint64_t self,
    const Bounds& self_world_bounds,std::uint64_t target,float factor,RootVector& endpoint);

struct ActorMovementScheduleResult {
    RootVector adjustment{}; // adjusted endpoint minus original, XYZW (1e86e8)
    bool adjusted{};         // any pair returned true
};
// 001e7f40: current-actor gates, self bounds, generation-checked global list
// (with optional action-7 interaction), then player slots 0..3.
ActorMovementScheduleResult schedule_actor_movement_adjustment(ActorMovementWorldHost&,
    std::uint64_t self,float elapsed,const RootVector& endpoint);
}
