#include "animation_slider.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float checked(float value) {
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
        throw std::runtime_error("Animation slider requires finite normal or zero arithmetic");
    return value;
}
}

std::vector<SliderContribution> select_slider_contributions(
    std::span<const SliderChild> children, float value, float parent_weight, bool smooth) {
    checked(value);
    checked(parent_weight);
    if (parent_weight < 0.0F || children.empty() || children.size() > 256)
        throw std::runtime_error("Animation slider weight or child count is unsupported");
    for (std::size_t index = 0; index != children.size(); ++index) {
        const auto& child = children[index];
        checked(child.start);
        checked(child.end);
        if (index + 1 != children.size()) {
            checked(children[index + 1].start);
            if (child.start > child.end || child.end > children[index + 1].start)
                throw std::runtime_error("Animation slider thresholds are not ordered");
        }
    }

    std::vector<SliderContribution> result;
    float carried = 0.0F;
    for (std::size_t index = 0; index != children.size(); ++index) {
        const auto& child = children[index];
        const bool last = index + 1 == children.size();
        const float upper = last ? 1.0F : children[index + 1].start;
        float remainder = 0.0F;
        if (value >= child.start && (value < upper || last)) {
            carried = parent_weight;
            if (value > child.end && !last) {
                const float denominator = checked(upper - child.end);
                const float numerator = checked(upper - value);
                float ratio = checked(numerator / denominator);
                if (smooth) {
                    const float squared = checked(ratio * ratio);
                    const float accumulated = checked(3.0F * squared);
                    const float cubed = checked(ratio * squared);
                    const float doubled = checked(2.0F * cubed);
                    ratio = checked(accumulated - doubled);
                }
                carried = std::min(parent_weight, checked(parent_weight * ratio));
                remainder = checked(parent_weight - carried);
            }
        }
        if (carried != 0.0F) result.push_back({index, carried});
        carried = remainder;
    }
    return result;
}

}
