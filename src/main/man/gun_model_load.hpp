#pragma once
// man.cc gun models (Checkpoint 121g): the constructor 0038d530, the HOG loader 0038cd90 and
// the bounds 0038d720 (the +3e0 / +3f0 box the backpack hotbox 00253fb0 returns). 0038d720
// runs the VU0 position-query program (microcode set 1) on every LOD 1 chunk; the goldens
// (tests/gun_model_bounds_goldens.hpp) run the original code with the VU0 micro-mode model
// tools/ee/ee_vu0.py. See research/COMBAT.md "Checkpoint 121g".
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// The model object (0x420 bytes) with per-byte definedness: a byte the original code leaves
// as the allocation (or the loader's stack) left it is undefined. Pointer words (+400 and the
// LOD records' +0 / +8 at +2c0 + 16 i) hold offsets into the HOG given to the loader.
struct GunModel {
    static constexpr std::size_t size = 0x420;
    std::array<std::uint8_t, size> bytes{};
    std::array<bool, size> defined{};
    std::uint32_t word(std::size_t offset) const;      // throws when a byte is undefined
    float real(std::size_t offset) const;               // throws when a byte is undefined
    bool all_defined(std::size_t offset, std::size_t count) const;
};

// A gun HOG (tag 0x403429c1 is not checked by the original): the bytes and, optionally, their
// definedness (empty = all defined). Offsets are relative to the HOG base, which the original
// keeps 16-aligned (lq reads align down to 16).
struct GunHog {
    std::span<const std::uint8_t> bytes;
    std::span<const bool> defined;
};

// The texture services the loader calls; their results go only to +3c0..+3df.
struct GunTextureHost {
    virtual ~GunTextureHost() = default;
    virtual void tinit() = 0;                                           // 0036e5d0
    virtual void tload(const std::string& member0) = 0;                 // 0036e1b0(ctx, member 0 name, reader, 0)
    virtual std::int16_t tex(const std::array<std::uint8_t, 16>& name,
                             const std::array<bool, 16>& defined) = 0;  // 00380d40(*00539248, name, ctx)
    virtual std::int16_t texd() = 0;                                    // 00381000(*00539248)
    virtual void tfree() = 0;                                           // 0036e220(ctx, -1)
};

// One DMA channel 0 transfer of a LOD 1 chunk by 0038d720: MADR (HOG offset) and QWC.
struct GunModelUpload { std::uint32_t hog_offset{}, qwords{}; std::uint16_t entry{}; };   // entry: the chunk's MSCAL

// The record a3 of 0038d530 points at: a shells model's LOD 0 part record (00147880), whose
// word +0 (the chunk start) and halfword +4 (the REF tag's QWC) become +404 / +408.
struct GunShellRecord { std::uint32_t chunk_start{}; std::uint16_t qwords{}; };

// 0038d530(model, name, index, a3, t0, t1, t2, t3): +404 = 0, or with a3 +404 = *a3 and
// +408 = *(a3 + 4) (without a3 +408 is left as the allocation had it).
void gun_model_construct(GunModel& model, std::string_view name, std::int16_t index,
                         std::uint8_t t0, std::uint8_t t1, std::uint8_t t2, std::uint8_t t3,
                         const std::optional<GunShellRecord>& a3 = std::nullopt);

// 0038cd90(model, hog), including 0038d720. Without a texture host +3c0..+3df are left
// undefined (the texture services are not hosted). Returns the 0038d720 chunk uploads.
std::vector<GunModelUpload> gun_model_load(GunModel& model, const GunHog& hog,
                                           GunTextureHost* textures);

// 0038d720(model) alone, on a model the loader filled.
std::vector<GunModelUpload> gun_model_bounds(GunModel& model, const GunHog& hog);

}
