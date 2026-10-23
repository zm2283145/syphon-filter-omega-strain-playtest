#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace omega {

struct Texture {
    std::uint32_t width{};
    std::uint32_t height{};
    bool uses_alpha{};
    std::vector<std::uint8_t> rgba;
    // RGBA of mip levels 1..n (each half the previous size), when present.
    std::vector<std::vector<std::uint8_t>> mips;
    // Animation frames in the TDX (header +0x22); rgba/mips hold frame 0.
    std::uint32_t frames{1};
    // Frames 1..frames-1 (each with frames == 1), as the texture object's
    // upload 003bb3f0 sends frame N from +0x40 + N * (+0x38).
    std::vector<Texture> animation;
    // The GS texel alpha (0x80 = 1.0) of each level (base, then mips), kept
    // only when a sampled texel exceeds 0x80: rgba stores min(255, 2a), which
    // loses As > 1.0 where the blend uses it unclamped (ALPHA 0x48 Cs*As + Cd,
    // 0x42 Cd - Cs*As). Empty otherwise.
    std::vector<std::vector<std::uint8_t>> raw_alpha;
    // Port-only (checkpoint 132md1, research/MODDING.md): a mod's replacement image of this frame, which the GPU
    // uploads use instead of rgba / mips / raw_alpha (upload_image). The fields above stay the original's (sizes, UV
    // normalisation and every CPU-side reader unchanged). Null without mods.
    std::shared_ptr<const Texture> replacement;
    // Port-only (checkpoint 132md11, research/MODDING.md "Mods on the Vita"): a replacement read from a BC1 / BC3 DDS
    // (block_bytes 8 / 16) keeps its GPU blocks instead of RGBA: `blocks` holds the base level and each mip (rgba and
    // mips empty); gfx/texture_blocks.hpp upload_levels uploads or decodes them. 0 / empty otherwise.
    std::uint32_t block_bytes{};
    std::vector<std::vector<std::uint8_t>> blocks;
};

// The texture a renderer uploads: the mod replacement when there is one, else the texture itself.
inline const Texture& upload_image(const Texture& texture) { return texture.replacement ? *texture.replacement : texture; }
// Port-only (checkpoint 132md1): called by decode_tdx on the decoded frame 0 and on each animation frame (the mod
// layer's texture replacement attaches Texture::replacement). Null (no call) without mods.
using TextureDecodeHook = void (*)(Texture&);
void set_texture_decode_hook(TextureDecodeHook hook);

Texture decode_tdx(std::span<const std::uint8_t> bytes);
// Checkpoint 132cp8: the bytes the GS upload of a TDX member read in place
// from an in-memory HOG image reaches (Tdx_Read 00380370 -> 0036d990 ->
// 0036d4e0 returns image + member offset without a copy; the REF packet
// reads +0x40 + frames * (+0x38) bytes at draw time). A member the archiver
// cut short of that length continues into the following image bytes;
// `relocated_tdx` lists the image offsets of the TDX headers Tdx_Relocate
// 003803f0 has run on by then, whose +0x3c word it set to 1. Other members
// are returned unchanged.
std::vector<std::uint8_t> tdx_in_place(std::span<const std::uint8_t> image, std::size_t offset, std::size_t size,
                                       std::span<const std::size_t> relocated_tdx = {});
Texture decode_tdx8(std::span<const std::uint8_t> bytes);
// Test reference (checkpoint 132win5): decode_tdx through the per-texel GS model it was optimised from.
Texture decode_tdx_reference(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> composite_bgra(const Texture& texture, bool checkerboard);
std::vector<std::uint8_t> encode_bmp(std::uint32_t width, std::uint32_t height,
                                    std::span<const std::uint8_t> bgra);
void write_bmp(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> bgra);

}
