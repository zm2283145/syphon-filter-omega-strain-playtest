#include "texture.hpp"
#include "binary.hpp"
#include "disc.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

constexpr std::size_t gs_memory_size = 4 * 1024 * 1024;
std::atomic<TextureDecodeHook> decode_hook{nullptr};  // port-only (132md1): the mod layer's texture replacement

std::uint32_t block_index(std::uint32_t x, std::uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2);
}

std::size_t address32(std::uint32_t x, std::uint32_t y, std::uint32_t buffer_width) {
    const auto page = (y / 32) * buffer_width + x / 64;
    const auto column = (x & 1) | ((y & 1) << 1) | ((x & 6) << 1) | ((y & 6) << 3);
    return std::size_t(page) * 8192 + block_index((x / 8) & 7, (y / 8) & 3) * 256 + column * 4;
}

std::size_t address8(std::uint32_t x, std::uint32_t y, std::uint32_t buffer_width) {
    const auto page = (y / 64) * (buffer_width / 2) + x / 128;
    // GS byte addressing includes an XOR between the x/column and y bits.
    const auto column = ((y >> 1) & 1) | ((x & 8) >> 2) | ((x & 1) << 2)
        | ((y & 1) << 3) | ((x & 2) << 3)
        | ((((x >> 2) ^ (y >> 1) ^ (y >> 2)) & 1) << 5)
        | ((y & 4) << 4) | ((y & 8) << 4);
    return std::size_t(page) * 8192 + block_index((x / 16) & 7, (y / 16) & 3) * 256 + column;
}

// PSMT4 nibble address: 128x128 pages, 32x16 blocks (same block order as
// PSMCT16), and the in-block column layout. Bit form derived from the GS
// memory organisation and checked against an independent emulator table.
std::size_t nibble_address4(std::uint32_t x, std::uint32_t y, std::uint32_t buffer_width) {
    const auto page = (y / 128) * (buffer_width / 2) + x / 128;
    const auto bx = (x / 32) & 3, by = (y / 16) & 7;
    const auto block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
    const auto cx = x & 31, cy = y & 15;
    const auto column = ((cy >> 1) & 1) | (((cx >> 3) & 1) << 1) | (((cx >> 4) & 1) << 2)
        | ((cx & 1) << 3) | ((cy & 1) << 4) | (((cx >> 1) & 1) << 5)
        | ((((cx >> 2) ^ (cy >> 1) ^ (cy >> 2)) & 1) << 6) | (((cy >> 2) & 1) << 7) | (((cy >> 3) & 1) << 8);
    return std::size_t(page) * 16384 + std::size_t(block) * 512 + column;
}

struct Transfer {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t buffer_width{};
    std::uint32_t bytes{};
    std::uint32_t source_offset{};
    bool uploaded = true;
};

// doubled_count: IMAGE/REF counts are twice the rectangle (the swizzled
// 4-bit domain). The GS consumes exactly the TRXREG rectangle and ignores the
// remainder of an IMAGE transfer, so the rectangle bounds the source.
// row: the TRXPOS destination row (DSAY) of a level's later transfer.
Transfer transfer(std::span<const std::uint8_t> bytes, std::size_t registers, unsigned expected_psm = 0,
                  bool doubled_count = false, std::uint32_t row = 0) {
    checked_bytes(bytes, registers, 96);
    if (registers % 16) throw std::runtime_error("Unaligned TDX transfer registers");
    for (std::size_t i = 0; i < 4; ++i) {
        if (read_u64(bytes, registers + i * 16 + 8) != 0x50 + i)
            throw std::runtime_error("Unsupported TDX GS transfer register sequence");
    }
    if (read_u64(bytes, registers + 16) != std::uint64_t(row) << 48 || read_u64(bytes, registers + 48) != 0)
        throw std::runtime_error("Unsupported TDX transfer position or direction");
    const auto destination = read_u32(bytes, registers + 4);
    if ((destination & 0x3fff) || ((destination >> 24) & 63) != expected_psm)
        throw std::runtime_error("Unsupported TDX transfer: unexpected upload format or base");
    const auto width = read_u32(bytes, registers + 32);
    const auto height = read_u32(bytes, registers + 36);
    if (width == 0 || height == 0 || width > 512 || height > 512)
        throw std::runtime_error("Unsupported TDX transfer dimensions");
    const auto size = expected_psm == 0x14 ? width * height / 2 : expected_psm == 0x13 ? width * height
        : expected_psm == 1 ? width * height * 3 : width * height * 4;
    const auto image_tag = read_u64(bytes, registers + 64);
    const auto dma_tag = read_u32(bytes, registers + 80);
    const auto count = (doubled_count ? size * 2 : size) / 16;
    // A rectangle under one quadword (the 4 x 4 PSMT4 level of a 32 x 32 TDX with MXL 3, 8 bytes) has IMAGE NLOOP 0
    // and REF QWC 0: the GS receives none of its texels (uploaded false).
    if ((size % 16 && count != 0) || (image_tag >> 58) != 2 || (image_tag & 0x7fff) != count
        || (dma_tag >> 16) != 0x3000 || (dma_tag & 0xffff) != count)
        throw std::runtime_error("TDX transfer length or IMAGE/REF tag mismatch");
    const auto buffer_width = (destination >> 16) & 63;
    if (buffer_width == 0 || buffer_width * 64 < width)
        throw std::runtime_error("Invalid TDX transfer buffer width");
    return {width, height, buffer_width, size, read_u32(bytes, registers + 84), count != 0};
}

