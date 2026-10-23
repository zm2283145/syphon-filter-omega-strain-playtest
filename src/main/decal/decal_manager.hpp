#pragma once
// The world's decal manager *004ffc3c (world +ec; research/LEVEL_OBJECTS.md
// "Checkpoint 132o2: bullet decals"): two rings of 0x60-byte records built by
// 0027a020(mgr, 100, 20). List 1 holds the bullet-impact decals 00279ca0 adds
// (00276460's static impact), list 2 the blood decals of 002797c0, which go
// through 002792f0 and the type-3 projector 003d4350 (research/LEVEL_OBJECTS.md
// "Checkpoint 132o3: blood decals"; the projector is character_shadows'
// add_shadow_projector).
//
// Header: +0 / +4 the list pointers, +8 / +c their sizes, +10 / +14 the
// counts, +18 / +1c the write indices, +20 / +24 the fade-ahead distance
// cvt.w.s(0.1 * size). A record: +0 flags (the mode byte | 1), +10..+4f the
// matrix (rows), +50 the half size, +54 the texture handle, +58 the object
// (its +50 transform node carries the decal), +5c the age (-1 alive, >= 0
// fading for 2 s, -2 dead).
//   00279ca0  add: the record at the old write index is rewritten (0027a570);
//             a full ring starts fading the record +20 ahead of it.
//   0027a570  record set: an object without a node (terrain: +50 is 0) is
//             dropped; without an object the matrix is the frame, otherwise
//             frame * rigid-inverse(node +10) (00133300 / 00133200, VU).
//   00279630  service (00133800, world dt): fading ages advance and die at 2;
//             a decal whose object is not present (+2d, +2c, !+2e) dies.
//   00278650  list-1 draw (scene frame 0013e954, after the water effects,
//             before the effect draw 003ec300): mode word 0x400005, one
//             00371200 flush per run of texture / ALPHA / object; quads of
//             (+-s, 0, +-s) through the matrix, under node * stack when the
//             record has an object, colour (128, 128, 128, 128 * alpha).
//   00278460  the 32 blood directions (mgr +30), built by the constructor.
//   002797c0  blood add: 32 LOS probes from the point along the directions
//             (length 2 * size); each hit object (class 1, or a loaded class-0xf
//             part) gets one list-2 record (0027a370) carrying the point and the
//             averaged hit normal in the object's node space.
//   002792f0  the scene step after 003d37e0: one type-3 projector request per
//             live list-2 record (alpha 1, or 1 - age / 2 while fading).
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

using DecalMatrix = std::array<std::array<float, 4>, 4>;  // rows, row vectors

struct DecalRecord {
    std::uint32_t flags_0 = 0;
    DecalMatrix matrix_10{};
    float size_50 = 0.0F;
    std::int32_t texture_54 = -1;
    std::uint64_t object_58 = 0;
    float age_5c = -1.0F;
};

// What 0027a570 / 00278650 read through the object: its +50 transform node
// +10..+4f after the node's vt+58 refresh (taken when the node byte +50 is
// set); nullopt when the object's +50 is 0.
using DecalNodeLookup = std::function<std::optional<DecalMatrix>(std::uint64_t object)>;
// The object's root bytes +2c, +2d, +2e (00279630's presence test).
using DecalRootLookup = std::function<std::array<std::uint8_t, 3>(std::uint64_t object)>;

struct DecalVertex {
    std::array<float, 4> position{};     // the corner through the record matrix (001325e0)
    std::array<std::int32_t, 4> rgba{};  // 0025c9d0: cvt.w.s(128 * colour)
    std::array<float, 4> st{};           // 00132000(u + 2048, v + 2048, 1, 0)
};
// One 00371200 flush: the texture bound by 003820e0, the ALPHA_1 word
// 005392d8 (004bc1b0[blend]), the object whose node the records draw under
// (0: the stack top as the draw found it) and its quads (4-vertex strips).
struct DecalFlush {
    std::int32_t texture = -1;
    std::uint64_t alpha = 0;
    std::uint64_t object = 0;
    std::optional<DecalMatrix> node;
    std::vector<std::array<DecalVertex, 4>> quads;
};

using DecalVector = std::array<float, 4>;
// What 002797c0's LOS request (vt+0c of 003caa50(004ffb50, 004ea0b0)) reports
// for one probe from `origin` (w = 1) along `displacement` (w = 0): the hit
// normal (result +50) and the owner **(result +84): its class byte +49, for
// class 0xf its +64 part and for class 0x10 its vt+124 part (an NIE: 0023c180,
// the asset +140 for kinds 1 / 3 / 6) (0: none), and whether that part's
// model word **(+68) is nonzero.
struct BloodTraceHit {
    DecalVector normal{};
    std::uint8_t owner_class = 0;
    std::uint64_t owner = 0;
    std::uint64_t part = 0;
    bool part_loaded = false;
};
using BloodTrace = std::function<std::optional<BloodTraceHit>(const DecalVector& origin, const DecalVector& displacement)>;
// One 003d4350 call of 002792f0: the record's object, the world position and
// normal (the record +40 / +20 through the object's node), f12 = size, f13 =
// 0.33 * alpha, t0 = texture.
struct BloodProjectorRequest {
    std::int32_t record = 0;
    std::uint64_t object = 0;
    DecalVector position{}, normal{};
    float size = 0.0F, strength = 0.0F;
    std::int32_t texture = -1;
};
// What 002792f0 reads through a record's object: +38 == 0x80 (culled this frame).
using BloodHiddenLookup = std::function<bool(std::uint64_t object)>;
inline constexpr std::size_t blood_direction_count = 32;  // 0027a33c: 00278460(mgr + 0x30, 32)
inline constexpr float blood_projector_strength = 0.33F;  // 0x3ea8f5c3

