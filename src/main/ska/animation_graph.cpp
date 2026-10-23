#include "animation_graph.hpp"
#include "animation_slider.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
        throw std::runtime_error("Animation graph requires finite normal or zero arithmetic");
    return value;
}
float add(float a, float b) { return checked(a + b); }
float sub(float a, float b) { return checked(a - b); }
float mul(float a, float b) { return checked(a * b); }
void quaternion_valid(const GraphQuaternion& value) { for (float part : value) checked(part); }
GraphQuaternion multiply(const GraphQuaternion& left, const GraphQuaternion& right) {
    GraphQuaternion result;
    // VOPMULA, two VMADDAW instructions, then VOPMSUB: retain this order.
    for (unsigned index = 0; index != 3; ++index) {
        const unsigned y = (index + 1) % 3, z = (index + 2) % 3;
        const float cross_positive = mul(left[y], right[z]);
        const float first = add(cross_positive, mul(left[index], right[3]));
        const float second = add(first, mul(right[index], left[3]));
        result[index] = sub(second, mul(right[y], left[z]));
    }
    result[3] = sub(sub(sub(mul(left[3], right[3]), mul(left[2], right[2])),
        mul(left[1], right[1])), mul(left[0], right[0]));
    return result;
}
GraphQuaternion compose(const GraphQuaternion& incoming, const GraphQuaternion& local) {
    // Original fast paths test W alone, not an invented quaternion norm test.
    if (local[3] == 1.0F) return incoming;
    if (incoming[3] == 1.0F) return local;
    return multiply(incoming, local);
}
}

namespace {
void validate_leaf_policy(const GraphLeafPolicy& leaf) {
    if (leaf.clip_key.empty() || leaf.clip_key.size() > 255 || leaf.clip_key.find('\0') != std::string::npos)
        throw std::runtime_error("Animation graph leaf key is unsupported");
    for (float value : {leaf.range_start, leaf.range_end, leaf.phase_offset, leaf.rate_scale, leaf.duration}) checked(value);
    if (leaf.duration < 0.0F || leaf.rate_scale < 0.0F)
        throw std::runtime_error("Animation graph duration or rate is negative");
}
}

