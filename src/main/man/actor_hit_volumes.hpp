#pragma once
// Actor hit volumes (research/COMBAT.md, "C1d: hit volumes"): the model query a
// bullet makes against a character, 00188140 -> 003a9d40 / 0038b990 / 0038c050
// (with 0038bd30 and 0038ba80). The volumes are not spheres or boxes: the query
// skins the low-detail part meshes (render +20c0 records, the SKM chunks of
// skinned_mesh.hpp) into a frame built from the ray with the VU0 position-query
// program (microcode set 1, `004937c0`[1]) and keeps the vertices whose distance
// from the ray axis is below the record radius (+4c). Interpreter-verified with a
// VIF0/VU0 micro-mode model of that program on synthetic skeletons and meshes.
//
// Matrices are `ShotFrame`s in game memory order: row k = qword k (row 3 is the
// translation); a product A x B is row_i(A) through B, rounded per VU lane
// ((b0*a.x + b1*a.y) + b2*a.z) + b3*a.w.
#include "bullet_hit.hpp"
#include "skinned_mesh.hpp"
#include "weapon_discharge.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace omega {

// One part record pair of the model (render +1ec0 / +20c0 + 16 i, 0038c920).
struct HitVolumePart {
    const SkinnedMesh* lod0{};      // +1ec0 (null: no mesh)
    const SkinnedMesh* lod1{};      // +20c0 (null: none, e.g. the MOUT_ pair 0038c920 clears)
    std::uint8_t flags0{};          // +1ec6: bit 0x80 disables the gore pass; & 0x1c gore-pass code
    std::uint8_t flags1{};          // +20c6: & 0x1c is the part code (0x1c: never tested)
};
// 003ffb20 (tail of the mesh loader 003fffc0): the bits ORed into the part-mesh
// cache record byte +6 (005639b6 + 32 index + 16 LOD, copied to +1ec6 / +20c6)
// from the 16-byte cache name (005609b0 + 16 index), compared from byte 1 with
// strncmp: & 0x1c the part code, 0x80 the gore-disable bit. The loader stores 0
// in +6 first (003fffc0 t1 = 0), so a loaded record's byte is exactly this value.
std::uint8_t part_mesh_hit_flags(const std::array<char,16>& cache_name);
std::uint8_t part_mesh_hit_flags(std::string_view cache_name);   // zero-padded / cut to 16 bytes
// Model gore state: render +2340 (one array per part, 32 bytes per LOD0 chunk,
// allocated by the query) and +2c08 (nonzero once the gore pass ran).
struct HitVolumeGore {
    std::vector<std::optional<std::vector<std::uint8_t>>> parts;
    std::uint32_t level_2c08{};
};
// The 0x50-byte query record. Fields the query never wrote stay unset; reading
// one (as the original would read stale stack) throws.
struct HitVolumeRecord {
    std::optional<RootVector> point;            // +00: VU0 qword 5 + 2 vertex after the LOD0 chunk ran (0038ba80)
    float near_score{}, near_y{};               // +20 / +24 (0038c050 sets +20 = 1e7)
    std::optional<std::int32_t> code;           // +28
    std::uint16_t vertex{}, chunk{}, part{};    // +2c / +2e / +30
    float far_score{}, far_y{};                 // +34 / +38 (+34 = -1e7)
    std::optional<std::int32_t> far_code;       // +3c
    std::array<std::int8_t,8> groups{};         // +40..+47: group bones, negated outside the hit vertex
    std::optional<std::int8_t> group_count;     // +48
    float radius{};                             // +4c (input)
};

// Node matrices as the native skeleton returns them (skeleton_node_matrices():
// 16 floats in node +10..+4f memory order) as ShotFrame rows.
std::vector<ShotFrame> hit_shot_frames(std::span<const std::array<float,16>> matrices);
// 00188140 head: the ray frame (rows: side, normalized direction, third axis,
// origin with w = 1) and its rigid inverse (00133200).
struct HitRayFrame { ShotFrame frame{}, inverse{}; RootVector direction{}; };
HitRayFrame hit_ray_frame(const RootVector& origin,const RootVector& direction);
// 003a9d40 into scratchpad, as 0038c050 loads the first 21 into VU0 qword 172:
// node k = (inverse_bind[k] x node[k]) x (component x frame inverse). Needs at
// least 21 nodes (0038c050 copies 21 matrices).
std::vector<ShotFrame> hit_node_matrices(std::span<const ShotFrame> nodes,std::span<const ShotFrame> inverse_bind,
    const ShotFrame& component,const ShotFrame& frame_inverse);