// 004bc1b0: the ALPHA_1 words by blend index (00278650 picks 3 for mode bit 8,
// 0 for 4, 2 for 0x10, otherwise 3).
inline constexpr std::array<std::uint64_t, 4> decal_alpha_words{0x0000008000000044ULL, 0x0000008000000068ULL,
                                                                0x0000008000000048ULL, 0x0000008000000042ULL};
// 00278650's draw mode word (the old 005392d0 is restored after the draw).
inline constexpr std::uint32_t decal_draw_mode = 0x400005U;

class DecalManager {
public:
    // 0027a020(mgr, list1, list2) with the blood directions 00278460 (mgr +30, 32 entries).
    explicit DecalManager(std::int32_t list1 = 100, std::int32_t list2 = 20);
    // 002797c0(mgr, point, texture, normal or 0, f12 = size).
    void add_blood(const DecalVector& point, std::int32_t texture, const std::optional<DecalVector>& normal, float size,
                   const BloodTrace& trace, const DecalNodeLookup& node);
    // 002792f0(mgr) up to its 003d4350 calls (the caller makes them in order;
    // the return value is ignored). A record whose fade reaches 0 is marked dead.
    // A record without an object throws (002792f0 dereferences +58 unchecked).
    std::vector<BloodProjectorRequest> blood_projectors(const DecalRootLookup& root, const BloodHiddenLookup& hidden,
                                                        const DecalNodeLookup& node);
    const std::array<DecalVector, blood_direction_count>& blood_directions() const { return directions_; }
    // 00279ca0(mgr, frame, texture, mode, object, f12 = size).
    void add(const DecalMatrix& frame, std::int32_t texture, std::uint8_t mode, std::uint64_t object, float size,
             const DecalNodeLookup& node);
    // 002795a0(mgr) (game::ResetLevel 0012f774): +5c = -2.0 (dead) on the
    // first count +10 records of list 1 and the first count +14 of list 2.
    void kill_all();
    // 00279630(mgr, f12 = dt).
    void service(float dt, const DecalRootLookup& root);
    // 00278650(mgr): the list-1 flushes that carry quads.
    std::vector<DecalFlush> draw(const DecalNodeLookup& node);

    std::int32_t size1() const { return size_8; }
    std::int32_t size2() const { return size_c; }
    std::int32_t count1() const { return count_10; }
    std::int32_t count2() const { return count_14; }
    std::int32_t write1() const { return write_18; }
    std::int32_t write2() const { return write_1c; }
    std::int32_t ahead1() const { return ahead_20; }
    std::int32_t ahead2() const { return ahead_24; }
    const std::vector<DecalRecord>& list1() const { return list1_; }
    std::vector<DecalRecord>& list1() { return list1_; }
    const std::vector<DecalRecord>& list2() const { return list2_; }
    std::vector<DecalRecord>& list2() { return list2_; }
    // Test hooks: the list-2 count and the list-1 header.
    void set_count2(std::int32_t n) { count_14 = n; }
    void set_header1(std::int32_t count, std::int32_t write) { count_10 = count; write_18 = write; }

private:
    std::vector<DecalRecord> list1_, list2_;
    std::int32_t size_8 = 0, size_c = 0, count_10 = 0, count_14 = 0, write_18 = 0, write_1c = 0;
    std::int32_t ahead_20 = 0, ahead_24 = 0;
    std::array<DecalVector, blood_direction_count> directions_{};
};

// 0027a370(rec, flags, point, normal, f12 = size, f13 = texture as a float,
// t0 = object): with flags & 2 and an object, rows (1,0,0,0), normal,
// (0,0,1,0), point, the last two through rigid-inverse(node +10) when the
// object has a node.
void blood_record_set(DecalRecord& record, std::uint32_t flags, const DecalVector& point, const DecalVector& normal,
                      float size, float texture, std::uint64_t object, const DecalNodeLookup& node);
// 00278460(out, n): n directions sweeping yaw by 360 / sqrt(n) and pitch from
// -90 by 180 / sqrt(n), through the soft-double cos 00120bc0 / sin 001211a8.
std::vector<DecalVector> blood_direction_table(std::int32_t n);

// 0027a570(rec, flags, frame, object, f12 = size, f13 = texture as a float).
void decal_record_set(DecalRecord& record, std::uint32_t flags, const DecalMatrix& frame, std::uint64_t object,
                      float size, float texture, const DecalNodeLookup& node);
// 00133200: the rigid inverse in place (3x3 transposed; a nonzero translation
// t becomes -t through the transposed 3x3, w kept).
DecalMatrix decal_rigid_inverse(const DecalMatrix& m);
// The vmulax / vmadday / vmaddaz / vmaddw row product: row r = sum_j a[r][j] * b[j].
DecalMatrix decal_multiply(const DecalMatrix& a, const DecalMatrix& b);

}  // namespace omega