AnimationGraph::AnimationGraph(std::vector<AnimationGraphNode> nodes) : nodes_(std::move(nodes)) {
    if (nodes_.empty() || nodes_.size() > 4096)
        throw std::runtime_error("Animation graph node count is unsupported");
    auto target = [&](std::size_t index) {
        if (index >= nodes_.size()) throw std::runtime_error("Animation graph child index is invalid");
    };
    for (const auto& node : nodes_) {
        quaternion_valid(node.rotation);
        switch (node.kind) {
        case AnimationGraphKind::ControlledLeaf:
            checked(node.angle_start); checked(node.angle_end);
            for (float value : node.axis) checked(value);
            [[fallthrough]];
        case AnimationGraphKind::Leaf:
            validate_leaf_policy(node.leaf);
            break;
        case AnimationGraphKind::Slider: {
            std::vector<SliderChild> thresholds;
            for (const auto& child : node.slider_children) {
                target(child.node_index);
                thresholds.push_back({child.start, child.end});
            }
            // Validate scalar table without evaluating a transition interval.
            if (thresholds.empty() || thresholds.size() > 256)
                throw std::runtime_error("Animation graph slider count is unsupported");
            for (std::size_t i = 0; i != thresholds.size(); ++i) {
                checked(thresholds[i].start); checked(thresholds[i].end);
                if (i + 1 != thresholds.size() && (thresholds[i].start > thresholds[i].end ||
                        thresholds[i].end > thresholds[i + 1].start))
                    throw std::runtime_error("Animation graph slider thresholds are unordered");
            }
            break;
        }
        case AnimationGraphKind::Selector:
            if (node.selector_children.empty() || node.selector_children.size() > 256)
                throw std::runtime_error("Animation graph selector count is unsupported");
            for (const auto& child : node.selector_children) {
                target(child.node_index);
                if (child.rules.size() != node.selector_children.size())
                    throw std::runtime_error("Animation graph selector rule matrix is incomplete");
                for (const auto& rule : child.rules) {
                    checked(rule.duration_start); checked(rule.duration_end);
                    if (rule.duration_start < 0 || rule.duration_end < 0 || rule.channel < -1 ||
                        rule.redirect < -1 || (rule.redirect >= 0 &&
                        std::size_t(rule.redirect) >= node.selector_children.size()) ||
                        rule.intermediates.size() > 256)
                        throw std::runtime_error("Animation graph selector rule is unsupported");
                    float preceding_end = 0;
                    for (const auto& intermediate : rule.intermediates) {
                        target(intermediate.node_index);
                        checked(intermediate.start); checked(intermediate.end);
                        if (intermediate.start < preceding_end || intermediate.end < intermediate.start ||
                            intermediate.end > 1)
                            throw std::runtime_error("Animation graph selector waypoint bounds are unordered");
                        preceding_end = intermediate.end;
                    }
                }
            }
            break;
        default: throw std::runtime_error("Animation graph node kind is unsupported");
        }
    }
    std::vector<unsigned> colors(nodes_.size());
    std::function<void(std::size_t, unsigned)> visit = [&](std::size_t index, unsigned depth) {
        if (depth > 128) throw std::runtime_error("Animation graph depth exceeds supported limit");
        if (colors[index] == 1) throw std::runtime_error("Animation graph contains a cycle");
        if (colors[index] == 2) return;
        colors[index] = 1;
        const auto& node = nodes_[index];
        if (node.kind == AnimationGraphKind::Slider)
            for (const auto& child : node.slider_children) visit(child.node_index, depth + 1);
        if (node.kind == AnimationGraphKind::Selector)
            for (const auto& child : node.selector_children) {
                visit(child.node_index, depth + 1);
                for (const auto& rule : child.rules)
                    for (const auto& intermediate : rule.intermediates) visit(intermediate.node_index, depth + 1);
            }
        colors[index] = 2;
    };
    for (std::size_t index = 0; index != nodes_.size(); ++index) visit(index, 1);
}

std::size_t AnimationGraph::append_leaf(AnimationGraphNode node) {
    if (node.kind != AnimationGraphKind::Leaf) throw std::runtime_error("Animation graph append takes a leaf");
    quaternion_valid(node.rotation);
    validate_leaf_policy(node.leaf);
    if (nodes_.size() >= 4096) throw std::runtime_error("Animation graph node count is unsupported");
    for (const auto& existing : nodes_)
        if (existing.name == node.name) throw std::runtime_error("Animation graph append of an existing node name: " + node.name);
    nodes_.push_back(std::move(node));
    return nodes_.size() - 1;
}

void AnimationGraph::validate_context(const AnimationGraphContext& context) const {
    if (context.channels.size() > 4096 || context.selectors.size() > 4096 || context.transitions.size() > 4096)
        throw std::runtime_error("Animation graph context count exceeds supported limit");
    for (float value : context.channels) checked(value);
    std::vector<bool> seen(nodes_.size());
    for (const auto& state : context.transitions) {
        if (state.node_index >= nodes_.size() || seen[state.node_index])
            throw std::runtime_error("Animation graph selector state is invalid or duplicated");
        seen[state.node_index] = true;
        const auto& node = nodes_[state.node_index];
        if (node.kind != AnimationGraphKind::Selector || state.source_index >= node.selector_children.size() ||
            state.target_index >= node.selector_children.size() || node.channel >= context.selectors.size())
            throw std::runtime_error("Animation graph selector state has invalid indices");
        checked(state.elapsed); checked(state.blend);
        if (state.elapsed < 0 || state.blend < 0 || state.blend > 1 ||
            (state.ready && (state.source_node >= nodes_.size() || state.target_node >= nodes_.size())))
            throw std::runtime_error("Animation graph selector state has unsupported values");
    }
}

