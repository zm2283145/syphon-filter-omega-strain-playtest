#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace omega {

// Scalar threshold pair in each original slider child record (+04, +08).
struct SliderChild { float start{}, end{}; };
struct SliderContribution { std::size_t child_index{}; float weight{}; };

// Pure scalar selection from 003b0760. Quaternion composition, channel lookup,
// child callbacks and callback mutations are outside this bounded component.
// Children: 1..256, ordered starts; each nonfinal start <= end <= next start.
// All inputs and evaluated intermediates must be finite normal values or zero.
// Parent weight must be nonnegative; channel value has no [0,1] restriction.
// Smooth interpolation uses separately rounded floats, not an EE ACC model.
std::vector<SliderContribution> select_slider_contributions(
    std::span<const SliderChild> children, float value, float parent_weight, bool smooth);

}
