#include "scene_draw_order.hpp"

#include <algorithm>
#include <bit>
#include <numeric>

namespace omega {

std::uint32_t scene_model_order_flags_00377700(std::uint32_t header_flags) {
    if (header_flags & 1U) return 0x400000U;
    if (header_flags & 2U) return 0x800000U;
    return 0x200000U;
}

SceneBucket scene_model_bucket(std::uint32_t header_flags) {
    if (header_flags & 8U) return SceneBucket::list_4110;
    const auto flags = scene_model_order_flags_00377700(header_flags);
    if (flags & 0x800000U) return SceneBucket::list_6130;
    if (flags & 0x400000U) return SceneBucket::list_5120;
    return SceneBucket::list_8150;
}

SceneBucket scene_gob_bucket(std::uint32_t gob_kind, std::uint32_t header_flags) {
    if (gob_kind == 4) return SceneBucket::list_20f0;
    if (gob_kind == 6) return SceneBucket::list_3100;
    return scene_model_bucket(header_flags);
}

SceneBucket scene_actor_bucket(std::uint16_t alpha_2c4c) {
    return alpha_2c4c == 0x80 ? SceneBucket::list_7140 : SceneBucket::list_10e0;
}

std::vector<SceneDrawStep> scene_draw_steps(std::uint32_t vision_mode) {
    using K = SceneStepKind;
    using B = SceneBucket;
    const auto walk = [](K kind, B bucket) { return SceneDrawStep{kind, bucket, 0, 0}; };
    const auto call = [](std::uint32_t address) { return SceneDrawStep{K::call, {}, 0, address}; };
    const bool mode2 = vision_mode == 2;
    std::vector<SceneDrawStep> steps{call(0x3d37e0), call(0x2792f0), walk(K::walk_list, B::list_9160),
                                     walk(K::qsort_walk, B::list_8150)};
    if (!mode2) steps.push_back(walk(K::walk_sorted, B::list_7140));          // 0013e8e0
    steps.push_back(walk(K::qsort_walk, B::list_6130));
    steps.push_back(call(0x37bd40));
    steps.push_back(walk(K::walk_list, B::list_4110));
    steps.push_back(walk(K::walk_sorted, B::list_5120));
    steps.push_back(call(0x2b1cb0));
    steps.push_back(call(0x278650));
    steps.push_back(call(0x3ec300));
    steps.push_back(walk(K::walk_list, B::list_3100));
    // 25f230: 25f270(.., 3100, 1) for vision modes 1 and 2 only.
    if (vision_mode == 1 || vision_mode == 2) steps.push_back({K::gob_draw, B::list_3100, 1, 0});
    steps.push_back({K::gob_draw, B::list_20f0, 0, 0});
    if (!mode2) steps.push_back(walk(K::walk_list, B::list_10e0));            // 0013e9a4
    for (int k = 0; k < 6; ++k) steps.push_back(call(0x3bafc0));               // 0x539258 + 0x14 k
    steps.push_back(call(0x25f4c0));
    steps.push_back(call(0x25f930));
    if (mode2) {                                                               // 0013ea28..0013eb94
        steps.push_back(call(0x37a1e0));
        steps.push_back(walk(K::walk_sorted, B::list_7140));
        steps.push_back(walk(K::walk_list, B::list_10e0));
    }
    return steps;
}

namespace {

// The EE FPU's order of a float word (0013f7a4 c.olt.s): no infinities or NaNs, so exponent 255 is a normal
// number beyond FLT_MAX; a zero exponent reads as zero of either sign; then sign and magnitude.
std::int64_t ee_compare_key(float value) {
    const auto word = std::bit_cast<std::uint32_t>(value);
    if (((word >> 23) & 0xffU) == 0) return 0;
    const auto magnitude = static_cast<std::int64_t>(word & 0x7fffffffU);
    return (word >> 31) ? -magnitude : magnitude;
}

}

std::vector<std::size_t> scene_walk_order_0013eee0(std::span<const float> keys, bool sorted) {
    std::vector<std::size_t> order(keys.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (!sorted) return order;
    std::vector<std::int64_t> ranks;
    ranks.reserve(keys.size());
    for (const float key : keys) ranks.push_back(ee_compare_key(key));
    // Equal keys (including -0, +0 and denormals) go right of the node: list order.
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ranks[a] < ranks[b]; });
    return order;
}

SceneDrawSequence scene_draw_sequence(std::uint32_t vision_mode, std::span<const SceneDrawEntry> entries) {
    SceneDrawSequence out;
    const auto list_of = [&](SceneBucket bucket) {
        std::vector<std::size_t> list;
        for (std::size_t k = 0; k < entries.size(); ++k) {
            if (entries[k].bucket != bucket) continue;
            if (list.size() < scene_bucket_capacity) list.push_back(k);
        }
        return list;
    };
    std::vector<SceneBucket> buckets;
    for (const auto& entry : entries)
        if (std::find(buckets.begin(), buckets.end(), entry.bucket) == buckets.end()) buckets.push_back(entry.bucket);
    for (const auto bucket : buckets) {
        const auto total = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
            [&](const SceneDrawEntry& entry) { return entry.bucket == bucket; }));
        if (total > scene_bucket_capacity) out.dropped += total - scene_bucket_capacity;
    }
    out.steps = scene_draw_steps(vision_mode);
    for (const auto& step : out.steps) {
        out.step_begin.push_back(out.order.size());
        if (step.kind == SceneStepKind::call || step.kind == SceneStepKind::gob_draw) continue;
        const auto list = list_of(step.bucket);
        if (step.kind != SceneStepKind::walk_sorted) {
            out.order.insert(out.order.end(), list.begin(), list.end());
            continue;
        }
        const bool keyed = std::all_of(list.begin(), list.end(), [&](std::size_t k) { return entries[k].keyed; });
        if (!keyed) {
            out.unkeyed_walks.push_back(step.bucket);
            out.order.insert(out.order.end(), list.begin(), list.end());
            continue;
        }
        std::vector<float> keys;
        for (const auto k : list) keys.push_back(entries[k].key);
        for (const auto index : scene_walk_order_0013eee0(keys, true)) out.order.push_back(list[index]);
    }
    out.step_begin.push_back(out.order.size());
    return out;
}

}
