#pragma once
// Edge candidate and physical edge-slot layouts shared by the 001dfb80 entry
// (actor_edge_presets) and its detection body (actor_edge_detection).
#include "animation_root.hpp"
#include <array>
#include <cstdint>

namespace omega {
// Candidate +50..+5b: triangle index (gathered record +60), edge index and the
// owner reference (gathered record +64). The owner reference is represented by
// the source geometry/object pair.
struct ActorEdgeIdentity {
    std::uint32_t triangle{};
    std::uint32_t edge{};
    std::uint64_t geometry{};
    std::uint64_t object{};
    bool operator==(const ActorEdgeIdentity&) const=default;
};
// One 0x60-byte candidate appended by 003c0850 to the vector at 004f5360.
struct ActorEdge {
    RootVector p0{},p1{},third{};     // +00/+10/+20, owner-transformed
    std::uint8_t flags{};             // +30: raw COL5 triangle byte +0x0b+edge
    float length{};                   // +34 (001d10c0)
    RootVector direction{};           // +40: (p1-p0) xyz / length, W kept
    ActorEdgeIdentity identity;       // +50
    std::array<std::uint16_t,2> vertex_indices{}; // raw COL indices of p0/p1 (003c0e00)
    bool operator==(const ActorEdge&) const=default;
};
// Physical slot P+3e0+i*0xa0. Requests latch +00/+01 (001adb30/0024fbc0),
// the 001dfb80 entry clears +02/+70, 001ec850 publishes the rest.
struct ActorEdgeSlotState {
    bool request{};       // +00
    bool query_only{};    // +01 passive request (ANDed by repeated latches)
    bool result{};        // +02
    ActorEdge edge;       // +10..+6f
    std::uint32_t mask{}; // +70
    float cosine{};       // +74
    RootVector orientation{}; // +80 quaternion (VU block 1ecee0, same code as 0027d808)
    RootVector point{};       // +90
    bool operator==(const ActorEdgeSlotState&) const=default;
};
// Ladder contact fields written by the detection body (+990..+9a5). Every
// body run resets +990/+998 and the ten bytes; +994 (z of the last band-3
// hit) is retained across runs. The constructor (001ebfe4..1ec04c) zeroes
// +990/+994/+998 and copies the zero byte 0048a990 into the ten bytes.
struct ActorLadderContactFields {
    float top_990{},bottom_998{};
    float band3_994{};
    std::array<std::uint8_t,10> flags_99c{}; // +99c..+9a5
    bool operator==(const ActorLadderContactFields&) const=default;
};
// 001adb30(001adb70(P+0x3e0, slot), passive) and the identical input helper
// 0024fbc0(P, slot, passive): the first request stores the passive flag, a
// repeated request ANDs it (an active request wins), then +00 is set.
void latch_actor_edge_request(ActorEdgeSlotState& slot,bool passive);
}