std::size_t bounded_offset(std::uint64_t value, std::size_t end) {
    if (value > end) throw std::runtime_error("TDX relative offset outside frame");
    return static_cast<std::size_t>(value);
}

// Header +0x22 is the frame count and +0x38 the frame length: the upload at
// 003bb3f0..003bb408 starts frame N at +0x40 + N * (+0x38). The texture
// object's frame index (+0x4c) starts at 0 (003bba44) and is advanced by the
// frame animator 003d1960 (material_anim.cpp). Returns the end of frame 0.
std::size_t first_frame_end(std::span<const std::uint8_t> bytes) {
    const unsigned frames = read_u16(bytes, 34);
    const std::uint64_t length = read_u32(bytes, 56);
    if (frames == 0) throw std::runtime_error("Unsupported TDX animation frame count");
    const auto end = bounded_offset(length + 64, bytes.size());
    const auto all = bounded_offset(length * frames + 64, bytes.size());
    if (end < 96 || bytes.size() - all > 2047
        || std::any_of(bytes.begin() + static_cast<std::ptrdiff_t>(all), bytes.end(), [](auto byte) { return byte != 0; }))
        throw std::runtime_error("Invalid TDX frame length or padding");
    return end;
}

void put32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) output.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

}

namespace {
enum class LevelMode { psmt8_from_32, direct8, direct4, swizzled4 };
// One level: validate its transfer against the expected sampled size, then
// reproduce the GS upload/sample addressing (direct uploads cancel).
// The per-texel model the faster decode_level below replaced (checkpoint 132win5), kept as its test
// reference (decode_tdx_reference).
thread_local bool reference_levels = false;
std::vector<std::uint8_t> decode_level_reference(std::span<const std::uint8_t> bytes, const Transfer& pixels, LevelMode mode,
    std::size_t source, std::uint32_t width, std::uint32_t height, std::uint32_t buffer_width,
    std::size_t palette_source, bool alpha, std::vector<std::uint8_t>& raw_alpha) {
    std::vector<std::uint8_t> rgba;
    rgba.reserve(std::size_t(width) * height * 4);
    raw_alpha.clear();
    raw_alpha.reserve(std::size_t(width) * height);
    const auto emit = [&](unsigned palette_index) {
        const auto color = palette_source + palette_index * 4;
        rgba.push_back(bytes[color]); rgba.push_back(bytes[color + 1]); rgba.push_back(bytes[color + 2]);
        rgba.push_back(alpha ? static_cast<std::uint8_t>(std::min(255U, unsigned(bytes[color + 3]) * 2)) : 255);
        // Without TCC (header +2 alpha flags clear) the GS takes As = 0x80.
        raw_alpha.push_back(alpha ? bytes[color + 3] : std::uint8_t{0x80});
    };
    // The 256-entry CSM1 palette swaps index bits 3 and 4; 16 entries do not.
    const auto permute8 = [](unsigned index) { return (index & ~24U) | ((index & 8) << 1) | ((index & 16) >> 1); };
    if (mode == LevelMode::direct4) {
        // Equal PSMT4 upload/sample rectangles cancel GS addressing; low nibble first.
        for (std::size_t i = 0; i < std::size_t(width) * height; ++i) emit((bytes[source + i / 2] >> ((i & 1) * 4)) & 15);
        return rgba;
    }
    if (mode == LevelMode::direct8) {
        for (std::size_t i = 0; i < std::size_t(width) * height; ++i) emit(permute8(bytes[source + i]));
        return rgba;
    }
    std::size_t limit = 0;
    for (std::uint32_t y = 0; y < pixels.height; ++y)
        for (std::uint32_t x = 0; x < pixels.width; ++x) limit = std::max(limit, address32(x, y, pixels.buffer_width) + 4);
    if (limit > gs_memory_size) throw std::runtime_error("TDX upload exceeds GS memory");
    std::vector<std::uint8_t> memory(limit);
    std::vector<bool> written(limit, false);
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            const auto destination = address32(x, y, pixels.buffer_width);
            const auto from = source + (std::size_t(y) * pixels.width + x) * 4;
            for (unsigned channel = 0; channel < 4; ++channel) {
                memory[destination + channel] = bytes[from + channel];
                written[destination + channel] = true;
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (mode == LevelMode::swizzled4) {
                // PSMT4 sampling of the uploaded words.
                const auto nibble = nibble_address4(x, y, buffer_width);
                const auto address = nibble / 2;
                if (address >= memory.size() || !written[address])
                    throw std::runtime_error("TDX sampling reads outside the uploaded texels");
                emit((memory[address] >> ((nibble & 1) * 4)) & 15);
            } else {
                const auto address = address8(x, y, buffer_width);
                if (address >= memory.size() || !written[address])
                    throw std::runtime_error("TDX sampling reads outside the uploaded texels");
                emit(permute8(memory[address]));
            }
        }
    }
    return rgba;
}

