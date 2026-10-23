#pragma once
// The scene frame 0013e1b0's model draw order: the bucket lists it files the
// world's objects in (0013e250..0013e788), the order it walks them in
// (0013e844..0013ec60) and the bucket walk 0013eee0.
// Goldens: tools/ee/gen_scene_draw_walk.py (tests/scene_draw_walk_goldens.hpp).
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omega {

// The lists, named by their offset in 0013e1b0's frame (sp + offset is the
// count word, followed by up to 0x400 object pointers; 0013e374 drops later
// objects).
enum class SceneBucket : std::uint16_t {
    list_00d0 = 0x00d0,   // kind 0xd (never walked by the draw)
    list_10e0 = 0x10e0,   // actors with alpha +0x2c4c != 0x80
    list_20f0 = 0x20f0,   // GOBs of kind (+0x68) 4: drawn by 25f270
    list_3100 = 0x3100,   // GOBs of kind 6
    list_4110 = 0x4110,   // models with VUM header flag 8
    list_5120 = 0x5120,   // models with model flag 0x400000 (header flag 1); kind 4, kind 0x10 + 0023c140
    list_6130 = 0x6130,   // models with model flag 0x800000 (header flag 2, not 1)
    list_7140 = 0x7140,   // actors with alpha 0x80
    list_8150 = 0x8150,   // other models (0x200000); kind 3
    list_9160 = 0x9160,   // kind 5
};
inline constexpr std::size_t scene_bucket_capacity = 0x400;

// 00377700: the order bits it ORs into model +0x24 from VUM header +4:
// header flag 1 -> 0x400000, else flag 2 -> 0x800000, else 0x200000.
std::uint32_t scene_model_order_flags_00377700(std::uint32_t header_flags);
// 0013e4a8..0013e5a0, a model instance (kind 1; kind 0xf through +0x64;
// kind 0x10 through virtual +0x124) whose model is loaded: header flag 8 ->
// 4110, else model flag 0x800000 -> 6130, else 0x400000 -> 5120, else 8150.
SceneBucket scene_model_bucket(std::uint32_t header_flags);
// A GOB (+0x49 == 0xf): kind +0x68 4 -> 20f0, 6 -> 3100, else its model's.
SceneBucket scene_gob_bucket(std::uint32_t gob_kind, std::uint32_t header_flags);
// An actor (+0x49 == 2): halfword +0x2c4c (the model's alpha +0x2bec, model =
// actor + 0x60) 0x80 -> 7140, else 10e0.
SceneBucket scene_actor_bucket(std::uint16_t alpha_2c4c);

// One step of the draw sequence 0013e844..0013ec60.
enum class SceneStepKind : std::uint8_t {
    walk_list,      // 0013eee0(list, 0): list order
    walk_sorted,    // 0013eee0(list, 1): ascending cull distance +0x3c
    qsort_walk,     // 001273c8 qsort of list[1 .. count-1] (comparator 0013eeb0), then walk_list
    gob_draw,       // 25f270(cVisionFxMan, list, pass) on the kind-4 GOB list
    call,           // a non-model renderer call (address), not a bucket walk
};
struct SceneDrawStep {
    SceneStepKind kind{};
    SceneBucket bucket{};      // walks and gob_draw
    std::uint32_t pass{};      // gob_draw: 25f270's a2
    std::uint32_t address{};   // call
};
// The steps for vision mode [*004ffc30] (cVisionFxMan +0: 0 off, 1, 2). The
// pre-draw shadow walks 13f1a0 (8150, 7140, 5120, 4110, 10e0) are not listed.
// Mode 2 moves the 7140 and 10e0 walks behind the effect calls (0013ea28..,
// with its fog changes); 25f230 adds 25f270(.., 3100, 1) in modes 1 and 2.
std::vector<SceneDrawStep> scene_draw_steps(std::uint32_t vision_mode);

// The scene's objects in filing order (the world-array walk 0013e250..):
// each with its list and, for the sorted walks, its cull distance (+0x3c,
// the culler 003c8990's -max corner z). keyed = false: no distance hosted.
struct SceneDrawEntry {
    SceneBucket bucket{};
    float key{};
    bool keyed{true};
};
struct SceneDrawSequence {
    std::vector<std::size_t> order;           // entry indices in draw order
    std::vector<SceneBucket> unkeyed_walks;   // sorted walks drawn in list order: an entry had no key
    std::size_t dropped{};                    // entries past a list's 0x400 slots (0013e374)
    // Checkpoint 132cp7: scene_draw_steps(vision_mode), and per step the index in `order` where it begins
    // (steps.size() + 1 values, the last one order.size()): a walk's entries are [step_begin[s], step_begin[s + 1]),
    // a call or gob_draw step is empty and stands at step_begin[s].
    std::vector<SceneDrawStep> steps;
    std::vector<std::size_t> step_begin;
};
// The model draws of scene_draw_steps(vision_mode) over the entries: each
// list holds its first 0x400 entries in filing order; walk_list and the
// qsort walks keep list order (qsort hands the comparator 0013eeb0 slot
// addresses; it compares, unsigned, the words 0x68 past them: the object
// pointers 26 slots further on, or stale slots past the count, i.e. heap
// addresses the port does not have), walk_sorted is 0013eee0's order.
// gob_draw and calls draw no model: 25f270 makes only the corona calls
// 00378170 (00236f20, the object's vcalls +88 / +e0), never the object's +58.
SceneDrawSequence scene_draw_sequence(std::uint32_t vision_mode, std::span<const SceneDrawEntry> entries);

// 0013eee0(list, sorted): the order of the objects' virtual +0x58 calls, as
// indices into the list. sorted = 0: list order. sorted = 1: the objects are
// inserted into a multimap keyed by the float +0x3c (0013f770: key < node
// left, else right) and visited from the leftmost node, i.e. a stable
// ascending sort. The compare 0013f7a4 c.olt.s is the EE FPU's: denormal keys
// order as zeros and exponent-255 keys (no infinities or NaNs on the EE) beyond
// every finite key, by magnitude (checkpoint 132uk5).
std::vector<std::size_t> scene_walk_order_0013eee0(std::span<const float> keys, bool sorted);

}
