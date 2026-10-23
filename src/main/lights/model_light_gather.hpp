#pragma once
// Model light gather of the light manager 00535d20 and the per-frame stealth
// pass (research notes FLASHLIGHT.md sections 6.1 and 16, LEVEL_LIGHTS.md section 11):
// - 00373320 (manager, mask, flags, transform): candidate lights from the
//   light table (003730f0 sphere test, qsort 001273c8 / 00373ba0), then
//   00372120 (up to four light slots: direction and attenuated colour; the
//   colour sum 00537d80), then 00371e70 (the slots in model space, x128);
// - 00133ab0: for every present actor of the active list 004ffd44, the
//   reference = actor virtual +e0, the gather with mask 0x17 and flags 3, and
//   actor +44 from the colour (actor_stealth_value, flashlight.hpp).
// Every float operation follows the original order and is bit-exact with the
// EE interpreter model (binary32, round to nearest, denormal results flushed
// to zero). Paths that are not hosted throw std::runtime_error: flags bit 4
// (00371cf0 and the +0x2130 light list), the bounds test 00373590 for a
// non-directional entry (it reads the VU status flag), values that are not
// finite, divisions by zero, and fields past a light pool's stride.
#include "character_shadows.hpp"
#include "level_lights.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omega {
// One candidate (scratchpad +0x2000 + 8*k: light, key).
struct ModelLightCandidate { LevelLightRef light; float key{}; };

// The manager fields the gather reads and writes. Defaults: the constructor
// 003747ac (+0x40..+0x43 = 1, +0x44 = 0; the rest zero in the boot image).
struct ModelLightGather {
    ShadowVector reference{};                 // +0x20 (00535d40): the point lights are measured from
    ShadowVector reference_30{};              // +0x30: second bounds corner (bounds setters only)
    bool sphere_40{true};                     // +0x40: 1 = sphere test 003730f0, 0 = bounds test 00373590
    bool reference_changed_41{true};          // +0x41: set by every reference setter
    bool dirty_42{true};                      // +0x42 (00535d62): set by light changes (enable, release, position)
    bool dynamic_colour_43{true};             // +0x43: 0 = the colour is the ambient pool sum (vision modes 1/2 clear it in their frame)
    std::uint32_t mask_44{0};                 // +0x44: mask of the last recompute
    std::vector<ModelLightCandidate> candidates;           // scratchpad +0x2000, count +0x50
    std::int32_t slot_count_48{0};                         // +0x48
    std::array<ShadowVector,4> slot_direction{},slot_colour{};   // scratchpad +0x2800 / +0x2840
    ShadowVector colour{};                                 // +0x2060 (00537d80)
    // 00371e70 (flags bit 2): +0x2070 the slot directions as gathered, +0x20b0
    // the slots in model space (inverse transform) stored transposed (qword k
    // lane j = slot j lane k), +0x20f0 the slot colours with xyz x128.
    std::array<ShadowVector,4> world_direction{},model_direction{},light_colour{};
    // Not a manager field: true when code that is not native (the renderer's
    // lit draws through 0037c350, light setters that raise +0x42) may have
    // used the manager since the last native call. A call that then takes the
    // cached path (no recompute) also recomputes on a copy and throws unless
    // both give the same results; the next call clears it.
    bool unhosted_queries{false};
};

// 00133b7c..00133bfc (and the same block in 003a1760): when a lane of the VU
// difference reference - p is not a zero result (MAC flags other than 0xf),
// reference = p, +0x40 = 1, +0x41 = 1. Throws for a lane whose difference is
// -0 or denormal (the interpreter flags them as non-zero; the hardware flag is
// not established) and for non-finite lanes.
void set_model_light_reference(ModelLightGather& m,const ShadowVector& p);

// 00373320. `table` is the table as the frame's rebuild left it (its entries
// keep the light fields copied then; rows[k].light names the live record in
// `lights`, which 00372120 reads). transform: the model transform 00371e70
// reads (nullptr: the slots are used as gathered). Throws as listed at the top.
void gather_model_lights(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
    std::uint32_t mask,std::uint32_t flags,const ShadowMatrix* transform);

// The particle emitter's light sample in 003a1760 (Checkpoint 131c): the
// reference block 003a17c8..003a183c (as 00133b7c), 00373320(manager, 0x16,
// 1, null) and the colour 00537d80 it leaves. The emitter's def+140 scale is
// the particle system's (particle_emitter.cpp).
ShadowVector particle_game_light(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
    const ShadowVector& point);

// One actor of 00133ab0.
struct StealthActor {
    bool present_2c{},present_2d{},hidden_2e{};   // gather only when +0x2d, +0x2c and not +0x2e
    ShadowVector position{};                      // virtual +e0 (00133e00: the body centre)
    ShadowMatrix transform;                       // +0x2e78 render object +0x10, after its refresh (byte +0x50: vcall +0x58)
    bool player{};                                // controller +0x58 present and its vcall +0x54
    bool flashlight_on{};                         // 00199740
    std::int32_t stealth_44{};                    // in: the previous value (003cf0e0 writes 0); out: the new one
};
struct StealthThresholds {
    std::int32_t darkness_decreaser{65};          // 00489bb0 (TORONTO1 init::StartLevel writes 82)
    std::int32_t npc_darkness_decreaser{90};      // 00489bb8
};
// 00133ab0 over the active list in list order. Actors that are not present
// keep their +44.
void run_stealth_pass(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
    std::vector<StealthActor>& actors,const StealthThresholds& thresholds);
// The body of 00133ab0 for one present actor: the reference, the gather
// (mask 0x17, flags 3, the actor's transform) and actor_stealth_value.
std::int32_t model_light_stealth(ModelLightGather& m,const LevelLights& lights,const LevelLightTable& table,
    const StealthActor& actor,const StealthThresholds& thresholds);

// Play host source of actor +44 (CombatPerceptionWorld::stealth_44, the
// player-targeting host). update() runs one pass at the end of a world frame
// (00133768 follows the actor traversal 0040ca30), over the table the last
// scene frame built (00373670 at 0013e820) and the live lights; it marks the
// manager as used by unhosted code first (the renderer's model lighting is
// not native). value() is the stored +44: 0 before the first pass
// (003cf0e0), so perception in frame n reads the pass of frame n - 1.
class StealthLighting {
public:
    explicit StealthLighting(std::size_t actors,StealthThresholds thresholds={});
    void update(const LevelLights& lights,const LevelLightTable& table,std::vector<StealthActor>& actors);
    std::int32_t value(std::size_t actor) const;
    // A run-time actor appended to the traversal (a generator spawn): its
    // +44 is the base constructor's 0 (001b7b00 -> 00393760 -> 003cf0e0 at
    // 003cf148) until the next pass.
    void append_actor();
    // ResetNpc's in-place rebuild (001b8130, the same base constructor):
    // the actor's +44 back to 0 until the next pass.
    void reset_actor(std::size_t actor);
    const ModelLightGather& gather() const { return gather_; }
    // The thresholds the next pass reads (00489bb0 / 00489bb8); a sample of
    // 003a1760 may create the object before the first pass.
    void set_thresholds(StealthThresholds thresholds) { thresholds_=thresholds; }
    // The shared manager 00535d20 for its other users (the particle light
    // sample of 003a1760).
    ModelLightGather& manager() { return gather_; }
private:
    ModelLightGather gather_;
    StealthThresholds thresholds_;
    std::vector<std::int32_t> values_;
};
}
