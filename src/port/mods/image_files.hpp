#pragma once
// Port-only (checkpoint 132md1, research/MODDING.md): the image files a mod supplies for texture replacements and the
// PNGs the mod tool exports. Self-contained (no system codec), so the same code serves later platforms:
//   - PNG: 8-bit grey, grey + alpha, RGB, RGBA and palette (1/2/4/8-bit, tRNS) and 16-bit grey / RGB / RGBA (the high
//     byte kept), non-interlaced; Adam7 interlacing is refused;
//   - DDS: uncompressed 32-bit images (legacy DDPF_RGB with an alpha mask, or a DX10 header with
//     DXGI_FORMAT_R8G8B8A8_UNORM / B8G8R8A8_UNORM); (checkpoint 132md11) BC1 / BC3 (fourcc DXT1 / DXT5, or a DX10
//     header with BC1_UNORM(_SRGB) / BC3_UNORM(_SRGB)), decoded by gfx/texture_blocks.hpp, or kept as blocks
//     (dds_blocks: the mod textures omega_mod_tool pack --platform vita writes, encode_dds);
//   - the PNG writer stores 8-bit RGBA with zlib "stored" blocks (valid, uncompressed).
// Every decoder returns straight RGBA8 rows, top row first. Errors throw std::runtime_error.
// The inflate (132md8) decodes through a 10-bit Huffman table with a 64-bit bit buffer (codes past 10 bits walk the
// canonical code) and the PNG reader unfilters one loop per filter type: about 3x fewer cycles than the bit-by-bit
// reader on the HD test pack, with identical output (research/MODDING.md "Speed").

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::mods {

struct RgbaImage {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;  // width * height * 4
};

// The largest replacement accepted (each side): D3D11's 2D texture limit is 16384.
inline constexpr std::uint32_t image_size_limit = 8192;

// zlib (RFC 1950) stream inflate; `limit` bounds the output.
std::vector<std::uint8_t> zlib_inflate(std::span<const std::uint8_t> stream, std::size_t limit);
// Port-only (checkpoint 132md10): a raw RFC 1951 stream (no zlib header), as in a zip member.
std::vector<std::uint8_t> inflate_raw(std::span<const std::uint8_t> stream, std::size_t limit);
std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);

RgbaImage decode_png(std::span<const std::uint8_t> bytes);
RgbaImage decode_dds(std::span<const std::uint8_t> bytes);
// By the file's signature (PNG or "DDS ").
RgbaImage decode_image(std::span<const std::uint8_t> bytes);
RgbaImage read_image_file(const std::filesystem::path& path);
// Port-only (checkpoint 132md11): the file's bytes (an archive member when inside a registered mod archive).
std::vector<std::uint8_t> read_image_bytes(const std::filesystem::path& path);
// A BC1 / BC3 image's levels as stored (base first, each half the previous size; block_bytes 8 / 16).
struct BlockImage {
    std::uint32_t width{}, height{}, block_bytes{};
    std::vector<std::vector<std::uint8_t>> levels;
};
// A block-compressed DDS's levels (its mip count, at least 1); nullopt for any other image. Throws when truncated.
std::optional<BlockImage> dds_blocks(std::span<const std::uint8_t> bytes);
// A DDS (legacy header, fourcc DXT1 / DXT5, the mip count) of `image`.
std::vector<std::uint8_t> encode_dds(const BlockImage& image);
// Port-only (checkpoint 132md9): the width and height from a PNG's IHDR or a DDS header (the first bytes of the file;
// nothing decoded or checked past them). Throws when the file is neither or its header is short.
// 132md11: block_bytes 8 / 16 for a BC1 / BC3 DDS (its level 0 then takes block bytes per 4 x 4 texels), else 0.
struct ImageSize {
    std::uint32_t width{}, height{};
    std::uint32_t block_bytes{};
};
ImageSize image_size(std::span<const std::uint8_t> header);
ImageSize read_image_size(const std::filesystem::path& path);

std::vector<std::uint8_t> encode_png(const RgbaImage& image);
void write_png(const std::filesystem::path& path, const RgbaImage& image);

}  // namespace omega::mods