std::vector<GraphLeafContribution> AnimationGraph::evaluate(std::size_t root, float parent_weight,
    const GraphQuaternion& incoming, AnimationGraphContext& context) const {
    checked(parent_weight); quaternion_valid(incoming);
    if (parent_weight < 0.0F || root >= nodes_.size())
        throw std::runtime_error("Animation graph root or weight is invalid");
    validate_context(context);
    auto next = context;
    auto channel = [&](std::size_t index) {
        if (index >= context.channels.size()) throw std::runtime_error("Animation graph scalar channel is absent");
        return context.channels[index];
    };
    std::vector<GraphLeafContribution> result;
    unsigned visits = 0;
    std::function<void(std::size_t, float, const GraphQuaternion&, unsigned)> visit =
        [&](std::size_t index, float weight, const GraphQuaternion& parent, unsigned depth) {
            if (++visits > 10000 || depth > 128)
                throw std::runtime_error("Animation graph evaluation exceeds supported traversal budget");
            const auto& node = nodes_[index];
            if (node.kind == AnimationGraphKind::ControlledLeaf) {
                const float difference = sub(node.angle_end, node.angle_start);
                const float angle = add(node.angle_start, mul(channel(node.channel), difference));
                const float half = mul(0.5F, angle);
                const float cosine = checked(std::cos(half)), sine = checked(std::sin(half));
                GraphQuaternion rotation{mul(sine, node.axis[0]), mul(sine, node.axis[1]),
                    mul(sine, node.axis[2]), cosine};
                if (parent[3] != 1.0F) rotation = multiply(parent, rotation);
                if (node.rotation[3] != 1.0F) rotation = multiply(rotation, node.rotation);
                result.push_back({index, weight, rotation});
            } else {
                const auto rotation = compose(parent, node.rotation);
                if (node.kind == AnimationGraphKind::Leaf) {
                    result.push_back({index, weight, rotation});
                } else if (node.kind == AnimationGraphKind::Slider) {
                    std::vector<SliderChild> thresholds;
                    for (const auto& child : node.slider_children) thresholds.push_back({child.start, child.end});
                    const auto contributions = select_slider_contributions(thresholds,
                        channel(node.channel), weight, node.smooth);
                    for (const auto& contribution : contributions)
                        visit(node.slider_children[contribution.child_index].node_index,
                            contribution.weight, rotation, depth + 1);
                } else {
                    if (node.channel >= next.selectors.size())
                        throw std::runtime_error("Animation graph selector channel is absent");
                    const auto selection = next.selectors[node.channel];
                    if (selection.requested >= node.selector_children.size() ||
                        selection.previous >= node.selector_children.size())
                        throw std::runtime_error("Animation graph selected child is invalid");
                    const auto state = std::find_if(next.transitions.begin(), next.transitions.end(),
                        [&](const auto& entry) { return entry.node_index == index; });
                    if (state == next.transitions.end()) {
                        visit(node.selector_children[selection.previous].node_index, weight, rotation, depth + 1);
                    } else {
                        if (!state->ready)
                            throw std::runtime_error("Animation graph pending selector requires update before evaluation");
                        // Child evaluation may append other selector records: retain values, not an iterator.
                        const auto active = *state;
                        const float target_weight = mul(weight, active.blend);
                        const float source_weight = std::max(0.0F, sub(weight, target_weight));
                        visit(active.source_node, source_weight, rotation, depth + 1);
                        visit(active.target_node, target_weight, rotation, depth + 1);
                    }
                    const bool exists = std::any_of(next.transitions.begin(), next.transitions.end(),
                        [&](const auto& entry) { return entry.node_index == index; });
                    if (selection.requested != selection.previous && !exists) {
                        if (next.transitions.size() == 4096)
                            throw std::runtime_error("Animation graph selector transition budget exceeded");
                        GraphSelectorTransition pending;
                        pending.node_index = index;
                        pending.source_index = selection.previous;
                        pending.target_index = selection.requested;
                        next.transitions.push_back(pending);
                    }
                }
            }
        };
    visit(root, parent_weight, incoming, 1);
    context = std::move(next);
    return result;
}

