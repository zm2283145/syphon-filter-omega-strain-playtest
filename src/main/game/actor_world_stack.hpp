#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace omega {
struct ActorWorldObjectSnapshot {
    std::int32_t generation{};
    std::uint8_t byte_2c{},byte_2d{},byte_2e{};
};
struct ActorWorldStack {
    std::vector<std::uint64_t> objects; // Original first-node order; null sentinel omitted.
    std::uint64_t published{};
    std::int32_t published_generation{-1};
    bool pending{};
    std::uint64_t pending_object{};
};
struct ActorWorldStackHost {
    std::function<ActorWorldObjectSnapshot(std::uint64_t)> snapshot;
    std::function<void(std::uint64_t,ActorWorldStack&)> activate;
    std::function<void(std::uint64_t,ActorWorldStack&)> deactivate;
};
// Synchronous virtual+5c/+60 calls (original argument1). Callbacks may change
// this list; each original reread observes those changes. The owner must use a
// transaction draft for rollback across external callbacks; this helper does
// not defer callbacks or pretend their external effects can be rolled back.
// 00131320(world, object): with a non-empty list the first object's virtual
// +60 (argument 1), then the object's virtual +5c (argument 1), the published
// reference 00583878/0058387c = object and its +c generation, then the object
// inserted first (001313d0 -> 00138ea0). A null object throws (the original
// calls through it).
void push_actor_world_object(ActorWorldStack&,std::uint64_t,const ActorWorldStackHost&);
std::uint64_t pop_actor_world_object(ActorWorldStack&,const ActorWorldStackHost&);
void remove_actor_world_object(ActorWorldStack&,std::uint64_t,const ActorWorldStackHost&);
void finish_actor_world_pending(ActorWorldStack&,const ActorWorldStackHost&);
}
