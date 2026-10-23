#pragma once
#include "animation_root.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>

namespace omega {
// Shared game RNG 00127dd0: 64-bit LCG state, returns bits 32..62. The state is
// global in the original, so every consumer must advance the same caller state.
std::uint32_t next_game_random(std::uint64_t& state);
// EE cvt.s.w rounds toward zero; also prevents the host round-up of values near
// 2^31 that would select an out-of-range element after the /2^31 scaling.
float game_int_to_float(std::int32_t value);

struct WaterActorBounds { RootVector minimum{},maximum{}; };
// Actor view used by the water-effect manager. Actor id 0 is the null pointer.
// Every method call corresponds to one original read/virtual call; hosts may
// run real side effects, and the manager rereads afterwards like the original.
class WaterEffectActorHost {
public:
    virtual ~WaterEffectActorHost()=default;
    // Current +c generation word, or nullopt when the object no longer exists.
    virtual std::optional<std::int32_t> generation(std::uint32_t actor)=0;
    virtual bool has_transform_component(std::uint32_t actor)=0; // +50 nonnull
    // Virtual +88. Nullopt models a null return pointer.
    virtual std::optional<WaterActorBounds> bounds(std::uint32_t actor)=0;
    virtual std::uint8_t byte_49(std::uint32_t actor)=0;
    // +50 component: virtual +58 refresh only when its +50 dirty byte is set,
    // then world translation at component +40.
    virtual RootVector refreshed_translation(std::uint32_t actor)=0;
    // Fresh read of component +40 without the dirty refresh (002b3960).
    virtual RootVector current_translation(std::uint32_t actor)=0;
};
// Resource words read by record constructor 002b2a40 from the runtime effect
// table (entries 0x28 and 0x27) and the ripple resource's frame-count halfword.
struct WaterRecordResources {
    std::uint32_t splash_resource{},ripple_resource{};
    std::uint16_t ripple_frame_count{};
};
enum class WaterEffectRequestKind:std::uint8_t { start /*001ca0e0*/, stop /*001ca210*/ };
struct WaterEffectRequest {
    WaterEffectRequestKind kind{};
    std::size_t slot{};
    std::uint32_t actor{};
    RootVector position{}; // start: record +10 snapshot at send; stop: unused
};
class WaterEffectSink {
public:
    virtual ~WaterEffectSink()=default;
    virtual WaterRecordResources record_resources()=0;
    // Synchronous 003c9420 send to the world receiver 004ffb50. Delivery may
    // re-enter the manager; the caller rereads the slot afterwards.
    virtual void request(const WaterEffectRequest&)=0;
    // 00232120 effect id 0x11/0x12 with identity rotation at the given point.
    virtual void splash(std::uint8_t effect_id,const RootVector& position)=0;
};

struct WaterRippleNode { // 0x60-byte payload at node +10
    RootVector position{};              // +10: X/Y/W from actor, Z = water height
    std::array<RootVector,4> corners{}; // +20..+50: rotated, XYZ-normalized
    float lifetime{};                   // +70
    float sinking{};                    // +74: 1.0 when speed <= -dz
    std::int32_t frame{};               // +78
    std::uint32_t variant{};            // +7c: one of the original {0,2,3}
    bool operator==(const WaterRippleNode&) const=default;
};
struct WaterEffectRecord { // 160-byte record
    std::uint32_t owner{};          // +0
    std::int32_t generation{-2};    // +4
    float radius{};                 // +8
    RootVector position{};          // +10
    RootVector displacement{};      // +20 (XY only, ZW cleared)
    float height{};                 // +30
    // +40 splash block (002b4710).
    float splash_timer{};           // +40
    RootVector splash_position{};   // +50
    float splash_inner{},splash_outer{}; // +60/+64
    std::uint32_t splash_resource{};     // +68
    // +70 ripple emitter (002b3960/3ed0). Constructor seeds the spawn
    // accumulator with the interval, so the first submerged update spawns.
    float ripple_accumulator{0.0375F};   // +70
    std::list<WaterRippleNode> ripples;  // +74 count, +78 sentinel; front=newest
    float ripple_inner{},ripple_outer{},ripple_height{}; // +80/+84/+88
    std::uint32_t ripple_resource{};     // +8c
    std::uint16_t ripple_frame_count{};  // +90
};

// Manager rendering data built by 002b3070/002b2d30. Construction consumes 48
// game-RNG values in original order; keep it for RNG-stream fidelity.
struct WaterSurfaceGrid {
    std::array<RootVector,16> positions{}; // +000
    std::array<RootVector,16> texcoords{}; // +100
    std::array<RootVector,16> colors{};    // +200
    std::array<float,64> sine{};           // +300: host sin approximation
};

class WaterEffectManager {
public:
    static constexpr std::size_t slot_count=20;
    static constexpr std::size_t ripple_capacity=32;
    // 002b3070. Advances the shared RNG state 48 times.
    WaterEffectManager(std::uint64_t& random_state);
    // 002b24c0: ensure-present attempt for an actor at a qualifying height.
    void ensure(float height,std::uint32_t actor,WaterEffectActorHost&,WaterEffectSink&);
    // 002b2210: height refresh of an existing valid record only.
    void update_height(float height,std::uint32_t actor,WaterEffectActorHost&);
    // 002b2330: first valid owner match; request precedes record mutation.
    void remove(std::uint32_t actor,WaterEffectActorHost&,WaterEffectSink&);
    // 002b2b70: ownerless point splash (no capacity checks, no request).
    void add_point_splash(float radius,float height,const RootVector& position,WaterEffectSink&);
    // 002b1d20 per-frame update from world+e4 inside 00133420.
    void update(float elapsed,WaterEffectActorHost&);
    // 002b2f80 (destructor body): frees all records; +454 is left unchanged.
    void clear();
    // 002b2280.
    int lookup(std::uint32_t actor,WaterEffectActorHost&) const;

    const WaterEffectRecord* slot(std::size_t index) const;
    std::int32_t allocated() const{return allocated_;} // +450
    std::int32_t active() const{return active_;}       // +454
    const WaterSurfaceGrid& surface() const{return surface_;}
private:
    std::uint32_t resolve(const WaterEffectRecord&,WaterEffectActorHost&) const;
    WaterEffectRecord& allocate(std::size_t index,WaterEffectSink&);
    void advance_ripples(float elapsed,WaterEffectRecord&,std::uint32_t owner,
        WaterEffectActorHost&,const RootVector& motion);
    std::uint64_t* random_;
    std::array<std::unique_ptr<WaterEffectRecord>,slot_count> slots_;
    std::int32_t allocated_{},active_{};
    WaterSurfaceGrid surface_;
};
// 002b3ed0: age every ripple (newest first) and erase expired nodes.
void age_water_ripples(float elapsed,WaterEffectRecord&);
}
