#pragma once
#include "morph_host.hpp"
#include "morph.hpp"
#include "animation_root.hpp"
#include <span>
#include <string>
#include <string_view>

namespace omega {
// One source gender's two hand-name maps. Values are extracted from the ISO.
struct MorphGripMap {
    std::array<std::uint16_t,2> shape_counts{};
    std::array<std::array<std::uint8_t,24>,2> shapes{};
};
struct MorphEquipmentGrip {
    std::uint8_t grip{}, other_hand_grip{}; // Original unsigned fields +40d/+40e.
};
using MorphEquipment = std::array<std::optional<MorphEquipmentGrip>,2>;
MorphGripMap read_morph_grip_map(std::istream& input);
// 003ffa70(table, name): the first entry of a shape-name table (16-byte entries, MORPH.HOG
// FEMALE.BIN / MALE.BIN / ?HAND?.BIN) that strcmp-matches the name, else 0. An entry without
// a NUL inside its 16 bytes throws (strcmp would run into the next entry).
std::uint8_t morph_shape_index(const ShapeNames& names,std::string_view name);
// 00404060 (004043a8..004044c8): one table's two hand maps. Hand h's count is its
// "<g>HAND<h'>.BIN" entry count (+571500 +2g+h, one per 16-byte key) and byte k is
// 003ffa70(hand h's names, grip key k) for the 24 keys at 00493af0.
MorphGripMap morph_grip_map(std::span<const std::string> keys,const std::array<const ShapeNames*,2>& hands);
// The ELF's curve arrays (00493360 grip, 00493380 active, 004933a0 periodic): 8-byte keys,
// a signed frame word and a float.
MorphCurve decode_morph_curve(std::span<const std::uint8_t> bytes);
void initialize_equipment_grips(MorphHostState& state,const MorphGripMap& map,const MorphEquipment& equipment);
std::array<std::optional<std::uint8_t>,2> equipment_secondary_grips(const MorphHostState& state,
    const MorphGripMap& map,const MorphEquipment& equipment);
// Original 0019d2c0/model-published quaternion basis; no normalization.
SkinMatrix root_placement_matrix(const RootParentTransform& placement);
// Inputs are already-published original transform matrices. Optional node14
// means the actor model-present flag is set. Live target ownership/generation
// and virtual target getter remain the caller's responsibility.
std::optional<std::array<float,3>> resolve_morph_gaze(const SkinMatrix& root_world,
    const std::optional<SkinMatrix>& node14_model,const std::optional<RootVector>& live_target,
    const RootVector& fallback);
// 00132910(context, node) with context byte +14 set (a node collection
// exists): the node's skeleton-space translation (node +40, column 3 of its
// matrix at +10, W included) through the published component matrix (context
// +28, matrix at +10) as the ordered four-column product (vmulax, vmadday,
// vmaddaz, vmaddw). Neither matrix is validated as affine; the original reads
// them as stored.
RootVector skeleton_node_world_position(const SkinMatrix& component,const SkinMatrix& node);
// 001e5880(out, context): the model context's vector +50 (the centroid
// 003a9090 stores, all four lanes) through the component matrix with the same
// ordered four-column product (vmulax, vmadday, vmaddaz, vmaddw). With the
// vector (0,0,0,1) the result is the component's translation column.
RootVector component_placed_vector(const SkinMatrix& component,const RootVector& local);
// The same over a model's published placement (003ab510 publishes the pose
// aggregate into the component as root_placement_matrix) and its
// skeleton-space node matrices in file order. Rejects a node outside them.
RootVector published_skeleton_node_position(const RootParentTransform& published,
    std::span<const SkinMatrix> nodes,int node);
// 00133ed0(out, context, node) with a node collection: every row of the
// node's skeleton-space matrix (node +10..+4f) through the published component
// matrix with the same four-column product as 00132910 (row 3 equals
// skeleton_node_world_position). Rows are returned as stored (x, y, z, t).
std::array<RootVector,4> skeleton_node_world_matrix(const SkinMatrix& component,const SkinMatrix& node);
std::array<RootVector,4> published_skeleton_node_matrix(const RootParentTransform& published,
    std::span<const SkinMatrix> nodes,int node);
}