void AnimationGraph::advance_selectors(AnimationGraphContext& context, float effective_dt) const {
    checked(effective_dt);
    if (effective_dt < 0) throw std::runtime_error("Animation graph selector time is negative");
    validate_context(context);
    auto next = context;
    for (std::size_t index = 0; index < next.transitions.size();) {
        auto& state = next.transitions[index];
        const auto& node = nodes_[state.node_index];
        const auto previous = next.selectors[node.channel].previous;
        if (previous >= node.selector_children.size())
            throw std::runtime_error("Animation graph previous selector index is invalid");
        std::vector<bool> visited(node.selector_children.size());
        const GraphSelectorRule* rule = nullptr;
        for (;;) {
            if (visited[state.target_index])
                throw std::runtime_error("Animation graph selector redirect cycle");
            visited[state.target_index] = true;
            rule = &node.selector_children[state.source_index].rules[state.target_index];
            if (rule->redirect == -1) break;
            state.target_index = std::size_t(rule->redirect);
        }
        float duration = rule->duration_start;
        if (rule->channel != -1) {
            if (std::size_t(rule->channel) >= next.channels.size())
                throw std::runtime_error("Animation graph rule duration channel is absent");
            duration = add(rule->duration_start,
                mul(next.channels[std::size_t(rule->channel)], sub(rule->duration_end, rule->duration_start)));
        }
        if (duration < 0) throw std::runtime_error("Animation graph computed selector duration is negative");
        bool remove = duration == 0;
        if (!remove) {
            if (rule->reversible) {
                if (!state.reversing && previous == state.source_index) state.reversing = true;
                else if (state.reversing && previous == state.target_index) state.reversing = false;
            }
            if (!state.reversing) {
                state.elapsed = add(state.elapsed, effective_dt);
                if (state.elapsed >= duration) {
                    state.source_index = state.target_index;
                    state.target_index = previous;
                    state.elapsed = 0;
                    remove = state.source_index == previous;
                }
            } else {
                state.elapsed = sub(state.elapsed, effective_dt);
                if (state.elapsed <= 0) {
                    state.reversing = false;
                    state.target_index = previous;
                    state.elapsed = 0;
                    remove = state.source_index == previous;
                }
            }
        }
        if (remove) {
            next.transitions.erase(next.transitions.begin() + std::ptrdiff_t(index));
            continue;
        }
        // Preserve the rule selected before the possible source/target shift above.
        const float normalized = std::min(1.0F, checked(state.elapsed / duration));
        float lower = 0;
        std::size_t left = node.selector_children[state.source_index].node_index;
        const auto terminal = node.selector_children[state.target_index].node_index;
        for (std::size_t waypoint = 0; waypoint <= rule->intermediates.size(); ++waypoint) {
            const bool last = waypoint == rule->intermediates.size();
            const float upper = last ? 1.0F : rule->intermediates[waypoint].start;
            const auto right = last ? terminal : rule->intermediates[waypoint].node_index;
            if (normalized <= upper) {
                state.source_node = left;
                if (normalized <= lower) {
                    state.target_node = left;
                    state.blend = 1;
                    state.interpolating = false;
                } else {
                    state.target_node = right;
                    float value = checked(sub(normalized, lower) / sub(upper, lower));
                    if (rule->smooth) {
                        const float squared = mul(value, value);
                        const float cubed = mul(value, squared);
                        value = sub(mul(3, squared), mul(2, cubed));
                    }
                    state.blend = std::min(std::max(value, 0.0F), 1.0F);
                    state.interpolating = true;
                }
                state.ready = true;
                break;
            }
            left = right;
            lower = rule->intermediates[waypoint].end;
        }
        ++index;
    }
    context = std::move(next);
}

void AnimationGraph::finish_selector_frame(AnimationGraphContext& context) const {
    validate_context(context);
    for (auto& selector : context.selectors) selector.previous = selector.requested;
}

std::vector<GraphLeafContribution> AnimationGraph::evaluate_frame(std::size_t root, float parent_weight,
    const GraphQuaternion& incoming, AnimationGraphContext& context, float effective_dt) const {
    auto next = context;
    advance_selectors(next, effective_dt);
    auto result = evaluate(root, parent_weight, incoming, next);
    finish_selector_frame(next);
    context = std::move(next);
    return result;
}
}