std::vector<std::uint8_t> decode_level(std::span<const std::uint8_t> bytes, const Transfer& pixels, LevelMode mode,
    std::size_t source, std::uint32_t width, std::uint32_t height, std::uint32_t buffer_width,
    std::size_t palette_source, bool alpha, std::vector<std::uint8_t>& raw_alpha) {
    if (reference_levels)
        return decode_level_reference(bytes, pixels, mode, source, width, height, buffer_width, palette_source, alpha, raw_alpha);
    // Port (checkpoint 132win5): the same texels as the per-texel GS model, faster (front-end screen loads).
    // The palette is expanded once (16 or 256 entries as the format addresses), the output is written in
    // place, and the upload's coverage is kept per 32-bit word (the PSMCT32 upload writes whole words).
    const std::size_t count = std::size_t(width) * height;
    std::vector<std::uint8_t> rgba(count * 4);
    raw_alpha.assign(count, 0);
    const bool four = mode == LevelMode::direct4 || mode == LevelMode::swizzled4;
    std::array<std::uint32_t, 256> colours{};
    std::array<std::uint8_t, 256> raws{};
    for (unsigned i = 0; i < (four ? 16U : 256U); ++i) {
        // The 256-entry CSM1 palette swaps index bits 3 and 4; 16 entries do not.
        const unsigned entry = four ? i : (i & ~24U) | ((i & 8) << 1) | ((i & 16) >> 1);
        const auto color = palette_source + entry * 4;
        const std::uint8_t a = alpha ? static_cast<std::uint8_t>(std::min(255U, unsigned(bytes[color + 3]) * 2)) : 255;
        colours[i] = std::uint32_t(bytes[color]) | std::uint32_t(bytes[color + 1]) << 8 |
                     std::uint32_t(bytes[color + 2]) << 16 | std::uint32_t(a) << 24;
        // Without TCC (header +2 alpha flags clear) the GS takes As = 0x80.
        raws[i] = alpha ? bytes[color + 3] : std::uint8_t{0x80};
    }
    std::uint8_t* out = rgba.data();
    std::uint8_t* out_alpha = raw_alpha.data();
    const auto emit = [&](std::size_t i, unsigned index) {
        const std::uint32_t c = colours[index];
        out[4 * i] = static_cast<std::uint8_t>(c);
        out[4 * i + 1] = static_cast<std::uint8_t>(c >> 8);
        out[4 * i + 2] = static_cast<std::uint8_t>(c >> 16);
        out[4 * i + 3] = static_cast<std::uint8_t>(c >> 24);
        out_alpha[i] = raws[index];
    };
    if (mode == LevelMode::direct4) {
        // Equal PSMT4 upload/sample rectangles cancel GS addressing; low nibble first.
        for (std::size_t i = 0; i < count; ++i) emit(i, (bytes[source + i / 2] >> ((i & 1) * 4)) & 15);
        return rgba;
    }
    if (mode == LevelMode::direct8) {
        for (std::size_t i = 0; i < count; ++i) emit(i, bytes[source + i]);
        return rgba;
    }
    std::size_t limit = 0;
    for (std::uint32_t y = 0; y < pixels.height; ++y)
        for (std::uint32_t x = 0; x < pixels.width; ++x) limit = std::max(limit, address32(x, y, pixels.buffer_width) + 4);
    if (limit > gs_memory_size) throw std::runtime_error("TDX upload exceeds GS memory");
    std::vector<std::uint8_t> memory(limit);
    std::vector<std::uint8_t> written(limit / 4 + 1, 0);  // per word: address32 is word-aligned
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        const std::uint8_t* from = bytes.data() + source + std::size_t(y) * pixels.width * 4;
        for (std::uint32_t x = 0; x < pixels.width; ++x, from += 4) {
            const auto destination = address32(x, y, pixels.buffer_width);
            std::memcpy(memory.data() + destination, from, 4);
            written[destination / 4] = 1;
        }
    }
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x, ++i) {
            if (mode == LevelMode::swizzled4) {
                // PSMT4 sampling of the uploaded words.
                const auto nibble = nibble_address4(x, y, buffer_width);
                const auto address = nibble / 2;
                if (address >= memory.size() || !written[address / 4])
                    throw std::runtime_error("TDX sampling reads outside the uploaded texels");
                emit(i, (memory[address] >> ((nibble & 1) * 4)) & 15);
            } else {
                const auto address = address8(x, y, buffer_width);
                if (address >= memory.size() || !written[address / 4])
                    throw std::runtime_error("TDX sampling reads outside the uploaded texels");
                emit(i, memory[address]);
            }
        }
    }
    return rgba;
}
}

