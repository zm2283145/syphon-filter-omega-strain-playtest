#pragma once
#include "animation_root.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>

namespace omega {
using GatherIdentity=std::uint64_t;
using GatherMatrix=std::array<float,16>;
struct GatherBounds { RootVector minimum{},maximum{}; };
struct GatherSphere { float radius{};RootVector center{}; };
// Original begin/end are captured together, but each element is read later.
// The host must keep range storage valid; edits of existing elements are live.
struct GatherRange { GatherIdentity identity{};std::size_t count{}; };
enum class GatherRoute { direct_col, node_col };
struct GatherEmission {
    GatherIdentity object{},geometry{};
    GatherRoute route{};
    GatherBounds local_bounds;
    // No frozen owner: original 00183780 rereads object+0x50 per triangle.
};
struct LiveGatherHost {
    std::function<void()> clear_output,update_provider,finalize_output;
    std::function<GatherRange()> provider_collection;
    std::function<GatherIdentity(GatherRange,std::size_t)> member_at;
    std::function<GatherIdentity(GatherIdentity)> geometry; // virtual +0xb8; zero absent
    std::function<GatherSphere(GatherIdentity)> geometry_sphere;
    std::function<GatherIdentity(GatherIdentity)> owner; // object +0x50, zero absent
    std::function<GatherMatrix(GatherIdentity)> owner_world; // refresh dirty model then read
    std::function<GatherMatrix(GatherIdentity)> owner_inverse; // original 00133190 getter
    // Return object+0x54 only when nonnull and its byte zero is nonzero.
    std::function<GatherIdentity(GatherIdentity)> enabled_history;
    std::function<GatherMatrix(GatherIdentity)> history_world; // history +0x10; pure read
    std::function<bool(GatherIdentity)> eligible; // +2d && +2c && !+2e; pure fresh read
    std::function<bool(GatherIdentity)> excluded; // live provider exclusion list
    std::function<GatherRange(GatherIdentity)> children; // virtual +0x80; zero range absent
    std::function<void(const GatherEmission&)> emit;
};
struct LiveGatherStats { std::size_t objects{},sphere_rejected{},ineligible{},excluded{},emissions{}; };
// 00181270 and recursive 001815e0 traversal, culls and local query transforms.
// Required host callbacks perform actual lookup/geometry emission. The function
// does not fabricate collections, collision data or virtual-getter behavior.
// Callback effects are synchronous and are NOT rolled back on a later failure.
LiveGatherStats gather_live_collision(const GatherBounds&,const LiveGatherHost&);
}
