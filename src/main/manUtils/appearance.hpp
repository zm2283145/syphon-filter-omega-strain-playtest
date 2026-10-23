#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>

namespace omega {
// Only the appearance text-packing boundary: no costume selection or mesh loading.
struct AppearancePacking {
    static constexpr std::size_t capacity = 0x440;
    std::array<std::uint8_t, capacity> bytes{};
    std::array<bool, capacity> initialized{};
    std::size_t input_size{}, terminator_offset{};
    std::size_t pair_count() const { return (terminator_offset - 0x40) / 0x20; }
    std::optional<std::string> slot(std::size_t index) const;
};
// Unwritten original destination state is unknown, not implicitly zero.
AppearancePacking pack_appearance(std::span<const std::uint8_t> source);
void write_appearance(std::ostream& output, const AppearancePacking& appearance);
}
