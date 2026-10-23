#pragma once
// Port-only (checkpoint 132md11, research/MODDING.md "Mods on the Vita", RENDERER.md "Block-compressed textures"):
// BC1 / BC3 (DXT1 / DXT5) 4x4 block compression for mod textures packed for a low-memory platform
// (omega_mod_tool pack --platform vita), and the levels a renderer uploads for a Texture.
//   - BC1: 8 bytes a block (two RGB565 end points, 2-bit indices; 4 bits a texel); BC3: 16 bytes (an alpha block of two
//     8-bit end points and 3-bit indices, then a BC1 colour block; 8 bits a texel). Direct3D's BC1 / BC3, GXM's UBC1 /
//     UBC3, OpenGL's S3TC DXT1 / DXT5: the same bytes (blocks in row order; a GXM backend tiles them at upload).
//   - The encoder: per block, the colours' principal axis (power iteration) gives the end points, one least-squares
//     refit of them from the chosen indices is kept when its error is lower; BC1 blocks are always four-colour
//     (opaque); BC3 alpha takes the 8-value or the 6-value (with 0 and 255) mode, whichever errs less. Texels past
//     the image's edge repeat its last column / row. Deterministic.
//   - The decoder follows the D3D formulas (565 expanded by bit replication; 2/3 + 1/3 interpolation rounded down;
//     a BC1 block with colour0 <= colour1 is three-colour with index 3 transparent black).
// A Texture with block_bytes != 0 (texture.hpp) holds its levels in `blocks`; upload_levels uploads them as
// gfx::Format::bc1 / bc3 on a device whose capabilities() has block_textures, else decodes them to RGBA8.

#include "gfx_device.hpp"
#include "texture.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omega::gfx {

inline constexpr unsigned bc1_block_bytes = 8, bc3_block_bytes = 16;

// Bytes of one level / one row of blocks of a width x height image (blocks of 4 x 4; partial ones whole).
std::size_t block_level_bytes(unsigned width, unsigned height, unsigned block_bytes);
unsigned block_row_pitch(unsigned width, unsigned block_bytes);
// `rgba`: width * height * 4 bytes, rows top first. block_bytes: bc1_block_bytes or bc3_block_bytes.
std::vector<std::uint8_t> encode_blocks(std::span<const std::uint8_t> rgba, unsigned width, unsigned height, unsigned block_bytes);
// The RGBA8 texels (width * height * 4); throws when `blocks` is shorter than the level.
std::vector<std::uint8_t> decode_blocks(std::span<const std::uint8_t> blocks, unsigned width, unsigned height, unsigned block_bytes);

// What a renderer passes to Device::create_texture for `texture` (normally upload_image(source)): RGBA8 rows of its base
// level and (with_mips) its mips, or (block_bytes != 0) its blocks as Format::bc1 / bc3 on a block_textures device,
// else those decoded to RGBA8 here (`decoded` holds them). Throws when a level's size does not match.
struct UploadLevels {
    TextureDesc desc;
    std::vector<Level> levels;
    std::vector<std::vector<std::uint8_t>> decoded;
};
UploadLevels upload_levels(const Device* device, const omega::Texture& texture, bool with_mips);

}  // namespace omega::gfx
