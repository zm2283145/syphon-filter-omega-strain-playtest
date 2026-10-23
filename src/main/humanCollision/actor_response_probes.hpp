#pragma once
#include "collision_collection.hpp"
#include <bit>
#include <functional>
#include <optional>

namespace omega {
// Global 004f5430 world acceleration (feet/s^2). Static initializer 004ccb00
// stores (0, 0, -bits(0x4200bd7b) / 1.0, 0). 0025ad80 temporarily adds
// DAT_0048af98 to Z while integrating type-7 objects and restores it, so the
// value seen by actor physics is this constant unless a host models that window.
inline const RootVector game_initial_gravity{0,0,-std::bit_cast<float>(0x4200bd7bU),0};

// 001e3cd4..3d24 inside 001e2180: when the primary root weight (+d0) is
// positive, force (physical +330) += gravity * mass (physical +2e0), all four
// lanes (0017f670 then 001e5610). Weight equality with zero skips.
RootVector apply_actor_gravity(const RootVector& force,const RootVector& gravity,
                               float mass,float primary_weight);

// 001e5f30: water-surface probe. Query config 003c0220(0.1, 0.05, 0.05, 0, 0.1)
// with REQUIRED surface flag 0x80 and no exclusion (3c3450(q, 0x80, 0)), so it
// only meets water surfaces; the solid body query uses the opposite masks.
struct ActorResponseProbeHost {
    // Null pointer is the original null collection (no hit); a present batch
    // may be empty. Two-sided candidate normals mutate in place.
    std::function<CollisionCandidateBatch*()> candidates;
    std::function<std::span<const std::uint8_t>(std::uint64_t)> geometry;
    std::function<std::optional<CollisionMatrix>(std::uint64_t)> owner;
};
struct ActorWaterProbeHit {
    // Copied to physical +170/+180/+190 (vertices), +1a0 (normal), +1b0
    // (surface), +1c0..+1c2 (edge flags), +1d0 (source).
    ExpandedCollisionTriangle triangle;
    float height{}; // hit point Z -> physical +e4
};
// Start = upper_reference + (0,0,6,0); displacement = target - start. In
// 001e2180 these are its resolved positions sp+10c0 and sp+10e0 (target is the
// published root position; upper_reference shares its XY). No hit writes nothing.
std::optional<ActorWaterProbeHit> run_actor_water_probe(const RootVector& upper_reference,
    const RootVector& target,const ActorResponseProbeHost&);

// 003c36c0/003c3c40 nearest-hit segment test over candidates in order, with
// required/excluded surface masks (probe +60/+61). Two-sided (flag 2) triangles
// facing away have their candidate normal negated in place before the plane
// test, whether or not they are then hit. A hit shortens the displacement.
struct FloorRayHit {
    Vec3 normal;          // candidate normal after any two-sided flip
    RootVector point{};   // origin + displacement * s
    std::size_t ordinal{};// candidate index -> batch.sources
};
struct FloorRayResult {
    std::optional<FloorRayHit> hit;
    RootVector displacement{}; // final (possibly shortened) probe +40
};
FloorRayResult trace_floor_ray(const RootVector& origin,const RootVector& displacement,
    std::span<FaceCandidate> candidates,std::uint8_t required_flags,std::uint8_t excluded_flags);

// 001e3d74..401c: downward floor probe of (0,0,-15.5,0) from 001e2180's
// sp+10c0 (the water probe's unshifted reference). Shape byte +14 >= 3 uses the
// 0.1 sphere query (003c0220 + 3c3450(q, 0, 0x80)); otherwise the ray probe
// (001e5550 record, 003bcdd0 -> 003c36c0, merged by 003bf470). Both exclude
// water (flag 0x80). The result feeds the floor response at 1e4020.
struct ActorFloorProbeHit {
    Vec3 normal;          // public result +20
    Vec3 face_normal;     // result +30: equals +20 for face/ray hits, else the
                          // source face normal reconstructed inside 3c3450
    RootVector point{};   // public result +50
    CollisionTriangleSource source;
};
std::optional<ActorFloorProbeHit> run_actor_floor_probe(const RootVector& reference,
    std::uint8_t shape_byte_14,const ActorResponseProbeHost& collection);
// 1e43b8..4428: when shape byte +14 >= 3, +dc is set and no edge/ladder weight
// applies, the floor is re-probed into the same result with a 1.0 sphere
// (003c0220(1.0, 0.05, 0.05, 0, 1.0), 3c3450(q, 0, 0x80)) along (0,0,-15.5).
std::optional<ActorFloorProbeHit> run_actor_floor_reprobe(const RootVector& reference,
    const ActorResponseProbeHost& collection);
}