// The VU0 position-query program on one chunk: the positions it leaves at VU0
// qword 5 + 2 i for i < vertices. One group: group.Y vertices transformed by its
// bone (slots past Y keep the unpacked second attribute vector, 32896 + value).
// Several groups: the first group weighted, the rest of the chunk zeroed with the
// sign of the first bone's translation row, then each later group's weighted
// position added to its range.
std::vector<RootVector> hit_query_positions(const SkinChunk& chunk,std::span<const ShotFrame> matrices);
// 0038b990: when +2c08 is set, every nonzero gore byte of the LOD0 chunks grows
// by +2c08 (at most 90), then +2c08 = 1.
void age_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore);
// 0038b8d0: every nonzero gore byte of the LOD0 chunks becomes 0x5a (the
// permanent stain).
void saturate_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore);
// 0038a118 (in the render update 00389fa0, after the actor's logic): with
// +2c08 > 0, draw mode +2bda 0 saturates and stores +2c08 = -1; otherwise, on
// an even frame (005383c8 & 1 == 0), +2c08 += 1 and reaching 0x5a stores -1 and
// saturates. Returns true when it saturated.
bool update_hit_gore(std::span<const HitVolumePart> parts,HitVolumeGore& gore,std::int8_t draw_mode_2bda,std::uint32_t frame);
// The upload 00386ad0's qword 123.y for a model with a gore array: max(+2c08, 0)
// (cvt.s.w then max with 0.0; the value is integral).
std::int32_t character_gore_level(std::uint32_t level_2c08);
// The character program's gore loop (VU1 0x3058, after the lit loop when 122.z
// is negative): a vertex whose gore byte has (b & 0x7f) != 0 loses, from its lit
// colour (0..255, 0x80 = 1.0, alpha untouched), with a = level + b (the whole
// byte) and m = min(a, 90): green and blue m, red ceil(floor(min(a / 8, 11.25)
// * 2) / 2) = (floor(m / 4) + 1) / 2; each channel stays >= 0. Both subtracts
// are 0 for any other byte.
struct CharacterGoreSubtract { std::uint8_t red{}, green_blue{}; };
CharacterGoreSubtract character_gore_subtract(std::uint8_t byte,std::int32_t level);
// The subtracts of a part's LOD0 draw per skinned vertex (skin_skinned_mesh's
// order: chunk c's written vertices in turn, vertex k taking gore byte
// 32 c + k). Throws when `vertex_count` is not the chunks' total, when the array
// is shorter than its chunks, and (naming 0x3058) when a flare chunk (programs
// 12 / 14, whose path does not reach the lit loop's gore branch 0x2258) carries
// a gore byte.
std::vector<CharacterGoreSubtract> character_gore_vertices(const SkinnedMesh& lod0,std::span<const std::uint8_t> bytes,
    std::int32_t level,std::size_t vertex_count);
// 0038c050 (with the gore pass 0038bd30 and the hit point 0038ba80): returns
// record +28 when a vertex lay inside the radius, else 0.
std::int32_t query_hit_volumes(std::span<const HitVolumePart> parts,HitVolumeGore& gore,
    std::span<const ShotFrame> matrices,std::int32_t mode,HitVolumeRecord& record);

// An impact effect request 00232180(word(004933d0 + 4 * table_index), position, direction).
struct HitVolumeEffect {
    std::uint32_t table_index{};
    RootVector position{}, direction{};
    bool operator==(const HitVolumeEffect&) const=default;
};
struct HitVolumeActor {
    std::span<const HitVolumePart> parts;        // render +23c0 records
    HitVolumeGore* gore{};                       // render +2340 / +2c08
    std::span<const ShotFrame> nodes;            // skeleton context +2e50 nodes, +10 (skeleton space)
    std::span<const ShotFrame> inverse_bind;     // the same nodes, +d0
    ShotFrame component{};                       // model component +2e78, +10
    std::uint8_t life_state{};                   // virtual +108
    std::uint8_t* gore_2df2{};                   // actor +2df2 (= render +2d92)
};
// 00188140: flags 0x100 hit, | 0x80 code 0xc, | 0x40 code 0x10 or 4, | 0x20 code
// 0x14; 0 when the query returns code 0. Mode > 0 requests the entry effect
// (table index 1, 2 for code 0xc, 3 for 0x10; at the frame point (0, +24, 0, 1),
// direction negated); mode 2, or both codes below 0xc, adds the exit effect
// (index 0, at (0, +38, 0, 1)) and sets +2df2 for an actor not in life state 2/3.
std::uint32_t actor_hit_volumes(const HitVolumeActor& actor,const RootVector& origin,const RootVector& direction,
    std::int32_t mode,HitVolumeRecord& record,std::vector<HitVolumeEffect>& effects);
// What 00188780 reads back (BulletHitHost::hit_volumes): the flags and, on a hit,
// record +24 and the group bones +40.. (count +48), for a fresh record whose
// radius (+4c) is the request's +28.
BulletHitVolumes bullet_hit_volumes(const HitVolumeActor& actor,const RootVector& origin,const RootVector& direction,
    std::int32_t mode,float radius,std::vector<HitVolumeEffect>& effects);

}
