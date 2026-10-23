#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omega {
using GraphQuaternion = std::array<float, 4>;
enum class AnimationGraphKind : std::uint8_t { Leaf, ControlledLeaf, Slider, Selector };

// Loaded configuration, never a compiled-in copy of a game's graph or clip data.
struct GraphLeafPolicy {
    std::string clip_key;
    std::uint32_t source_word20{};
    bool reverse{}, mirror{};
    float range_start{}, range_end{1}, phase_offset{}, rate_scale{1}, duration{};
    std::int32_t group{-1};
    std::uint8_t motion_bucket{};
    bool looping{}, align_sign{};
};
struct GraphSliderChild { std::size_t node_index{}; float start{}, end{}; };
struct GraphSelectorRule {
    std::vector<GraphSliderChild> intermediates;
    float duration_start{}, duration_end{};
    std::int32_t channel{-1};
    bool reversible{}, smooth{};
    std::int32_t redirect{-1};
};
struct GraphSelectorChild {
    std::size_t node_index{};
    std::vector<GraphSelectorRule> rules;
};
struct AnimationGraphNode {
    AnimationGraphKind kind{AnimationGraphKind::Leaf};
    std::string name;
    GraphQuaternion rotation{0, 0, 0, 1};
    GraphLeafPolicy leaf;
    std::size_t channel{};
    float angle_start{}, angle_end{};
    std::array<float, 3> axis{};
    bool smooth{};
    std::uint8_t source_flag35{}; // Retained source metadata; slider evaluator does not read it.
    std::vector<GraphSliderChild> slider_children;
    std::vector<GraphSelectorChild> selector_children;
};
struct GraphSelectorChannel { std::size_t requested{}, previous{}; };
struct GraphSelectorTransition {
    std::size_t node_index{}, source_index{}, target_index{};
    float elapsed{}, blend{};
    bool reversing{}, interpolating{}, ready{};
    std::size_t source_node{}, target_node{};
};
struct AnimationGraphContext {
    std::vector<float> channels;
    std::vector<GraphSelectorChannel> selectors;
    std::vector<GraphSelectorTransition> transitions;
};
struct GraphLeafContribution {
    std::size_t node_index{};
    float weight{};
    GraphQuaternion rotation{};
};

// Owns a bounded acyclic graph (nodes only appended, never changed). Evaluation collects ordered leaf
// requests under an explicit active transition host; it does not sample clips.
// Scalar/VU arithmetic is represented by ordered portable binary32 operations;
// native sin/cos approximate the original library and are not PS2 bit proofs.
// Host guards require nonnegative parent weight/time and finite normal/zero
// input/arithmetic. Reuse a context only with the graph that created its states.
class AnimationGraph {
public:
    explicit AnimationGraph(std::vector<AnimationGraphNode> nodes);
    const std::vector<AnimationGraphNode>& nodes() const { return nodes_; }
    // 003b4800's push_back of a clip node (LoadAnimation, level_animation.hpp):
    // a validated leaf whose name no node has; returns its index (the old
    // count). Existing indices and contexts stay valid.
    std::size_t append_leaf(AnimationGraphNode node);
    std::vector<GraphLeafContribution> evaluate(std::size_t root, float parent_weight,
        const GraphQuaternion& incoming, AnimationGraphContext& context) const;
    // Original model order: update retained selectors, evaluate graph, then copy
    // requested selector values to previous. Each public mutation is atomic.
    void advance_selectors(AnimationGraphContext& context, float effective_dt) const;
    void finish_selector_frame(AnimationGraphContext& context) const;
    std::vector<GraphLeafContribution> evaluate_frame(std::size_t root, float parent_weight,
        const GraphQuaternion& incoming, AnimationGraphContext& context, float effective_dt) const;
private:
    void validate_context(const AnimationGraphContext& context) const;
    std::vector<AnimationGraphNode> nodes_;
};
}
