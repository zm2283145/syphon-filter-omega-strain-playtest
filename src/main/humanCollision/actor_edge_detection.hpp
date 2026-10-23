#pragma once
#include "actor_edge_types.hpp"
#include "collision_collection.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace omega {
// Raw COL5 bytes for a geometry handle and the current owner matrix for an
// object handle (the matrix 1831b0 returns; nullopt = no transform component).
using ActorEdgeGeometry=std::function<std::span<const std::uint8_t>(std::uint64_t)>;
using ActorEdgeOwner=std::function<std::optional<CollisionMatrix>(std::uint64_t)>;

// 003c0790/003c0850 for the gathered collection, in batch order; per triangle
// edges 2, 1, 0 with (flag & 0x7f) != 0, the 001efb30 filter (stored normal Z
// not below 0.5) and the 003c0e00 duplicate test (same owner and the same
// unordered raw vertex-index pair). Only COL version 5 is supported.
std::vector<ActorEdge> gather_actor_edges(const CollisionCandidateBatch&,const ActorEdgeGeometry&,const ActorEdgeOwner&);

// 003c1540(member, candidate, f12 cosine, f13 squared distance, f14 overlap):
// same flag ignoring 0x80, |cos| not below the limit, both candidate ends
// within the squared distance of the member's line, and one candidate end
// projecting onto the member within [-overlap, length + overlap].
bool actor_edges_connected(const ActorEdge& member,const ActorEdge& candidate,
    float cosine,float distance_squared,float overlap);

// Grouping loop of 001dfb80 (1dfd10..1dfe74) with 004a0360/68/70. Each group
// lists edge indices; a merge appends the merged group and pushes the edge
// again, so an index can appear twice. Emptied groups are then removed in
// reverse order by moving the last group into their place.
using ActorEdgeGroups=std::vector<std::vector<std::size_t>>;
ActorEdgeGroups group_actor_edges(std::span<const ActorEdge>);

// 001ed910: normalize(direction x (0,0,1)), negated when it points towards the
// third vertex (dot > 0). W is zero.
RootVector actor_edge_outward_normal(const ActorEdge&);

// 001edd10 without its per-candidate cache: seven segment probes around the
// adjusted point tested in triangle order against every non-water (0x80)
// candidate with 003bce50 -> 003c3c40. Two-sided normals flip in place.
std::uint32_t probe_actor_ledge(const RootVector& adjusted,const RootVector& outward,
    const RootVector& direction,std::span<FaceCandidate> triangles);
// 001eda40 (table 004a0380): 0x200 without bit 0x10, 0x100 without 0x04,
// 0x004 without 0x02.
std::uint32_t actor_ledge_probe_mask(std::uint32_t probe_result);

// Preset record (preset manager 004f5358 +0x10 + i*0xf0) written by 001ea4f0.
struct ActorEdgePreset {
    std::int8_t selector{};                       // +00
    std::uint32_t required{},excluded{};          // +04/+08 candidate mask bits
    std::uint32_t probe_required{},probe_excluded{}; // +0c/+10
    float height_min{},height_max{};              // +14/+18
    float distance_min{},distance_max{};          // +1c/+20 signed squared distance
    float cosine_min{},cosine_max{};              // +24/+28
    bool polarity{};                              // +2c
    bool allow_unsupported{};                     // +30 (accepted while physical +dc)
};
const std::array<ActorEdgePreset,9>& actor_edge_presets();

// Per-frame record state (+31.. of the preset record).
struct ActorEdgeRecord {
    bool requested{};     // +31 copied from slot request byte
    bool accepted{};      // +32
    ActorEdge edge;       // +40..+9f
    std::uint32_t mask{}; // +a0
    RootVector outward{},projected{},adjusted{}; // +b0/+c0/+d0
    float height{};       // +e0 (frame start: -FLT_MAX)
    float cosine{};       // +e4
};
// 1dfb80 stack cache sp+330 (two 001edd10 caches: polarity 0 at +0, 1 at +8).
struct ActorLedgeProbeCache { std::array<std::optional<std::uint32_t>,2> results; };
struct ActorEdgeScoreInput {
    std::uint32_t mask{};           // a2 (per-record copy)
    RootVector away{};              // t0: -(R.x, R.y, 0, R.w)
    RootVector projected{};         // t1
    RootVector adjusted{};          // t2
    RootVector forward{};           // t3
    float height{};                 // f12
    bool unsupported_dc{};          // stack +0 (physical +dc)
    bool ordinary{};                // stack +8 (flag bit 0 clear)
};
// 001ed250: one preset record scoring one candidate.
void score_actor_edge(const ActorEdgePreset&,ActorEdgeRecord&,ActorLedgeProbeCache&,
    const ActorEdge&,const ActorEdgeScoreInput&,std::span<FaceCandidate> triangles);