namespace {
// Unpaletted PSMCT32 TDX (bpp 32, psm 0; e.g. the LGT_* glow sprites): one
// level, uploaded and sampled as PSMCT32 with the same base and buffer width,
// so GS addressing cancels and the texels are linear RGBA (alpha 0x80 = 1.0).
// PSMCT24 (bpp 24, psm 1; e.g. _BODY_SHU_BLAC0, SCONCE1) is the same with
// three bytes per texel: linear RGB, alpha from TEXA (see below).
Texture decode_tdx_direct(std::span<const std::uint8_t> bytes, bool rgb24) {
    const auto flags = read_u16(bytes, 2);
    if (flags & ~7U) throw std::runtime_error("Unsupported TDX flags");
    if (read_u16(bytes, 28) != 0 || read_u16(bytes, 36) != 1 || read_u16(bytes, 38) != 0)
        throw std::runtime_error("Unsupported TDX mipmaps, animation or transfer count");
    if (read_u32(bytes, 60) != 0) throw std::runtime_error("Relocated TDX data is unsupported");
    if (read_u32(bytes, 16) != 0 || read_u32(bytes, 20) != 0 || read_u32(bytes, 84) != 0)
        throw std::runtime_error("Unsupported palette on a 32-bit TDX");
    const std::uint32_t width = read_u16(bytes, 4);
    const std::uint32_t height = read_u16(bytes, 6);
    if (width < 8 || height < 8 || width > 512 || height > 512
        || !std::has_single_bit(width) || !std::has_single_bit(height))
        throw std::runtime_error("Unsupported TDX dimensions; expected supported powers of two");
    const std::uint32_t buffer_width = read_u16(bytes, 12);
    if (buffer_width == 0 || buffer_width * 64 < width)
        throw std::runtime_error("Unsupported TDX sampling buffer width");
    const auto end = first_frame_end(bytes);
    bytes = bytes.first(end);
    const auto pixel_chain = bounded_offset(std::uint64_t(read_u32(bytes, 88)) + 64, end);
    const auto registers = bounded_offset(std::uint64_t(pixel_chain) + read_u32(bytes, 64), end);
    const auto pixels = transfer(bytes, registers, rgb24 ? 1 : 0);
    if (pixels.width != width || pixels.height != height || pixels.buffer_width != buffer_width
        || read_u16(bytes, 52) != 128)
        throw std::runtime_error("Unsupported PSMCT32/24 transfer geometry or packet size");
    const auto source = bounded_offset(std::uint64_t(pixel_chain) + 128 + pixels.source_offset, end);
    checked_bytes(bytes, source, pixels.bytes);
    if (source < registers + 96 || source + pixels.bytes != end)
        throw std::runtime_error("Unsupported or overlapping TDX payload layout");
    if (rgb24) {
        // 003bba10 sets TCC for a 24-bit TDX only from the object's alpha
        // flags (as the paletted formats: header +2 & 6). The GS then expands
        // the texel alpha through TEXA, which the frame setup 00379960..00379a84
        // writes as 0x80_00008080: TA0 = TA1 = 0x80, AEM = 1, so RGB 0 samples
        // As = 0 and every other texel As = 0x80. Without TCC As is 0x80.
        const bool alpha = (flags & 6) != 0;
        Texture texture{width, height, alpha, {}, {}, read_u16(bytes, 34)};
        texture.rgba.reserve(std::size_t(width) * height * 4);
        for (std::size_t i = source; i < end; i += 3) {
            texture.rgba.insert(texture.rgba.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i),
                                bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
            const bool black = bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0;
            texture.rgba.push_back(alpha && black ? 0 : 255);
        }
        return texture;
    }
    // 003bba10 sets TEX0 TCC for every 32-bit TDX (header +8 == 32), so
    // texel alpha applies whatever the header +2 alpha flags say.
    Texture texture{width, height, true, {}, {}, read_u16(bytes, 34)};
    texture.rgba.assign(bytes.begin() + static_cast<std::ptrdiff_t>(source), bytes.begin() + static_cast<std::ptrdiff_t>(end));
    std::vector<std::uint8_t> raw;
    raw.reserve(texture.rgba.size() / 4);
    bool saturates = false;
    for (std::size_t i = 3; i < texture.rgba.size(); i += 4) {
        raw.push_back(texture.rgba[i]);
        saturates |= texture.rgba[i] > 0x80;
        texture.rgba[i] = static_cast<std::uint8_t>(std::min(255U, unsigned(texture.rgba[i]) * 2));
    }
    if (saturates) texture.raw_alpha.push_back(std::move(raw));
    return texture;
}
}

