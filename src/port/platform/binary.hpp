#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>

namespace omega {

inline std::span<const std::uint8_t> checked_bytes(std::span<const std::uint8_t> bytes,
                                                  std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Binary field outside input");
    return bytes.subspan(offset, size);
}

inline std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto value = checked_bytes(bytes, offset, 2);
    return static_cast<std::uint16_t>(value[0] | (std::uint16_t(value[1]) << 8));
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto value = checked_bytes(bytes, offset, 4);
    return std::uint32_t(value[0]) | (std::uint32_t(value[1]) << 8)
        | (std::uint32_t(value[2]) << 16) | (std::uint32_t(value[3]) << 24);
}

inline std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    checked_bytes(bytes, offset, 8);
    return std::uint64_t(read_u32(bytes, offset)) | (std::uint64_t(read_u32(bytes, offset + 4)) << 32);
}

}
