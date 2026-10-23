// PORT-ONLY (not in the original; checkpoint 132ln): little-endian byte helpers and CRC32 for the LAN
// formats. See lan_wire.hpp and research/LAN_PLAY.md §5.2 / §5.3.
#include "lan_wire.hpp"

#include <algorithm>

namespace omega::lan {

namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        table[i] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

}  // namespace

std::uint32_t lan_crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : bytes) crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void LanWriter::u16(std::uint16_t value) {
    out_.push_back(static_cast<std::uint8_t>(value));
    out_.push_back(static_cast<std::uint8_t>(value >> 8));
}

void LanWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out_.push_back(static_cast<std::uint8_t>(value >> shift));
}

void LanWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out_.push_back(static_cast<std::uint8_t>(value >> shift));
}

void LanWriter::fixed_text(std::string_view text, std::size_t size) {
    if (size == 0) return;
    const std::size_t used = std::min(text.size(), size - 1);
    for (std::size_t i = 0; i < size; ++i) out_.push_back(i < used ? static_cast<std::uint8_t>(text[i]) : 0);
}

bool LanReader::take(std::size_t size) {
    if (failed_ || in_.size() - offset_ < size) {
        failed_ = true;
        return false;
    }
    return true;
}

std::uint8_t LanReader::u8() {
    if (!take(1)) return 0;
    return in_[offset_++];
}

std::uint16_t LanReader::u16() {
    if (!take(2)) return 0;
    const auto value = static_cast<std::uint16_t>(in_[offset_] | (in_[offset_ + 1] << 8));
    offset_ += 2;
    return value;
}

std::uint32_t LanReader::u32() {
    if (!take(4)) return 0;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in_[offset_ + i]) << (8 * i);
    offset_ += 4;
    return value;
}

std::uint64_t LanReader::u64() {
    if (!take(8)) return 0;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(in_[offset_ + i]) << (8 * i);
    offset_ += 8;
    return value;
}

std::string LanReader::fixed_text(std::size_t size) {
    if (!take(size)) return {};
    std::string text;
    for (std::size_t i = 0; i < size && in_[offset_ + i] != 0; ++i) text.push_back(static_cast<char>(in_[offset_ + i]));
    offset_ += size;
    return text;
}

std::span<const std::uint8_t> LanReader::bytes(std::size_t size) {
    if (!take(size)) return {};
    const auto view = in_.subspan(offset_, size);
    offset_ += size;
    return view;
}

}  // namespace omega::lan