namespace {
// One frame: the TDX as stored, or a later frame spliced behind the header.
Texture decode_frame(std::span<const std::uint8_t> bytes) {
    checked_bytes(bytes, 0, 64);
    if (read_u16(bytes, 0) != 5) throw std::runtime_error("Unsupported TDX version; expected 5");
    if (read_u16(bytes, 8) == 32 && read_u16(bytes, 10) == 0) return decode_tdx_direct(bytes, false);
    if (read_u16(bytes, 8) == 24 && read_u16(bytes, 10) == 1) return decode_tdx_direct(bytes, true);
    const bool four = read_u16(bytes, 8) == 4 && read_u16(bytes, 10) == 0x14;
    if (!four && (read_u16(bytes, 8) != 8 || read_u16(bytes, 10) != 0x13))
        throw std::runtime_error("Unsupported TDX pixel format");
    // Header +0x1c is the maximum mip level (TEX1 MXL); one pixel transfer per
    // level. Later animation frames are decoded by decode_tdx.
    const unsigned levels = read_u16(bytes, 28) + 1U;
    // A one-level 8-bit picture over 512 KB (the 1024 x 512 ZEUS.HOG slides)
    // uploads in two transfers, the second continuing the first's rows: one
    // IMAGE packet holds at most 0x7fff quadwords.
    const bool split = !four && levels == 1 && read_u16(bytes, 36) == 2;
    if (levels > 7 || (read_u16(bytes, 36) != levels && !split) || read_u16(bytes, 38) != 1)
        throw std::runtime_error("Unsupported TDX mipmaps, animation or transfer count");
    if (read_u32(bytes, 60) != 0) throw std::runtime_error("Relocated TDX data is unsupported");
    if (read_u16(bytes, 16) != (four ? 8 : 16) || read_u16(bytes, 18) != (four ? 2 : 16)
        || read_u16(bytes, 20) != 32 || read_u16(bytes, 22) != 0)
        throw std::runtime_error("Unsupported TDX palette geometry or format");
    const auto flags = read_u16(bytes, 2);
    if (flags & ~7U) throw std::runtime_error("Unsupported TDX flags");
    const std::uint32_t width = read_u16(bytes, 4);
    const std::uint32_t height = read_u16(bytes, 6);
    if (width < (four ? 8U : 16U) || height < (four ? 8U : 16U) || width > (split ? 1024U : 512U) || height > 512
        || !std::has_single_bit(width) || !std::has_single_bit(height))
        throw std::runtime_error("Unsupported TDX dimensions; expected supported powers of two");
    const std::uint32_t buffer_width = read_u16(bytes, 12);
    if (buffer_width < 2 || buffer_width > 32 || buffer_width % 2 || buffer_width * 64 < width)
        throw std::runtime_error("Unsupported TDX sampling buffer width");
    if ((width >> (levels - 1)) == 0 || (height >> (levels - 1)) == 0)
        throw std::runtime_error("Unsupported TDX mip level below one texel");
    // Archive members are padded with zeros to 16 bytes (mission TEX.HOG) or to
    // a 2048-byte sector (COMMON MANTEX.HOG); any nonzero tail is rejected.
    const auto end = first_frame_end(bytes);
    bytes = bytes.first(end);
    const auto pixel_chain = bounded_offset(std::uint64_t(read_u32(bytes, 88)) + 64, end);
    const auto palette_chain = bounded_offset(std::uint64_t(read_u32(bytes, 92)) + 64, end);
    const auto pixel_registers = bounded_offset(std::uint64_t(pixel_chain) + read_u32(bytes, 64), end);
    const auto palette_registers = bounded_offset(std::uint64_t(palette_chain) + read_u32(bytes, 84), end);
    const auto palette = transfer(bytes, palette_registers);
    if (palette.width != (four ? 8U : 16U) || palette.height != (four ? 2U : 16U)
        || palette.buffer_width != 1 || palette.bytes != (four ? 64U : 1024U))
        throw std::runtime_error("Unsupported TDX palette transfer geometry");
    const auto packet_size = read_u16(bytes, 52);
    const auto palette_packet_size = read_u16(bytes, 54);
    if (levels > 1 && (packet_size != 128 * levels || palette_packet_size != 128))
        throw std::runtime_error("Unsupported TDX mip packet size");
    const auto palette_source = bounded_offset(std::uint64_t(palette_chain) + packet_size + palette_packet_size + palette.source_offset, end);
    checked_bytes(bytes, palette_source, palette.bytes);

    Texture texture{width, height, (flags & 6) != 0, {}, {}, read_u16(bytes, 34)};
    std::size_t expected_source = 0;
    std::size_t last_registers = pixel_registers;
    bool truncated = false;
    for (unsigned level = 0; level < levels; ++level) {
        const auto registers = bounded_offset(std::uint64_t(pixel_registers) + std::uint64_t(level) * 128, end);
        last_registers = registers;
        const std::uint32_t w = width >> level, h = height >> level;
        // 4-bit levels upload directly as PSMT4 or as a PSMCT32 rectangle the GS
        // samples as PSMT4; 8-bit levels as a PSMCT32 rectangle sampled as PSMT8,
        // or (small mip levels) directly as PSMT8.
        checked_bytes(bytes, registers, 8);
        const auto upload_psm = (read_u32(bytes, registers + 4) >> 24) & 63;
        LevelMode mode;
        if (four) mode = upload_psm == 0x14 ? LevelMode::direct4 : LevelMode::swizzled4;
        else mode = upload_psm == 0x13 && level > 0 ? LevelMode::direct8 : LevelMode::psmt8_from_32;
        const unsigned expected_psm = mode == LevelMode::direct4 ? 0x14 : mode == LevelMode::direct8 ? 0x13 : 0;
        auto pixels = transfer(bytes, registers, expected_psm, mode == LevelMode::swizzled4);
        if (split) {
            if (mode != LevelMode::psmt8_from_32 || pixels.height * 2 >= h)
                throw std::runtime_error("Unsupported TDX mipmaps, animation or transfer count");
            const auto next_registers = bounded_offset(std::uint64_t(registers) + 128, end);
            checked_bytes(bytes, next_registers, 8);
            const auto next = transfer(bytes, next_registers, expected_psm, false, pixels.height);
            if (read_u32(bytes, next_registers + 4) != read_u32(bytes, registers + 4)
                || next.width != pixels.width || next.source_offset != pixels.source_offset + pixels.bytes)
                throw std::runtime_error("Unsupported TDX split transfer layout");
            pixels.height += next.height;
            pixels.bytes += next.bytes;
            last_registers = next_registers;
        }
        switch (mode) {
        case LevelMode::direct4:
            if (pixels.width != w || pixels.height != h || pixels.bytes != w * h / 2
                || (level == 0 && pixels.buffer_width != buffer_width) || (levels == 1 && (packet_size != 128 || palette_packet_size != 128)))
                throw std::runtime_error("Unsupported direct PSMT4 transfer geometry or packet size");
            break;
        case LevelMode::swizzled4:
            if (pixels.width * 2 != w || pixels.height * 4 != h || pixels.bytes != w * h / 2
                || (levels == 1 && (packet_size != 128 || palette_packet_size != 128)))
                throw std::runtime_error("Unsupported swizzled PSMT4 transfer geometry or packet size");
            break;
        case LevelMode::direct8:
            if (pixels.width != w || pixels.height != h || pixels.bytes != w * h)
                throw std::runtime_error("Unsupported direct PSMT8 mip transfer geometry");
            break;
        case LevelMode::psmt8_from_32:
            if (pixels.width * 2 != w || pixels.height * 2 != h || pixels.bytes != w * h
                || (level == 0 && pixels.buffer_width * 2 != buffer_width))
                throw std::runtime_error("Unsupported TDX upload-to-sampling geometry");
            break;
        }
        // Smaller levels are uploaded and sampled at their own MIPTBP base; a
        // level that fits one GS page decodes identically at a zero base.
        // 003bbb7c sets TEX1 MTBA for every TDX, so the GS derives each mip
        // level's buffer width from TEX0 (TBWn = TBW0 >> n). A level over one
        // page (the 256 x 256 PSMT8 TRAINING walls and MINSK BIGDOOR1, level
        // 1: 128 x 128) is sampled with that width; its PSMCT32 upload's DBW
        // must match it (DBW 1 -> PSMT8 TBW 2), as level 0's does.
        const std::uint32_t level_buffer_width = level == 0 ? buffer_width : std::max(1U, buffer_width >> level);
        if (level > 0 && (w > 128 || h > (four ? 128U : 64U))
            && (mode != LevelMode::psmt8_from_32 || pixels.buffer_width * 2 != level_buffer_width
                || level_buffer_width * 64 < w))
            throw std::runtime_error("Unsupported TDX mip level wider than one GS page");
        const auto source = bounded_offset(std::uint64_t(pixel_chain) + packet_size + 1024 + pixels.source_offset, end);
        checked_bytes(bytes, source, pixels.bytes);
        if (level == 0 ? source < palette_source + palette.bytes : source != expected_source)
            throw std::runtime_error("Unsupported or overlapping TDX payload layout");
        expected_source = source + pixels.bytes;
        // Checkpoint 132cp8: a level the GS receives no texels for samples whatever GS memory holds at its MIPTBP,
        // which the port cannot know; the chain ends at the last uploaded level (its payload bytes stay unread).
        if (!pixels.uploaded) {
            if (level == 0) throw std::runtime_error("Unsupported TDX level 0 without texels");
            truncated = true;
            continue;
        }
        if (truncated) throw std::runtime_error("Unsupported TDX mip level uploaded after an empty one");
        auto& raw = texture.raw_alpha.emplace_back();
        auto rgba = decode_level(bytes, pixels, mode, source, w, h, level_buffer_width, palette_source, texture.uses_alpha, raw);
        if (level == 0) texture.rgba = std::move(rgba); else texture.mips.push_back(std::move(rgba));
    }
    const bool saturates = std::any_of(texture.raw_alpha.begin(), texture.raw_alpha.end(), [](const auto& level) {
        return std::any_of(level.begin(), level.end(), [](std::uint8_t a) { return a > 0x80; });
    });
    if (!saturates) texture.raw_alpha.clear();
    if (palette_source < std::max(last_registers, palette_registers) + 96 || expected_source != end)
        throw std::runtime_error("Unsupported or overlapping TDX payload layout");
    return texture;
}
}

