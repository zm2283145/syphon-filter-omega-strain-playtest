#pragma once
// Port-only (checkpoint 132md1, research/MODDING.md): texture replacement. A mod's image replaces the decoded TDX
// frame it is keyed to (by archive path + member, or by content hash) in the renderer only: the Texture keeps the
// original's width, height, pixels, alpha flag and mip levels (every CPU-side reader, UV normalisation by the original
// size, and gameplay stay the original's); Texture::replacement holds the image the GPU upload uses instead
// (texture.hpp upload_image). Alpha semantics: the decoded TDX alpha is min(255, 2 * As) (GS 0x80 = 1.0), so a
// replacement's alpha 255 is As 1.0; As > 1.0 (Texture::raw_alpha) cannot be expressed: when the original has
// raw_alpha the replacement's is (a + 1) / 2 per texel (255 -> 0x80).

#include "image_files.hpp"
#include "mod_set.hpp"
#include "texture.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace omega::mods {

// FNV-1a 64 over the frame's width and height (u32, little-endian) and its decoded RGBA8 base level.
std::uint64_t texture_content_hash(const Texture& texture);
// The replacement Texture for `image` standing in for `original`: RGBA as given, a full box-filtered mip chain
// (down to 1 x 1), uses_alpha as the original's, raw_alpha as described above.
Texture replacement_texture(const RgbaImage& image, const Texture& original);
// Maps the content hashes of the texture members (and nested HOG members) below `archive_key` that have a path-keyed
// replacement, decoding them from `archive` (the archive's resolved bytes). Once per archive key per installation.
// Port-only (checkpoint 132md8): the replacement images are not decoded here; attach_replacement decodes one when a
// decode first references it (other callers of the same image wait for it), so a level decodes only what it draws.
void register_texture_paths(const ModSet& set, const std::string& archive_key, std::span<const std::uint8_t> archive);
// The decode hook (texture.hpp set_texture_decode_hook): attaches the replacement registered for the frame's hash.
void attach_replacement(Texture& texture);
// Port-only (checkpoint 132md8): a batch of decodes with the replacements decoded across threads. Decode the
// originals (decode_tdx) with a WithoutReplacements on the thread, pass them to decode_replacements_ahead, then call
// attach_replacements on each in order: the attached images, the log lines and their order are the hook's.
struct WithoutReplacements {
    WithoutReplacements();
    ~WithoutReplacements();
    WithoutReplacements(const WithoutReplacements&) = delete;
    WithoutReplacements& operator=(const WithoutReplacements&) = delete;
    bool previous;
};
// Decodes across threads the replacement images of these frames (an original and its animation frames) that are not
// decoded yet; attach_replacement takes them, and notes them, when it first references them.
void decode_replacements_ahead(std::span<const Texture* const> originals);
// What the hook does after decode_tdx: attach_replacement on the texture, then on each of its animation frames.
void attach_replacements(Texture& texture);
// Clears the registered hashes and the image cache (mods::install). The memory cap stays.
void reset_texture_replacements();

