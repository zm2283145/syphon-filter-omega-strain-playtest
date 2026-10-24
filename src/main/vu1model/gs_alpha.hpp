#pragma once

#include <cstdint>
#include <optional>

namespace omega {

// GS ALPHA_1 register: Cv = (A - B) * C >> 7 + D, with A/B/D selecting Cs
// (0), Cd (1) or zero (2) and C selecting As (0), Ad (1) or FIX (2); FIX is
// bits 32..39 (0x80 = 1.0).
struct GsAlpha {
    std::uint8_t a{}, b{}, c{}, d{}, fix{};
};

GsAlpha decode_gs_alpha(std::uint64_t value);

// Host blend equations for the model render states found on the disc. The
// factor is As (source alpha) or FIX / 128 (constant blend factor).
enum class GsBlend {
    source_alpha_over,      // (Cs - Cd) * As + Cd   (0x44, renderer default 004bc1b0)
    source_alpha_add,       // Cs * As + Cd          (0x48)
    source_alpha_subtract,  // Cd - Cs * As          (0x42)
    fix_over,               // (Cs - Cd) * FIX + Cd  (0x64)
    fix_add,                // Cs * FIX + Cd         (0x68)
    fix_subtract,           // Cd - Cs * FIX         (0x62)
};

struct GsBlendState {
    GsBlend blend{};
    float fix{1};  // FIX / 128
};

// Throws naming the register value for selections without a host mapping.
GsBlendState gs_blend_state(std::uint64_t alpha);
// The same mapping, empty for selections without one.
std::optional<GsBlendState> try_gs_blend_state(std::uint64_t alpha);

// Later material passes (132ws7). 00377270 patches each pass section's
// TEX0_1 (texture +0x50) and MSCAL (00538080[shader]) only; VU1 programs
// 0x156 (shader 2), 0x4f8 (10 / 11), 0x57a (12) and 0x5cd (14) copy the
// section's GIF tag, TEX1_1, TEX0_1 and ALPHA_1 (qwords 2..5 at TOPS) with
// lq / sq into the output buffer at +283 and XGKICK it, so the GS blends
// each pass with the section's ALPHA_1 as stored in the file
// (tools/ee/check_later_pass_state.py). Empty for other shaders, a missing
// ALPHA_1 or a selection without a host mapping.
std::optional<GsBlendState> later_pass_blend(std::uint32_t shader, std::optional<std::uint64_t> alpha);

// One 8-bit colour channel of the GS blend with COLCLAMP = 1 (the model
// draws, 0037c350): ((A - B) * C >> 7) + D clamped to 0..255, C being As or
// FIX (0x80 = 1.0; As up to 0xff); the product shifted arithmetically (a
// negative A - B rounds toward minus infinity).
std::uint8_t gs_blend_channel(std::uint64_t alpha, std::uint8_t cs, std::uint8_t as, std::uint8_t cd);
// The source term of 0x48 (Cs * As >> 7) or 0x42 (-((0 - Cs) * As >> 7),
// i.e. Cs * As / 128 rounded up) limited to 255. Because 0 <= Cd <= 255,
// clamp(S + Cd) == clamp(min(255, S) + Cd) and clamp(Cd - S) == clamp(Cd -
// min(255, S)), so a host drawing this colour with blend ONE/ONE (add or
// reverse subtract) on an 8-bit target keeps the GS saturation for As > 0x80
// (the host blends in float: its rounding of S is the usual LSB difference).
std::uint8_t gs_premultiplied_source(std::uint64_t alpha, std::uint8_t cs, std::uint8_t as);

}