Texture decode_tdx(std::span<const std::uint8_t> bytes) {
    OMEGA_ZONE("load.decode_tdx");  // port-only (132vt2e): the PS Vita's load-time zones
    auto texture = decode_frame(bytes);
    // 003bb3e8..003bb418: the upload of the texture object's frame (+0x4c)
    // starts at header + 0x40 + frame * (+0x38) and reads its packet through
    // the shared header (+0x10.., +0x58/+0x5c chain offsets relative to it).
    const std::size_t length = read_u32(bytes, 56);
    for (std::uint32_t n = 1; n < texture.frames; ++n) {
        std::vector<std::uint8_t> frame(bytes.begin(), bytes.begin() + 64);
        frame[34] = 1;
        frame[35] = 0;
        const auto at = static_cast<std::ptrdiff_t>(64 + std::size_t(n) * length);
        frame.insert(frame.end(), bytes.begin() + at, bytes.begin() + at + static_cast<std::ptrdiff_t>(length));
        auto image = decode_frame(frame);
        if (image.width != texture.width || image.height != texture.height || image.uses_alpha != texture.uses_alpha
            || image.mips.size() != texture.mips.size())
            throw std::runtime_error("TDX animation frame " + std::to_string(n) + " differs in size, alpha or mip levels");
        texture.animation.push_back(std::move(image));
    }
    if (const auto hook = decode_hook.load(std::memory_order_acquire)) {
        hook(texture);
        for (auto& frame : texture.animation) hook(frame);
    }
    return texture;
}

