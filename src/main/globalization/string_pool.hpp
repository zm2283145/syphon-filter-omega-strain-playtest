#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
struct StringPoolEntry {
    // Full original slot after lowercasing the prefix before its first NUL.
    std::array<std::uint8_t, 16> key{};
    // Original bytes through the first NUL, without assuming an encoding.
    std::string text;
};

class StringPool {
public:
    std::int32_t lookup(std::string_view key) const;
    const std::vector<StringPoolEntry>& entries() const { return entries_; }
private:
    std::vector<StringPoolEntry> entries_;
    friend StringPool decode_string_pool(std::span<const std::uint8_t> bytes);
};

// Bounded base-pool reader; dynamic language/mission-pool replacement is separate.
StringPool decode_string_pool(std::span<const std::uint8_t> bytes);
}
