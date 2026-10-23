#pragma once
#include "animation_root.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace omega {
using LosObjectId = std::uint64_t;
struct LosRegionBounds {
    RootVector minimum{}, maximum{};
};
// An immutable observation of original object metadata. Bounds have already
// been obtained from virtual +0x88; this kernel does not emulate that getter.
struct LosSpatialObject {
    LosObjectId id{};
    std::uint32_t generation{};
    std::uint8_t type{};
    std::uint32_t descriptor{};
    std::optional<LosRegionBounds> bounds;
    std::vector<LosObjectId> members; // Original +0x1f4 array order; zero is null.
};
struct LosSpatialSnapshot {
    std::span<const LosSpatialObject> objects;
    std::span<const LosObjectId> global_regions; // Original registry array order.
    std::uint32_t region_descriptor{}; // Caller-resolved runtime 0x004ee780.
};
struct LosProviderState {
    RootVector position{};
    LosObjectId region{};
    std::uint32_t generation{0xffffffffu};
    bool operator==(const LosProviderState&) const = default;
};
enum class LosSelectionPath { retained, local, global, empty };
struct LosCollectionSelection {
    LosSelectionPath path{LosSelectionPath::empty};
    LosObjectId region{};
    // Reverse nonnull collection order consumed by 003c4f00. Duplicates remain.
    // Exclusions must NOT remove entries here: child geometry is independent.
    std::vector<LosObjectId> object_order;
};
// Original 002245f0 + 00224ac0 + 002248a0 selection, with authored stable IDs.
// The snapshot is fixed for this call. There is no virtual-getter reentrancy,
// collision gather, geometry filtering, object lifetime manager or world scan.
// Failure leaves state unchanged; successful no-region lookup clears its cache.
LosCollectionSelection select_los_collection(LosProviderState&, const RootVector& origin,
                                              const LosSpatialSnapshot&);
}