void set_texture_decode_hook(TextureDecodeHook hook) { decode_hook.store(hook, std::memory_order_release); }

Texture decode_tdx_reference(std::span<const std::uint8_t> bytes) {
    reference_levels = true;
    try {
        auto texture = decode_tdx(bytes);
        reference_levels = false;
        return texture;
    } catch (...) {
        reference_levels = false;
        throw;
    }
}

std::vector<std::uint8_t> tdx_in_place(std::span<const std::uint8_t> image, std::size_t offset, std::size_t size,
                                       std::span<const std::size_t> relocated_tdx) {
    if (offset > image.size() || size > image.size() - offset) throw std::runtime_error("TDX member outside its HOG image");
    const auto member = image.subspan(offset, size);
    if (size < 64) return {member.begin(), member.end()};
    const auto upload = 64 + std::uint64_t(read_u16(member, 34)) * read_u32(member, 56);
    if (upload <= size || upload > image.size() - offset) return {member.begin(), member.end()};
    std::vector<std::uint8_t> bytes(image.begin() + static_cast<std::ptrdiff_t>(offset),
                                    image.begin() + static_cast<std::ptrdiff_t>(offset + upload));
    for (const auto header : relocated_tdx)
        for (std::size_t k = 0; k < 4; ++k) {
            const auto at = header + 0x3c + k;
            if (at >= offset + size && at < offset + upload) bytes[at - offset] = k == 0 ? 1 : 0;
        }
    return bytes;
}

