#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace omega {
using ActorFrameSlot = std::uint64_t;
class ActorFrameService;
using ActorFrameCallback = std::function<void(ActorFrameService&,ActorFrameSlot,float)>;
struct ActorFrameState {
    std::uint32_t identity{}; // Original object word0c, independent of native slot.
    std::uint8_t flag2c{},flag2d{},flag2e{};
    std::int32_t world_key{-1}; // Original signed object word30.
};
struct ActorFrameCallbacks {
    ActorFrameCallback pre_logic; // virtual+4c, required even for ineligible actors
    ActorFrameCallback logic;     // virtual+48
    ActorFrameCallback pre_world; // virtual+54
    ActorFrameCallback world;     // virtual+50: model, physics, then morph
};
// Explicit native slots represent stable original pointer identities. This is
// the normal two-pass actor service, not the surrounding game/camera loop.
class ActorFrameService {
public:
    ActorFrameService()=default;
    ActorFrameService(const ActorFrameService&)=delete;
    ActorFrameService& operator=(const ActorFrameService&)=delete;
    // Establish spare pointer-array capacity before a run. During the first
    // pass, append is supported only within this bound (original00133654).
    // Relocation, removal and registration during the world pass are unhosted.
    void reserve_logic_capacity(std::size_t);
    void register_actor(ActorFrameSlot,const ActorFrameState&,ActorFrameCallbacks,bool in_logic_order=true);
    void remove_actor(ActorFrameSlot);
    ActorFrameState& actor(ActorFrameSlot);
    const ActorFrameState& actor(ActorFrameSlot) const;
    // Queue a pointer/identity snapshot if the actor's freshly read key is >=0.
    // Repeated submissions are preserved; this is not an identity set.
    void queue_world(ActorFrameSlot);
    // Concrete003ce650 boundary: optional attached-component virtual+40 first,
    // followed by a fresh key/identity read and queue insertion. The actual
    // derived logic callback chooses whether/when it reaches this tail.
    void logic_tail(ActorFrameSlot,float elapsed,const ActorFrameCallback& attached_component={});
    void run(float elapsed);
    std::size_t pending_count() const noexcept {return pending_.size();}
    bool running() const noexcept {return running_;}
private:
    struct Entry {ActorFrameState state;ActorFrameCallbacks callbacks;};
    struct Saved {ActorFrameSlot slot;std::uint32_t identity;};
    std::map<ActorFrameSlot,Entry> actors_;
    std::vector<ActorFrameSlot> logic_order_;
    std::size_t logic_capacity_{}; // Explicit bound, independent of STL growth.
    std::multimap<std::int32_t,Saved> pending_;
    bool running_{};
    bool first_pass_{};
};
}