// 001ec850: publish an accepted record into its slot (side bits for 0x8000,
// neighbour bits 0x400/0x800 within 1.74 (004a0320) along the edge's group when
// mask bit 0x100 is set, grab orientation and point).
void publish_actor_edge(ActorEdgeSlotState&,const ActorEdgeRecord&,const ActorEdgeGroups&,std::span<const ActorEdge>);

struct ActorEdgeMessage {
    enum class Kind { cas_event, cas_action, found_ledge } kind{};
    std::uint32_t code{};    // CAS_EVENT 8 / CAS_ACTION 0x2a / 0
    std::uint32_t action_kind{}; // 14 for CAS_ACTION (00195c80)
    std::size_t slot{};
    std::uint32_t mask{};    // action mask or ORed passive masks
    std::uint64_t object{};  // CAS_ACTION object (owner reference object)
    RootVector orientation{},point{}; // slot +80/+90 handed to 001959c0
};
// 001e1600 decision (no sending): messages in send order.
std::vector<ActorEdgeMessage> select_actor_edge_request(const std::array<ActorEdgeSlotState,9>&);

struct ActorEdgeDetectionInput {
    std::int32_t mode_20{};
    std::uint32_t bits_24{};
    bool unsupported_dc{};   // physical +dc
    bool byte_de{};          // physical +de
    float value_394{};
    RootVector velocity_310{};
    RootVector floor_root{}; // 'flor' root world position (sp+470)
    // sp+4a0: model P+8 -> component +28 (refreshed through virtual +58 when
    // +50 is set), the row at +20 of its world matrix at +10, i.e. the image
    // of the local Y axis with W 0 (1e0018..1e0060).
    RootVector forward{};
    // Ladder path (+24 bit 0x200, setup 1e00d0..1e02b8) and mode-9 tail
    // (001e19f0/001e1a60): the 'edge' (004a03d0) and 'ladd' (004a03d8) root
    // weights (+d0) and resolved world placements (00132400), and the
    // retained physical +994 before this run.
    float edge_weight{},ladd_weight{};
    RootParentTransform edge_world{},ladd_world{};
    float band3_994{};
};
// 1e00d0..1e02b8: the ladder reference frame. origin = (E + L*f) - E*f of the
// resolved root positions with W = 1; axis = row 1 of 00132380 for the blended
// quaternion normalized over four lanes with VU rsqrt.
struct ActorLadderFrame { RootVector origin{},axis{}; };
ActorLadderFrame actor_ladder_frame(float edge_weight,float ladd_weight,const RootParentTransform& edge,const RootParentTransform& ladd);
struct ActorEdgeDetectionResult {
    std::vector<ActorEdge> edges;
    ActorEdgeGroups groups;
    std::array<ActorEdgeRecord,9> records;
    std::array<ActorEdgeSlotState,9> slots;
    ActorLadderContactFields ladder;
    std::optional<RootVector> edge_direction_a10; // +a10 when slot 6 published
};
// 001dfb80 detection body 1dfcb8..1e0ddc. Slots are taken as the entry left
// them (+3e2/+450 already cleared). With +24 bit 0x200 the reference is the
// blended edge/ladd root origin and every flag-bit-0 edge in the ladder plane
// writes the contact fields (1e0718..1e0a44) instead of being scored; mode 9
// then runs the tail 001e19f0/001e1a60 after publication (LADDERS.md). Two-sided
// normal flips made by the probes are committed to the gathered batch only on
// success.
ActorEdgeDetectionResult detect_actor_edges(const ActorEdgeDetectionInput&,
    const std::array<ActorEdgeSlotState,9>& slots,CollisionCandidateBatch& gathered,
    const ActorEdgeGeometry&,const ActorEdgeOwner&);
}
