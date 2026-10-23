#pragma once
// PORT-ONLY (not in the original; LAN phase 2 groundwork, checkpoint 132ln): the little-endian byte helpers
// and the CRC32 shared by the LAN discovery packets (lan_discovery.hpp) and the LAN transport header
// (lan_transport.hpp). research/LAN_PLAY.md §5.2 (option M) and §5.3.
//
// The original has no LAN wire format: its datagrams are SCE-RT rt_udp (header builder 0x00308a58) under DME and
// Medius. Every format built on these helpers is port-defined and little-endian, so a Windows and a Linux build
// of the port talk to each other. Nothing here is wired into the game.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::lan {

// PORT-ONLY: CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320, initial value and final xor 0xFFFFFFFF;
// "123456789" -> 0xCBF43926).
std::uint32_t lan_crc32(std::span<const std::uint8_t> bytes);

// PORT-ONLY: append little-endian values to a byte vector.
class LanWriter {
public:
    explicit LanWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint8_t value) { out_.push_back(value); }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void bytes(std::span<const std::uint8_t> value) { out_.insert(out_.end(), value.begin(), value.end()); }
    // A fixed char[N] field: the text truncated to N - 1 bytes, NUL padded (always NUL terminated).
    void fixed_text(std::string_view text, std::size_t size);

private:
    std::vector<std::uint8_t>& out_;
};

// PORT-ONLY: read little-endian values from a byte span. A read past the end sets failed() and yields zeros.
class LanReader {
public:
    explicit LanReader(std::span<const std::uint8_t> in) : in_(in) {}
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    // A fixed char[N] field: the text up to the first NUL (or all N bytes when there is none).
    std::string fixed_text(std::size_t size);
    std::span<const std::uint8_t> bytes(std::size_t size);
    std::size_t offset() const { return offset_; }
    std::size_t remaining() const { return failed_ ? 0 : in_.size() - offset_; }
    bool failed() const { return failed_; }

private:
    bool take(std::size_t size);
    std::span<const std::uint8_t> in_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

// PORT-ONLY: serial-number arithmetic on 32-bit sequences (RFC 1982 style), so the reliable channel keeps its
// order across the 0xffffffff -> 0 wrap. The original's admission and ordered read (0x0030acd8 / 0x00307dd8) use
// signed compares with wrap special cases (research/OBJECTIVE_TRANSPORT.md); this is the port's model of that.
inline bool lan_seq_less(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) < 0;
}
inline bool lan_seq_less_equal(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) <= 0;
}

}  // namespace omega::lan