Texture decode_tdx8(std::span<const std::uint8_t> bytes) {
    checked_bytes(bytes, 0, 64);
    if (read_u16(bytes, 8) != 8 || read_u16(bytes, 10) != 0x13)
        throw std::runtime_error("Unsupported TDX pixel format; expected 8-bit PSMT8");
    return decode_tdx(bytes);
}

std::vector<std::uint8_t> composite_bgra(const Texture& texture, bool checkerboard) {
    if (texture.width == 0 || texture.height == 0
        || std::uint64_t(texture.width) * texture.height * 4 != texture.rgba.size())
        throw std::runtime_error("Invalid decoded texture dimensions");
    std::vector<std::uint8_t> result(texture.rgba.size());
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            const auto offset = (std::size_t(y) * texture.width + x) * 4;
            const unsigned background = checkerboard ? (((x / 8 + y / 8) & 1) ? 88U : 56U) : 0U;
            const unsigned alpha = texture.rgba[offset + 3];
            for (unsigned channel = 0; channel < 3; ++channel)
                result[offset + channel] = static_cast<std::uint8_t>(
                    (texture.rgba[offset + 2 - channel] * alpha + background * (255 - alpha) + 127) / 255);
            result[offset + 3] = 255;
        }
    }
    return result;
}

std::vector<std::uint8_t> encode_bmp(std::uint32_t width, std::uint32_t height,
                                    std::span<const std::uint8_t> bgra) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384
        || std::uint64_t(width) * height * 4 != bgra.size()
        || bgra.size() > std::numeric_limits<std::uint32_t>::max() - 54)
        throw std::runtime_error("Invalid BMP dimensions or data");
    std::vector<std::uint8_t> output(54);
    output[0] = 'B';
    output[1] = 'M';
    put32(output, 2, static_cast<std::uint32_t>(bgra.size() + 54));
    put32(output, 10, 54);
    put32(output, 14, 40);
    put32(output, 18, width);
    put32(output, 22, 0U - height);
    output[26] = 1;
    output[28] = 32;
    put32(output, 34, static_cast<std::uint32_t>(bgra.size()));
    output.insert(output.end(), bgra.begin(), bgra.end());
    return output;
}

void write_bmp(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> bgra) {
    write_bytes(path, encode_bmp(width, height, bgra));
}

}