// Port-only (checkpoint 132md9, research/MODDING.md "Texture memory"): a cap on the memory of the replacement images a
// level references, for low-memory platforms. Off (0) on PC; a build for a low-memory platform defaults to
// default_texture_memory_cap_mb (the port setting mods_texture_cap=<MB>, --mods-texture-cap <MB>). Each image counts
// as uploaded: RGBA8 with its full mip chain (replacement_bytes). With a cap, decode_replacements_ahead (the level's
// texture binding) plans one side limit for the level's images (plan_texture_cap): every image wider or taller than
// it is halved (box filter) until it fits, but never below its original's size; the largest power of two whose total
// fits the cap is taken. The images decoded later in the level take the same limit. Without a cap nothing changes.
// Port-only (checkpoint 132md11, research/MODDING.md "Mods on the Vita"): a BC1 / BC3 DDS with its full mip chain (what
// omega_mod_tool pack --platform vita writes) is kept as blocks (Texture::block_bytes / blocks; nothing decoded) and
// counts as its blocks (4 / 8 bits a texel); the cap halves it by dropping its top levels. It is decoded to RGBA8 (and
// counts as that) when its original has raw alpha or a level is missing. A device without Capabilities::
// block_textures decodes the blocks at the upload (gfx::upload_levels): its GPU then holds RGBA8.
#if defined(__vita__)
inline constexpr std::uint32_t default_texture_memory_cap_mb = 64;
#else
inline constexpr std::uint32_t default_texture_memory_cap_mb = 0;
#endif
void set_texture_memory_cap(std::uint64_t bytes);
std::uint64_t texture_memory_cap();
// The bytes of an RGBA8 image of this size with its mip chain down to 1 x 1; 132md11: of BC1 / BC3 blocks with
// block_bytes 8 / 16.
std::uint64_t replacement_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t block_bytes = 0);
// 132md11: the bytes a replacement Texture holds for its upload (its RGBA levels, or its blocks).
std::uint64_t texture_bytes(const Texture& texture);
struct CapItem {
    std::uint32_t width{}, height{};          // the replacement image's
    std::uint32_t min_width{}, min_height{};  // its original's (the floor of the downscale)
    std::uint32_t block_bytes{};              // 132md11: a BC1 / BC3 DDS kept as blocks (8 / 16), else 0 (RGBA8)
};
// The size an image gets under a side limit (0: none): halved while a side exceeds it and both halves stay at or above
// the original's.
void capped_size(const CapItem& item, std::uint32_t limit, std::uint32_t& width, std::uint32_t& height);
struct CapPlan {
    std::uint32_t limit = 0;          // 0: every image at its full size
    std::uint64_t full_bytes = 0;     // the images at their full size
    std::uint64_t planned_bytes = 0;  // under the limit
    bool fits = true;                 // false: over the cap even at the originals' sizes (limit is then 1)
};
CapPlan plan_texture_cap(std::span<const CapItem> items, std::uint64_t cap_bytes);
// Port-only (checkpoint 132md10): the cap covers the rest of a mod's images too.
//   - A model's glTF image (model_overrides.hpp): without a cap, replacement_texture(image, base) as before; with one,
//     halved as an image decoded outside a level's batch is (the level plan's side limit, further while the images
//     held and this one exceed the cap), never below `base` (the part's disc texture); counted while it is held.
//   - A texture inside a files\ replacement (a .TDX, or one in a replaced .HOG, nested ones too) whose frames differ
//     from the disc's: register_file_textures (the disc layer, only with a cap) notes them by content hash; the level
//     plan counts them, and the decode hook attaches their own pixels halved past the limit (never below the disc
//     frame's size) as the replacement the GPU draws. The Texture keeps its decoded size and pixels for every CPU
//     reader, so the cap saves texture (GPU) memory there, not the file's bytes.
std::shared_ptr<const Texture> capped_replacement_texture(const RgbaImage& image, const Texture& base);
// `replaced`: the resolved bytes of disc file `key` (mods::resolve_bytes); `disc`: the disc's own. A no-op without a cap.
void register_file_textures(const std::string& key, std::span<const std::uint8_t> replaced, std::span<const std::uint8_t> disc);
// One decoded texture of a files\ replacement and the disc's (nullptr: none): its frames that differ, as above.
void register_file_texture(const std::string& label, const Texture& replaced, const Texture* disc);

// Port-only (checkpoint 132md11, research/MODDING.md "Mods on the Vita"): a mod texture converted for a GXM device
// (omega_mod_tool pack --platform vita): each side the largest power of two at or below it (at least 4; GXM's
// swizzled textures are power-of-two), halved while either exceeds max_side, resampled by area averaging (none when
// the size stays); its full mip chain as replacement_texture builds it (box filter); BC1 when every texel is opaque,
// else BC3; as a DDS (encode_dds). Deterministic.
struct PlatformTexture {
    std::vector<std::uint8_t> dds;
    std::uint32_t width{}, height{}, block_bytes{};
};
PlatformTexture platform_texture(const RgbaImage& image, std::uint32_t max_side);
// Area-average resampling of an RGBA8 image to width x height (each output texel the mean of the input area it covers).
RgbaImage resample_image(const RgbaImage& image, std::uint32_t width, std::uint32_t height);

// Port-only (checkpoint 132md12, research/MODDING.md "Wide backgrounds"): a mod's native wide backgrounds for a screen
// key (ModSet::wide_background: "loading.toronto1", "frontend.<screen>", their kinds), each image decoded once per
// installation (a BC1 / BC3 DDS to RGBA8) with its mip chain, halved by the memory cap as a model image is
// (capped_replacement_texture, never below the PS2 frame's 640 x 448); a first-use note per image. nullptr without
// mods or without an entry for the screen (nothing read); an image that cannot be decoded is left out (a note).
// Thread-safe (the loading screen's thread).
struct WideImage {
    std::shared_ptr<const Texture> texture;
    std::uint32_t width{}, height{};   // the file's size (its aspect)
};
std::shared_ptr<const std::vector<WideImage>> wide_background_images(std::string_view screen);
// The image whose aspect is nearest the target's (log ratio; ties: the wider) among those wider than 4:3; nullptr when
// the width x height target is not wider than 4:3 or no image is.
const WideImage* pick_wide_background(std::span<const WideImage> images, unsigned width, unsigned height);
// Where an image goes in a width x height target, in pixels: the target's height at the image's own aspect, centred
// (x < 0: wider than the target, its sides cut; narrower: the target's sides past it are not drawn).
struct WideRect {
    double x{}, y{}, width{}, height{};
};
WideRect wide_background_rect(std::uint32_t image_width, std::uint32_t image_height, unsigned width, unsigned height);

}  // namespace omega::mods
